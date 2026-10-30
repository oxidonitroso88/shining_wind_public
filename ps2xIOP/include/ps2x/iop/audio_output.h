#pragma once
// Real-time audio output plumbing (milestone M3), independent of any operating system API so it can be unit tested.
//
//   SPU2 mix (deterministic, 48 kHz stereo, produced by the emulation thread)
//        --submit()-->  AudioPlaybackQueue  --pull()-->  audio device callback (WASAPI render thread)
//
// Rules that keep the audio backend from touching emulated time:
//  * submit() never blocks, never waits for the device and never takes a lock: when the queue is full the newest frames are dropped and counted.
//  * The emulation produces samples as a function of IOP time only; nothing the device does feeds back into it.
//  * The device side absorbs the difference between the emulation clock and the device clock: pull() resamples with a slowly varying ratio
//    (linear interpolation, at most +-1 %, slew limited) driven by the queue fill level, so the latency stays near its target instead of
//    drifting; an empty queue produces silence (with a short fade) and re-primes before playing again.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x::iop
{
    // Receives the final SPU2 mix (interleaved L,R int16, 48 kHz) from the emulation thread.
    class AudioSink
    {
    public:
        virtual ~AudioSink() = default;
        virtual void submit(const int16_t *interleavedStereo, size_t frames) noexcept = 0;
    };

    inline std::atomic<AudioSink *> &audioSinkSlot() noexcept
    {
        static std::atomic<AudioSink *> slot{nullptr};
        return slot;
    }
    inline void setAudioSink(AudioSink *sink) noexcept { audioSinkSlot().store(sink, std::memory_order_release); }

    // Set while the emulation runs unpaced (PS2X_FAST_FORWARD): the mix is still rendered (the SPU2 state is part of the emulation) but not sent to the
    // device, which would only overflow its queue.
    inline std::atomic<bool> &audioFastForwardSlot() noexcept
    {
        static std::atomic<bool> flag{false};
        return flag;
    }
    inline void setAudioFastForward(bool on) noexcept { audioFastForwardSlot().store(on, std::memory_order_relaxed); }
    inline bool audioFastForward() noexcept { return audioFastForwardSlot().load(std::memory_order_relaxed); }
    inline AudioSink *audioSink() noexcept { return audioSinkSlot().load(std::memory_order_acquire); }

    // PC Extras in front of the game (menu open / event session running): the mix is still rendered but not sent to the device (silence, with the
    // device side's own fade), exactly like the fast-forward case.
    inline std::atomic<bool> &audioMutedSlot() noexcept
    {
        static std::atomic<bool> flag{false};
        return flag;
    }
    inline void setAudioMuted(bool on) noexcept { audioMutedSlot().store(on, std::memory_order_relaxed); }
    inline bool audioMuted() noexcept { return audioMutedSlot().load(std::memory_order_relaxed); }

    struct AudioQueueStats
    {
        uint64_t pushedFrames = 0;
        uint64_t pulledFrames = 0;     // device frames produced (playing + silence)
        uint64_t silenceFrames = 0;    // device frames that were silence (priming or underrun)
        uint64_t underruns = 0;        // transitions from playing to empty
        uint64_t droppedFrames = 0;    // submitted frames that did not fit
        uint64_t skippedFrames = 0;    // queued frames discarded to cut a latency build-up (emulation running far ahead of real time)
        uint32_t fillFrames = 0;       // frames queued right now
        double ratio = 1.0;            // input frames consumed per device frame
        double averageFillFrames = 0.0;
    };

    // Single producer (emulation thread) / single consumer (device thread).
    class AudioPlaybackQueue
    {
    public:
        static constexpr uint32_t kCapacityFrames = 1u << 15; // 32768 frames = 683 ms

        explicit AudioPlaybackQueue(uint32_t targetFrames = 2880u) : m_storage(static_cast<size_t>(kCapacityFrames) * 2u, 0), m_target(targetFrames)
        {
            m_avgFill = static_cast<double>(targetFrames);
        }

        void setTargetFrames(uint32_t frames) noexcept { m_target = std::max<uint32_t>(frames, 240u); }
        [[nodiscard]] uint32_t targetFrames() const noexcept { return m_target; }

        // Producer. Wait-free. Returns the number of frames stored.
        size_t push(const int16_t *interleavedStereo, size_t frames) noexcept
        {
            const uint64_t w = m_write.load(std::memory_order_relaxed);
            const uint64_t r = m_read.load(std::memory_order_acquire);
            const size_t free = kCapacityFrames - static_cast<size_t>(w - r);
            const size_t n = std::min(frames, free);
            for (size_t i = 0; i < n; ++i)
            {
                const size_t slot = static_cast<size_t>((w + i) & (kCapacityFrames - 1u)) * 2u;
                m_storage[slot] = interleavedStereo[i * 2u];
                m_storage[slot + 1u] = interleavedStereo[i * 2u + 1u];
            }
            m_write.store(w + n, std::memory_order_release);
            m_pushed.fetch_add(n, std::memory_order_relaxed);
            if (n < frames)
                m_dropped.fetch_add(frames - n, std::memory_order_relaxed);
            return n;
        }

        // Consumer. Fills `frames` interleaved stereo device frames (48 kHz).
        void pull(int16_t *out, size_t frames) noexcept
        {
            uint64_t r = m_read.load(std::memory_order_relaxed);
            const uint64_t w = m_write.load(std::memory_order_acquire);
            // The emulation can run far ahead of real time for a while (loading screens with an instant CD). Rate control alone would need
            // minutes to give that latency back, so beyond four times the target the oldest audio is discarded (and faded in again).
            if (m_state == State::Playing && (w - r) > static_cast<uint64_t>(m_target) * 4u)
            {
                const uint64_t keep = m_target;
                m_skipped.fetch_add((w - r) - keep, std::memory_order_relaxed);
                r = w - keep;
                m_frac = 0.0;
                m_fade = 0.0f;
                m_avgFill = static_cast<double>(keep);
            }
            const double fill = static_cast<double>(w - r) - m_frac;
            m_avgFill += (fill - m_avgFill) * 0.02;
            updateRatio();

            for (size_t i = 0; i < frames; ++i)
            {
                const uint64_t avail = w - r;
                if (m_state == State::Priming)
                {
                    if (avail >= m_target)
                    {
                        m_state = State::Playing;
                        m_fade = 0.0f;
                    }
                }
                if (m_state == State::Playing && avail < 2u)
                {
                    m_state = State::Priming;
                    m_underruns.fetch_add(1, std::memory_order_relaxed);
                    m_frac = 0.0;
                }
                if (m_state == State::Priming)
                {
                    // short fade of the last output instead of a hard step to zero
                    m_lastL *= 0.98f;
                    m_lastR *= 0.98f;
                    out[i * 2u] = toInt16(m_lastL);
                    out[i * 2u + 1u] = toInt16(m_lastR);
                    m_silence.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const size_t a = static_cast<size_t>(r & (kCapacityFrames - 1u)) * 2u;
                const size_t b = static_cast<size_t>((r + 1u) & (kCapacityFrames - 1u)) * 2u;
                const float f = static_cast<float>(m_frac);
                m_lastL = static_cast<float>(m_storage[a]) + (static_cast<float>(m_storage[b]) - static_cast<float>(m_storage[a])) * f;
                m_lastR = static_cast<float>(m_storage[a + 1u]) + (static_cast<float>(m_storage[b + 1u]) - static_cast<float>(m_storage[a + 1u])) * f;
                if (m_fade < 1.0f)
                {
                    m_fade = std::min(1.0f, m_fade + 1.0f / 96.0f);
                    m_lastL *= m_fade;
                    m_lastR *= m_fade;
                }
                out[i * 2u] = toInt16(m_lastL);
                out[i * 2u + 1u] = toInt16(m_lastR);
                m_frac += m_ratio;
                while (m_frac >= 1.0)
                {
                    m_frac -= 1.0;
                    ++r;
                }
            }
            m_read.store(r, std::memory_order_release);
            m_pulled.fetch_add(frames, std::memory_order_relaxed);
            m_ratioOut.store(m_ratio, std::memory_order_relaxed);
            m_avgOut.store(m_avgFill, std::memory_order_relaxed);
        }

        [[nodiscard]] AudioQueueStats stats() const noexcept
        {
            AudioQueueStats s;
            s.pushedFrames = m_pushed.load(std::memory_order_relaxed);
            s.pulledFrames = m_pulled.load(std::memory_order_relaxed);
            s.silenceFrames = m_silence.load(std::memory_order_relaxed);
            s.underruns = m_underruns.load(std::memory_order_relaxed);
            s.droppedFrames = m_dropped.load(std::memory_order_relaxed);
            s.skippedFrames = m_skipped.load(std::memory_order_relaxed);
            s.fillFrames = static_cast<uint32_t>(m_write.load(std::memory_order_acquire) - m_read.load(std::memory_order_acquire));
            s.ratio = m_ratioOut.load(std::memory_order_relaxed);
            s.averageFillFrames = m_avgOut.load(std::memory_order_relaxed);
            return s;
        }

    private:
        enum class State : uint8_t { Priming, Playing };

        static int16_t toInt16(float v) noexcept
        {
            return static_cast<int16_t>(std::clamp(std::lrintf(v), -32768l, 32767l));
        }

        void updateRatio() noexcept
        {
            // Proportional control around the target level: fuller than the target -> consume a little faster, emptier -> slower.
            const double error = (m_avgFill - static_cast<double>(m_target)) / static_cast<double>(m_target);
            const double desired = 1.0 + std::clamp(error * 0.02, -0.01, 0.01);
            m_ratio += std::clamp(desired - m_ratio, -2e-5, 2e-5); // slew limit per device period: no audible pitch wobble
        }

        std::vector<int16_t> m_storage;
        std::atomic<uint64_t> m_write{0};
        std::atomic<uint64_t> m_read{0};
        uint32_t m_target;
        State m_state = State::Priming;
        double m_frac = 0.0;
        double m_ratio = 1.0;
        double m_avgFill = 0.0;
        float m_lastL = 0.0f, m_lastR = 0.0f;
        float m_fade = 1.0f;
        std::atomic<uint64_t> m_pushed{0}, m_pulled{0}, m_silence{0}, m_underruns{0}, m_dropped{0}, m_skipped{0};
        std::atomic<double> m_ratioOut{1.0}, m_avgOut{0.0};
    };
}
