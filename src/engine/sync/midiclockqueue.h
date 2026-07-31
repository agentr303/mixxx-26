#pragma once

// MidiClockQueue
//
// A fixed-capacity, wait-free single-producer/single-consumer ring
// buffer of (timestamp, MIDI byte) pairs.
//
// Producer: MidiClockGenerator::process(), running on the real-time
// audio callback thread. Must never block, allocate, or take a lock.
// Consumer: MidiClockOutputManager, running on its own dedicated
// std::thread. May block/sleep freely.
//
// Capacity is sized generously (a few seconds of clock ticks even at
// very high BPM) so the producer can never be blocked by a slow or
// stalled consumer; if the queue does fill up (e.g. no MIDI output
// device selected, or the consumer thread died), the producer simply
// drops the oldest un-sent tick rather than stalling the audio
// thread. This is a deliberate real-time-safety trade-off: a dropped
// MIDI clock tick is far less harmful than an audio glitch.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

class MidiClockQueue {
  public:
    struct Event {
        std::chrono::steady_clock::time_point timestamp;
        unsigned char byte = 0;
    };

    // ~4 seconds of headroom at the fastest realistic clock rate
    // (24 ppqn * 300 BPM / 60 ~= 120 ticks/sec) plus transport bytes.
    static constexpr size_t kCapacity = 1024;

    MidiClockQueue()
            : m_head(0),
              m_tail(0) {
    }

    // Producer side. Never blocks. Returns false (and drops the
    // event) only if the consumer has fallen more than kCapacity
    // events behind, which should not happen in normal operation.
    bool push(const Event& event) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t nextHead = (head + 1) % kCapacity;
        if (nextHead == m_tail.load(std::memory_order_acquire)) {
            // Queue full -- drop the event rather than block the
            // audio thread. Advance a drop counter so the consumer
            // can log/react if it wants to.
            m_droppedEvents.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_buffer[head] = event;
        m_head.store(nextHead, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool pop(Event* pEvent) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) {
            return false;
        }
        *pEvent = m_buffer[tail];
        m_tail.store((tail + 1) % kCapacity, std::memory_order_release);
        return true;
    }

    // Approximate -- for diagnostics only, not synchronized with
    // push()/pop() beyond the atomics above.
    size_t droppedEvents() const {
        return m_droppedEvents.load(std::memory_order_relaxed);
    }

  private:
    std::array<Event, kCapacity> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
    std::atomic<size_t> m_droppedEvents{0};
};
