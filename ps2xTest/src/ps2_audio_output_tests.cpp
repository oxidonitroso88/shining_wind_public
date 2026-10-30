// Real-time audio output queue (milestone M3): priming, continuity, underrun recovery, clock-drift absorption, overflow. The device side
// (WASAPI) is exercised separately by the ps2AudioProbe tool; everything that can be decided without a device is decided here.
#include "MiniTest.h"
#include "ps2x/iop/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    using ps2x::iop::AudioPlaybackQueue;

    // Interleaved ramp: L = n, R = -n (16-bit wrapping is avoided by the short runs used here).
    std::vector<int16_t> ramp(int start, int frames)
    {
        std::vector<int16_t> v(static_cast<size_t>(frames) * 2u);
        for (int i = 0; i < frames; ++i)
        {
            v[static_cast<size_t>(i) * 2u] = static_cast<int16_t>(start + i);
            v[static_cast<size_t>(i) * 2u + 1u] = static_cast<int16_t>(-(start + i));
        }
        return v;
    }

    // Producer pushes `rate` x 480 frames of a slow sine per 10 ms period, consumer pulls 480 frames. Returns the worst jump between consecutive
    // output samples (a click shows up as a jump far above the sine's own slope).
    struct DriftResult
    {
        double maxStep = 0.0;
        double minFill = 1e9, maxFill = 0.0;
        uint64_t underruns = 0, dropped = 0;
        double finalAvgFill = 0.0, finalRatio = 1.0;
    };

    DriftResult simulate(double producerRate, int periods, uint32_t targetFrames)
    {
        AudioPlaybackQueue q(targetFrames);
        DriftResult r;
        double phase = 0.0, carry = 0.0;
        const double step = 2.0 * 3.14159265358979 * 220.0 / 48000.0; // 220 Hz
        std::vector<int16_t> in, out(480u * 2u);
        int16_t prev = 0;
        bool havePrev = false;
        for (int p = 0; p < periods; ++p)
        {
            carry += 480.0 * producerRate;
            const int n = static_cast<int>(carry);
            carry -= n;
            in.assign(static_cast<size_t>(n) * 2u, 0);
            for (int i = 0; i < n; ++i)
            {
                const auto s = static_cast<int16_t>(std::lround(std::sin(phase) * 12000.0));
                in[static_cast<size_t>(i) * 2u] = s;
                in[static_cast<size_t>(i) * 2u + 1u] = s;
                phase += step;
            }
            q.push(in.data(), static_cast<size_t>(n));
            q.pull(out.data(), 480u);
            if (p > 400) // after priming and the first control transients
            {
                for (size_t i = 0; i < 480u; ++i)
                {
                    if (havePrev)
                        r.maxStep = std::max(r.maxStep, std::fabs(static_cast<double>(out[i * 2u]) - prev));
                    prev = out[i * 2u];
                    havePrev = true;
                }
                const auto s = q.stats();
                r.minFill = std::min<double>(r.minFill, s.fillFrames);
                r.maxFill = std::max<double>(r.maxFill, s.fillFrames);
            }
        }
        const auto s = q.stats();
        r.underruns = s.underruns;
        r.dropped = s.droppedFrames;
        r.finalAvgFill = s.averageFillFrames;
        r.finalRatio = s.ratio;
        return r;
    }
}

