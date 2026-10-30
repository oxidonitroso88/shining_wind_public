// ps2AudioProbe: measures the WASAPI backend on the real default render endpoint, with the system's own loopback capture as the observer.
//   ps2AudioProbe [target_latency_ms=60] [device_buffer_ms=20]
// Tests (exit code 0 only when all pass):
//   1. continuity: a 440 Hz sine submitted in 10 ms blocks at real-time pace for 8 s must come out of the endpoint without dropouts or
//      period glitches (the loopback capture sees what the mixer plays);
//   2. latency: bursts submitted at a known QPC time are located in the capture; end-to-end latency = capture time - submit time;
//   3. emulation-like jitter: the producer delivers 10.67 ms blocks late (up to 35 ms stalls, as a VBlank-granular emulator does) and the
//      queue must absorb them without an underrun.
// Windows only. It plays audible tones at low level for about 30 s.

#include "../src/lib/audio/wasapi_output.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace
{
    double qpcNowSeconds()
    {
        static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / freq;
    }

    // Loopback capture of the default render endpoint: mono (left channel) float samples with their QPC capture time.
    struct Capture
    {
        std::vector<float> samples;
        std::vector<double> times; // seconds (QPC base), one per packet start
        std::vector<size_t> packetStart;
        double rate = 48000.0;
        std::mutex mtx;
        std::atomic<bool> stop{false}, ready{false}, failed{false};
        std::thread thread;

        void run()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IMMDeviceEnumerator *en = nullptr;
            IMMDevice *dev = nullptr;
            IAudioClient *client = nullptr;
            IAudioCaptureClient *cap = nullptr;
            WAVEFORMATEX *fmt = nullptr;
            do
            {
                if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&en)))) break;
                if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
                if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client)))) break;
                if (FAILED(client->GetMixFormat(&fmt))) break;
                rate = fmt->nSamplesPerSec;
                if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt, nullptr))) break;
                if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap)))) break;
                if (FAILED(client->Start())) break;
                ready = true;
                const bool isFloat = fmt->wBitsPerSample == 32;
                while (!stop)
                {
                    UINT32 packet = 0;
                    cap->GetNextPacketSize(&packet);
                    while (packet != 0)
                    {
                        BYTE *data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        UINT64 devPos = 0, qpc = 0;
                        if (FAILED(cap->GetBuffer(&data, &frames, &flags, &devPos, &qpc))) break;
                        std::lock_guard<std::mutex> lock(mtx);
                        times.push_back(static_cast<double>(qpc) * 1e-7);
                        packetStart.push_back(samples.size());
                        for (UINT32 i = 0; i < frames; ++i)
                        {
                            float v = 0.0f;
                            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                                v = isFloat ? reinterpret_cast<const float *>(data)[static_cast<size_t>(i) * fmt->nChannels]
                                            : reinterpret_cast<const int16_t *>(data)[static_cast<size_t>(i) * fmt->nChannels] / 32768.0f;
                            samples.push_back(v);
                        }
                        cap->ReleaseBuffer(frames);
                        cap->GetNextPacketSize(&packet);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                client->Stop();
            } while (false);
            if (!ready)
                failed = true;
            if (fmt) CoTaskMemFree(fmt);
            if (cap) cap->Release();
            if (client) client->Release();
            if (dev) dev->Release();
            if (en) en->Release();
            CoUninitialize();
        }
        void start()
        {
            thread = std::thread([this] { run(); });
            while (!ready && !failed)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        void finish()
        {
            stop = true;
            if (thread.joinable())
                thread.join();
        }
        // time of capture sample i
        double timeOf(size_t i) const
        {
            size_t p = std::upper_bound(packetStart.begin(), packetStart.end(), i) - packetStart.begin();
            if (p == 0) return times.empty() ? 0.0 : times[0];
            --p;
            return times[p] + static_cast<double>(i - packetStart[p]) / rate;
        }
    };

    struct Producer
    {
        ps2x::audio::WasapiOutput &out;
        // Sine generator state; submits `frames` frames of a 440 Hz tone (amplitude 0.25) or silence.
        double phase = 0.0;
        void push(size_t frames, bool tone)
        {
            std::vector<int16_t> block(frames * 2u, 0);
            if (tone)
                for (size_t i = 0; i < frames; ++i)
                {
                    const auto s = static_cast<int16_t>(std::lround(std::sin(phase) * 8000.0));
                    block[i * 2u] = block[i * 2u + 1u] = s;
                    phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
                }
            out.submit(block.data(), frames);
        }
    };

    // Counts glitches in a captured 440 Hz sine segment: upward zero-crossing intervals must stay within 10 % of the nominal period, and the
    // amplitude must not collapse.
    struct SineReport
    {
        int crossings = 0, badIntervals = 0, silentWindows = 0;
        double maxIntervalError = 0.0;
    };

    SineReport analyseSine(const std::vector<float> &x, size_t begin, size_t end, double rate, float silentBelow)
    {
        SineReport r;
        const double nominal = rate / 440.0;
        double last = -1.0;
        for (size_t i = begin + 1; i < end; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                const double pos = static_cast<double>(i - 1) + (0.0 - x[i - 1]) / (static_cast<double>(x[i]) - x[i - 1]);
                if (last >= 0.0)
                {
                    const double err = std::fabs((pos - last) - nominal) / nominal;
                    r.maxIntervalError = std::max(r.maxIntervalError, err);
                    if (err > 0.10)
                        ++r.badIntervals;
                }
                last = pos;
                ++r.crossings;
            }
        const size_t window = static_cast<size_t>(rate * 0.005);
        for (size_t i = begin; i + window < end; i += window)
        {
            float peak = 0.0f;
            for (size_t k = 0; k < window; ++k)
                peak = std::max(peak, std::fabs(x[i + k]));
            if (peak < silentBelow)
                ++r.silentWindows;
        }
        return r;
    }

    void sleepUntil(std::chrono::steady_clock::time_point t) { std::this_thread::sleep_until(t); }
}

