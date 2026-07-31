#include "engine/sync/midiclockgenerator.h"

namespace {
// MIDI clock is defined as 24 pulses per quarter note, regardless of
// time signature.
constexpr int kPulsesPerQuarterNote = 24;

constexpr unsigned char kMidiTimingClock = 0xF8;
constexpr unsigned char kMidiStart = 0xFA;
constexpr unsigned char kMidiContinue = 0xFB;
constexpr unsigned char kMidiStop = 0xFC;

// Guard against divide-by-zero / runaway loops if a BPM of 0 or a
// tiny sample rate ever reaches us (e.g. no track loaded on the
// tempo source deck yet).
constexpr double kMinBpm = 1.0;
constexpr double kMaxBpm = 400.0;
} // namespace

MidiClockGenerator::MidiClockGenerator()
        : m_enabled(false),
          m_sendTransport(true),
          m_bpm(120.0),
          m_playing(false),
          m_realignRequested(true),
          m_wasEnabled(false),
          m_wasPlaying(false),
          m_samplesUntilNextTick(0.0),
          m_lastSamplesPerTick(0.0) {
}

void MidiClockGenerator::setEnabled(bool enabled) {
    m_enabled.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        requestRealign();
    }
}

void MidiClockGenerator::setBpm(double bpm) {
    if (bpm < kMinBpm) {
        bpm = kMinBpm;
    } else if (bpm > kMaxBpm) {
        bpm = kMaxBpm;
    }
    m_bpm.store(bpm, std::memory_order_relaxed);
}

void MidiClockGenerator::setPlaying(bool playing) {
    m_playing.store(playing, std::memory_order_relaxed);
}

void MidiClockGenerator::setSendTransport(bool sendTransport) {
    m_sendTransport.store(sendTransport, std::memory_order_relaxed);
}

void MidiClockGenerator::requestRealign() {
    m_realignRequested.store(true, std::memory_order_relaxed);
}

void MidiClockGenerator::process(int sampleRate,
        int numFrames,
        std::chrono::steady_clock::time_point bufferStartTime) {
    const bool enabled = m_enabled.load(std::memory_order_relaxed);
    if (!enabled) {
        m_wasEnabled = false;
        return;
    }

    if (sampleRate <= 0 || numFrames <= 0) {
        return;
    }

    const bool playing = m_playing.load(std::memory_order_relaxed);
    const bool sendTransport = m_sendTransport.load(std::memory_order_relaxed);
    const bool realign = m_realignRequested.exchange(false, std::memory_order_relaxed);

    // Handle transport edges (Start/Continue/Stop) and (re)alignment.
    // We emit these at the very start of the buffer (offset 0 samples)
    // since transport messages are not expected to be sample-locked
    // the way clock ticks are.
    if (!m_wasEnabled || realign) {
        m_samplesUntilNextTick = 0.0; // emit a tick immediately -> phase 0
        if (sendTransport && playing) {
            m_queue.push({bufferStartTime, kMidiStart});
        }
    } else if (sendTransport && playing != m_wasPlaying) {
        m_queue.push({bufferStartTime, playing ? kMidiStart : kMidiStop});
        if (playing) {
            // Re-align phase so the first tick after Continue lands
            // musically on the beat rather than wherever we happened
            // to stop mid-period.
            m_samplesUntilNextTick = 0.0;
        }
    }

    m_wasEnabled = true;
    m_wasPlaying = playing;

    // If transport is required and we're stopped, don't emit clock
    // ticks at all (this matches how hardware sequencers/DAWs behave:
    // clock only runs while playing). If the user disabled transport
    // messages entirely, the clock free-runs regardless of play
    // state, which is useful for gear you start/stop by hand.
    if (sendTransport && !playing) {
        return;
    }

    const double bpm = m_bpm.load(std::memory_order_relaxed);
    const double samplesPerTick =
            (60.0 * sampleRate) / (bpm * kPulsesPerQuarterNote);
    m_lastSamplesPerTick = samplesPerTick;

    // Walk through this buffer, sample by sample-period, emitting a
    // tick every time the phase accumulator crosses zero. Because we
    // track the remainder in samples (not seconds), tempo changes
    // that happen between buffers never accumulate rounding error --
    // each tick's timestamp is exact for the sample rate in effect
    // when it was computed.
    double samplesRemainingInBuffer = static_cast<double>(numFrames);
    double cursor = 0.0; // samples into this buffer

    while (true) {
        if (m_samplesUntilNextTick > samplesRemainingInBuffer) {
            m_samplesUntilNextTick -= samplesRemainingInBuffer;
            break;
        }
        cursor += m_samplesUntilNextTick;
        samplesRemainingInBuffer -= m_samplesUntilNextTick;

        const double secondsIntoBuffer = cursor / sampleRate;
        const auto tickTime = bufferStartTime +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(secondsIntoBuffer));
        m_queue.push({tickTime, kMidiTimingClock});

        // Recompute in case BPM changed intra-buffer (it won't in
        // practice since setBpm() is called at most once per engine
        // callback from the main thread, but this keeps behavior
        // correct if that ever changes).
        m_samplesUntilNextTick = samplesPerTick;
    }
}