void register_ps2_audio_output_tests()
{
    MiniTest::Case("Audio output queue (M3)", [](TestCase &tc)
                   {
        tc.Run("silence until the queue holds the target, then the stream starts at its first frame", [](TestCase &t) {
            AudioPlaybackQueue q(960u);
            std::vector<int16_t> out(480u * 2u, 123);
            const auto first = ramp(1000, 480);
            q.push(first.data(), 480u);
            q.pull(out.data(), 480u);
            bool allZero = true;
            for (int16_t s : out)
                allZero = allZero && s == 0;
            t.IsTrue(allZero, "below the target the device gets silence");
            t.IsTrue(q.stats().fillFrames == 480u, "nothing is consumed while priming");
            const auto second = ramp(1480, 600);
            q.push(second.data(), 600u);
            q.pull(out.data(), 480u);
            t.IsTrue(std::abs(out[0]) < 40, "playback fades in (no click at the start)");
            t.IsTrue(std::abs(out[200u * 2u] - (1000 + 200)) <= 2 && std::abs(out[200u * 2u + 1u] + (1000 + 200)) <= 2, "after the fade-in the stream is the first queued frames (none lost)");
            t.IsTrue(q.stats().underruns == 0, "priming is not an underrun");
        });

        tc.Run("a steady stream plays back without gaps or jumps", [](TestCase &t) {
            AudioPlaybackQueue q(960u);
            std::vector<int16_t> out(480u * 2u);
            int next = 0;
            double worst = 0;
            int prev = -1;
            for (int p = 0; p < 60; ++p) // 28800 frames: the ramp stays inside 16 bits
            {
                const auto in = ramp(next, 480);
                next += 480;
                q.push(in.data(), 480u);
                q.pull(out.data(), 480u);
                if (p >= 4)
                    for (size_t i = 0; i < 480u; ++i)
                    {
                        if (prev >= 0)
                            worst = std::max(worst, std::fabs(out[i * 2u] - static_cast<double>(prev)));
                        prev = out[i * 2u];
                    }
            }
            t.IsTrue(worst <= 2.0, "consecutive output samples of a ramp never jump (rate control keeps the ratio within 1 % of 1)");
            t.IsTrue(q.stats().underruns == 0 && q.stats().droppedFrames == 0, "no underrun, no drop");
        });

        tc.Run("an empty queue gives silence, counts one underrun and re-primes before playing again", [](TestCase &t) {
            AudioPlaybackQueue q(960u);
            std::vector<int16_t> out(480u * 2u);
            const auto in = ramp(2000, 1200);
            q.push(in.data(), 1200u);
            q.pull(out.data(), 480u);   // plays 480 of 1200
            q.pull(out.data(), 480u);   // 240 left
            q.pull(out.data(), 480u);   // runs dry
            t.IsTrue(q.stats().underruns == 1, "one underrun");
            t.IsTrue(std::abs(out[479u * 2u]) < 40, "the tail fades out instead of stopping dead");
            const auto more = ramp(3200, 500);
            q.push(more.data(), 500u);
            q.pull(out.data(), 480u);
            t.IsTrue(out[0] == 0 || std::abs(out[0]) < 40, "below the target after an underrun it keeps the fade (re-priming)");
            const auto more2 = ramp(3700, 600);
            q.push(more2.data(), 600u);
            q.pull(out.data(), 480u);
            t.IsTrue(std::abs(out[0]) > 40 || out[2] != 0, "playback resumes once the target is reached again");
            t.IsTrue(q.stats().underruns == 1, "still a single underrun event");
        });

        tc.Run("the queue is wait-free and drops the newest frames when full, keeping the stored ones intact", [](TestCase &t) {
            AudioPlaybackQueue q(960u);
            const auto big = ramp(0, 30000);
            const size_t a = q.push(big.data(), 30000u);
            const auto more = ramp(30000, 5000);
            const size_t b = q.push(more.data(), 5000u);
            t.IsTrue(a == 30000u && b == AudioPlaybackQueue::kCapacityFrames - 30000u, "stores up to the capacity");
            t.IsTrue(q.stats().droppedFrames == 5000u - b, "the rest is dropped and counted");
            std::vector<int16_t> out(480u * 2u);
            q.pull(out.data(), 480u);
            t.IsTrue(std::abs(out[200u * 2u] - 200) <= 2 && std::abs(out[200u * 2u + 1u] + 200) <= 2, "the oldest frames come out first, unchanged");
        });

        tc.Run("a long build-up (emulation far ahead of real time) is cut instead of drained slowly", [](TestCase &t) {
            AudioPlaybackQueue q(2880u);
            std::vector<int16_t> out(480u * 2u);
            for (int p = 0; p < 30; ++p)
            {
                const auto in = ramp(p * 480, 480);
                q.push(in.data(), 480u);
                if (p == 6)
                    q.pull(out.data(), 480u); // starts playing at the target
            }
            const uint32_t before = q.stats().fillFrames;
            q.pull(out.data(), 480u);
            const auto s = q.stats();
            t.IsTrue(before > 2880u * 4u, "set-up: far above four times the target");
            t.IsTrue(s.skippedFrames > 0u, "the excess was discarded and counted");
            t.IsTrue(s.fillFrames < 2880u + 600u, "the latency is back near the target after one pull");
            t.IsTrue(std::abs(out[0]) < 200, "the stream resumes with a fade-in (no hard step)");
            t.IsTrue(s.underruns == 0, "no underrun");
        });

        tc.Run("a producer clock 0.5 % faster than the device is absorbed: bounded latency, no drops, no clicks", [](TestCase &t) {
            const DriftResult r = simulate(1.005, 6000, 2880u); // 60 s
            t.IsTrue(r.dropped == 0 && r.underruns == 0, "no drop, no underrun");
            t.IsTrue(r.maxFill < 2880.0 * 1.8, "the queue never grows far past the target");
            t.IsTrue(r.maxStep < 2500.0, "no discontinuity (a 220 Hz sine at 12000 steps at most ~350 per sample)");
            t.IsTrue(r.finalRatio > 1.0, "the device side consumes faster to follow the faster producer");
        });

        tc.Run("a producer clock 0.5 % slower than the device is absorbed without underruns", [](TestCase &t) {
            const DriftResult r = simulate(0.995, 6000, 2880u);
            t.IsTrue(r.underruns == 0 && r.dropped == 0, "no underrun, no drop");
            t.IsTrue(r.minFill > 2880.0 * 0.2, "the queue never runs close to empty");
            t.IsTrue(r.maxStep < 2500.0, "no discontinuity");
            t.IsTrue(r.finalRatio < 1.0, "the device side slows down to follow the slower producer");
        });

        tc.Run("with exactly matching clocks the latency stays at the target and the ratio at 1", [](TestCase &t) {
            const DriftResult r = simulate(1.0, 3000, 2880u);
            t.IsTrue(std::fabs(r.finalAvgFill - 2880.0) < 480.0, "average fill within one period of the target (latency = target)");
            t.IsTrue(std::fabs(r.finalRatio - 1.0) < 0.002, "resampling ratio close to 1");
            t.IsTrue(r.underruns == 0, "no underrun");
        }); });
}