int main(int argc, char **argv)
{
    timeBeginPeriod(1); // accurate sleeps for the producer pacing

    ps2x::audio::WasapiConfig config;
    if (argc > 1) config.targetLatencyMs = static_cast<uint32_t>(std::max(10, std::atoi(argv[1])));
    if (argc > 2) config.deviceBufferMs = static_cast<uint32_t>(std::max(5, std::atoi(argv[2])));
    std::string error;
    auto out = ps2x::audio::WasapiOutput::create(config, &error);
    if (!out)
    {
        std::printf("FAIL: no audio device (%s)\n", error.c_str());
        return 2;
    }
    std::printf("device '%s': buffer %u frames, stream latency %u ms, queue target %u ms\n", out->info().device.c_str(), out->info().bufferFrames,
                out->info().streamLatencyMs, config.targetLatencyMs);
    std::printf("session volume %.2f%s, endpoint volume %.2f%s\n", out->info().sessionVolume, out->info().sessionMuted ? " (MUTED)" : "", out->info().endpointVolume, out->info().endpointMuted ? " (MUTED)" : "");
    // The loopback sees the stream after the endpoint's master volume (system volume slider): thresholds scale with it.
    const float level = out->info().endpointVolume > 0.0f && !out->info().endpointMuted ? out->info().endpointVolume : 1.0f;
    const float expectedPeak = 8000.0f / 32768.0f * level;
    const float silentBelow = expectedPeak * 0.1f, onsetAbove = expectedPeak * 0.08f;
    if (out->info().endpointMuted || out->info().sessionMuted)
        std::printf("WARNING: the endpoint or the session is muted: the capture will be silent\n");
    Capture cap;
    cap.start();
    if (cap.failed)
    {
        std::printf("FAIL: loopback capture unavailable\n");
        return 2;
    }
    int failures = 0;
    Producer prod{*out};
    const auto blockPeriod = std::chrono::microseconds(10000);

    // ---- 1. continuity ----
    const double t1Start = qpcNowSeconds();
    const uint64_t devStart1 = out->devicePositionFrames();
    uint32_t fillMin = 1u << 30, fillMax = 0;
    uint64_t firstUnderrunBlock = 0;
    {
        auto next = std::chrono::steady_clock::now();
        for (int i = 0; i < 800; ++i) // 8 s of 10 ms blocks
        {
            prod.push(480u, true);
            {
                const auto st = out->stats();
                if (i > 100) { fillMin = std::min(fillMin, st.fillFrames); fillMax = std::max(fillMax, st.fillFrames); }
                if (st.underruns != 0 && firstUnderrunBlock == 0) firstUnderrunBlock = static_cast<uint64_t>(i) + 1u;
            }
            next += blockPeriod;
            sleepUntil(next);
        }
    }
    const double t1End = qpcNowSeconds();
    const auto statsEnd1 = out->stats();
    const uint64_t devEnd1 = out->devicePositionFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        std::lock_guard<std::mutex> lock(cap.mtx);
        // analyse the middle of the capture window (skip start-up and tail)
        size_t b = 0, e = 0;
        for (size_t i = 0; i < cap.samples.size(); i += 64)
        {
            const double t = cap.timeOf(i);
            if (b == 0 && t > t1Start + 2.0) b = i;
            if (t < t1End - 1.0) e = i;
        }
        const SineReport r = analyseSine(cap.samples, b, e, cap.rate, silentBelow);
        const auto s = statsEnd1;
        std::printf("   device clock: played %.0f frames in %.3f s of QPC time -> %.4f x nominal\n", static_cast<double>(devEnd1 - devStart1), t1End - t1Start, static_cast<double>(devEnd1 - devStart1) / ((t1End - t1Start) * 48000.0));
        std::printf("   queue fill during the run: %.1f..%.1f ms, first underrun at block %llu\n", fillMin / 48.0, fillMax / 48.0, static_cast<unsigned long long>(firstUnderrunBlock));
        std::printf("1. continuity: %.1f s analysed, %d cycles, %d bad intervals (max error %.1f %%), %d silent 5 ms windows; underruns %llu, dropped %llu\n",
                    static_cast<double>(e - b) / cap.rate, r.crossings, r.badIntervals, r.maxIntervalError * 100.0, r.silentWindows,
                    static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.droppedFrames));
        if (r.crossings < 2000 || r.badIntervals != 0 || r.silentWindows != 0 || s.underruns != 0 || s.droppedFrames != 0)
        {
            std::printf("   FAIL\n");
            ++failures;
        }
        else
            std::printf("   PASS\n");
    }

    // ---- 2. latency ----
    {
        std::vector<double> lat;
        for (int burst = 0; burst < 6; ++burst)
        {
            size_t before = 0;
            {
                std::lock_guard<std::mutex> lock(cap.mtx);
                before = cap.samples.size();
            }
            auto next = std::chrono::steady_clock::now();
            double submitTime = 0.0;
            for (int i = 0; i < 100; ++i) // 1 s: silence, then 200 ms of the 440 Hz tone starting at block 50
            {
                if (i == 50)
                    submitTime = qpcNowSeconds();
                prod.push(480u, i >= 50 && i < 70);
                next += blockPeriod;
                sleepUntil(next);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::lock_guard<std::mutex> lock(cap.mtx);
            for (size_t i = before; i < cap.samples.size(); ++i)
                if (std::fabs(cap.samples[i]) > onsetAbove && cap.timeOf(i) > submitTime)
                {
                    lat.push_back((cap.timeOf(i) - submitTime) * 1000.0);
                    break;
                }
            if (lat.size() < static_cast<size_t>(burst) + 1u)
            {
                float peak = 0.0f;
                for (size_t i = before; i < cap.samples.size(); ++i) peak = std::max(peak, std::fabs(cap.samples[i]));
                for (size_t i = before; i < cap.samples.size(); i += static_cast<size_t>(cap.rate / 10))
                {
                    float pk = 0.0f;
                    for (size_t k = i; k < std::min(cap.samples.size(), i + static_cast<size_t>(cap.rate / 10)); ++k) pk = std::max(pk, std::fabs(cap.samples[k]));
                    std::printf("%.2f ", pk);
                }
                std::printf("\n");
                std::printf("   (burst %d not located: capture peak %.3f, %zu samples)\n", burst, peak, cap.samples.size() - before);
            }
        }
        std::sort(lat.begin(), lat.end());
        if (lat.empty())
        {
            std::printf("2. latency: burst never seen in the capture\n   FAIL\n");
            ++failures;
        }
        else
        {
            const double median = lat[lat.size() / 2];
            std::printf("2. latency (submit -> endpoint): min %.1f ms, median %.1f ms, max %.1f ms over %zu bursts (queue target %u ms + device buffer %u ms)\n", lat.front(), median,
                        lat.back(), lat.size(), config.targetLatencyMs, config.deviceBufferMs);
            const double limit = config.targetLatencyMs + config.deviceBufferMs + 60.0;
            if (median > limit)
            {
                std::printf("   FAIL (> %.0f ms)\n", limit);
                ++failures;
            }
            else
                std::printf("   PASS\n");
        }
    }

    // ---- 3. emulation-like jitter ----
    {
        const auto before = out->stats();
        auto next = std::chrono::steady_clock::now();
        uint32_t seed = 12345u;
        double carry = 0.0;
        for (int i = 0; i < 560; ++i) // ~6 s of 10.67 ms blocks (the AutoDMA block period)
        {
            prod.push(512u, true);
            next += std::chrono::microseconds(10667);
            // every few blocks the "emulator" is late (a heavy frame): delay the next submission by up to 35 ms without moving the schedule
            seed = seed * 1664525u + 1013904223u;
            if ((seed >> 24) % 5u == 0u)
                std::this_thread::sleep_for(std::chrono::milliseconds(5 + (seed >> 8) % 16u)); // 5-20 ms: up to about one video frame
            sleepUntil(next);
            (void)carry;
        }
        // one long stall (a heavy frame) shorter than the queue target must also be absorbed
        std::this_thread::sleep_for(std::chrono::milliseconds(config.targetLatencyMs * 3 / 4));
        for (int i = 0; i < 4; ++i)
            prod.push(512u, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto after = out->stats();
        const uint64_t underruns = after.underruns - before.underruns;
        std::printf("3. jitter: %llu underruns, %llu dropped frames, average fill %.1f ms, ratio %.5f\n", static_cast<unsigned long long>(underruns),
                    static_cast<unsigned long long>(after.droppedFrames - before.droppedFrames), after.averageFillFrames / 48.0, after.ratio);
        if (underruns != 0 || after.droppedFrames != before.droppedFrames)
        {
            std::printf("   FAIL\n");
            ++failures;
        }
        else
            std::printf("   PASS\n");
    }

    cap.finish();
    out->stop();
    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
