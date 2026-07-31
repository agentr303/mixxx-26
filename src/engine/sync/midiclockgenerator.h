#pragma once

// MidiClockGenerator
//
// Produces a sample-accurate stream of MIDI Real-Time clock bytes
// (0xF8 Clock / 0xFA Start / 0xFB Continue / 0xFC Stop), derived from
// the tempo of whichever deck (or the EngineSync leader) the user has
// selected as the clock's tempo source.
//
// WHY THIS EXISTS (vs. the existing "MIDI for light" JS script):
// The existing feature runs in a QJSEngine-driven controller script and
// emits its clock ticks from a QTimer. Qt timers are coalesced, run on
// the GUI/script-engine thread, and have jitter on the order of many
// milliseconds under load -- fine for driving disco lights, not
// accurate enough to be used as a sync clock for a DAW, groovebox, or
// modular rig. MIDI clock at 120 BPM ticks every ~20.8 ms; you need
// sub-millisecond jitter to be perceived as "solid" by downstream gear.
//
// This class is instead driven once per audio buffer from the
// engine callback thread (EngineMixer::process()), which is the only
// place in Mixxx that has a hard real-time relationship with the
// soundcard clock. Every tick is timestamped against
// std::chrono::steady_clock at the moment its owning buffer began, so
// downstream consumers can schedule delivery instead of just firing
// ticks back-to-back whenever the buffer happens to be processed.
//
// This class does no I/O itself -- it only computes tick times and
// pushes them into a lock-free queue. MidiClockOutputManager (which
// runs on its own dedicated thread, not the audio thread and not the
// GUI thread) drains the queue and performs the actual MIDI send at
// the scheduled time. This keeps the audio callback allocation-free
// and wait-free, which is required to avoid audio dropouts.

#include <QAtomicInteger>
#include <atomic>
#include <chrono>

#include "engine/sync/midiclockqueue.h"

class MidiClockGenerator {
  public:
    MidiClockGenerator();
    ~MidiClockGenerator() = default;

    // Called once per audio callback by EngineMixer, on the audio
    // thread. `sampleRate` and `numFrames` describe the buffer that is
    // about to be processed (numFrames is per-channel frame count, not
    // sample count -- i.e. this should be iBufferSize / channelCount
    // for interleaved stereo buffers).
    //
    // bufferStartTime should be captured by the caller as
    // std::chrono::steady_clock::now() as close as possible to the
    // point the OS handed control to the audio callback, so tick
    // timestamps stay anchored to the real hardware clock instead of
    // drifting with however long upstream processing takes.
    void process(int sampleRate,
            int numFrames,
            std::chrono::steady_clock::time_point bufferStartTime);

    // Thread-safe setters, called from the main/GUI thread whenever
    // the user changes preferences or the tempo source's BPM/play
    // state changes. These are plain atomics -- process() reads them
    // once per buffer, so there is no tearing and no locking needed.
    void setEnabled(bool enabled);
    void setBpm(double bpm);
    void setPlaying(bool playing);
    // If true, 0xFA/0xFB/0xFC transport messages are sent when the
    // source deck's play state changes. If false, only 0xF8 clock
    // ticks are sent and the clock free-runs regardless of deck state
    // (useful for gear that should always chase tempo, e.g. a
    // sequencer you start manually).
    void setSendTransport(bool sendTransport);

    // Consumer-side access to the generated event stream.
    MidiClockQueue* queue() {
        return &m_queue;
    }

    // Resets phase and forces the next call to process() to emit a
    // Start (0xFA) message. Call this when the user changes the tempo
    // source or re-enables the clock, so downstream gear resyncs
    // phase cleanly instead of picking up ticks mid-beat.
    void requestRealign();

  private:
    MidiClockQueue m_queue;

    std::atomic<bool> m_enabled;
    std::atomic<bool> m_sendTransport;
    std::atomic<double> m_bpm;
    std::atomic<bool> m_playing;
    std::atomic<bool> m_realignRequested;

    // Audio-thread-only state (never touched from other threads).
    bool m_wasEnabled;
    bool m_wasPlaying;
    // Fractional sample position within the current tick period.
    // Kept as samples (not seconds) so it stays exact across buffers
    // regardless of buffer-size jitter from the OS audio backend.
    double m_samplesUntilNextTick;
    double m_lastSamplesPerTick;
};
