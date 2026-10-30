#pragma once
// WASAPI shared-mode render backend for the SPU2 mix (milestone M3). Windows only; elsewhere create() returns nullptr.
//
// The emulation thread only calls submit() (wait-free, see ps2x/iop/audio_output.h). A dedicated, MMCSS-boosted render thread owns the
// device: event-driven, 48 kHz 16-bit stereo (the system converts to the endpoint format), pulls from the playback queue on every period.
// Nothing the device does (a slow endpoint, a device change, an exclusive-mode app) can stall the emulation: the worst case is silence.

#include "ps2x/iop/audio_output.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace ps2x::audio
{
    struct WasapiConfig
    {
        uint32_t targetLatencyMs = 60; // queue fill the rate controller aims for (on top of the device buffer)
        uint32_t deviceBufferMs = 20;  // requested shared-mode buffer
        bool logStats = false;         // PS2X_AUDIO_STATS=1
    };

    struct WasapiInfo
    {
        bool running = false;
        std::string device;            // friendly name of the endpoint
        uint32_t bufferFrames = 0;     // device buffer size
        uint32_t streamLatencyMs = 0;  // IAudioClient::GetStreamLatency
        float sessionVolume = -1.0f;   // this application's session volume (ISimpleAudioVolume), -1 = unknown
        bool sessionMuted = false;
        float endpointVolume = -1.0f;  // master volume of the endpoint (IAudioEndpointVolume)
        bool endpointMuted = false;
        std::string error;
    };

    class WasapiOutput final : public ps2x::iop::AudioSink
    {
    public:
        // Opens the default render endpoint and starts the render thread. Returns nullptr (and fills `error`) when no device is usable.
        static std::unique_ptr<WasapiOutput> create(const WasapiConfig &config, std::string *error = nullptr);
        ~WasapiOutput() override;

        void submit(const int16_t *interleavedStereo, size_t frames) noexcept override { m_queue.push(interleavedStereo, frames); }
        [[nodiscard]] ps2x::iop::AudioQueueStats stats() const noexcept { return m_queue.stats(); }
        [[nodiscard]] const WasapiInfo &info() const noexcept { return m_info; }
        // Frames the device has actually played so far (IAudioClock position), for latency measurements.
        [[nodiscard]] uint64_t devicePositionFrames() const noexcept { return m_devicePosition.load(std::memory_order_relaxed); }
        void stop();

    private:
        WasapiOutput(const WasapiConfig &config);
        void renderLoop();

        struct Impl;
        std::unique_ptr<Impl> m_impl;
        WasapiConfig m_config;
        WasapiInfo m_info;
        ps2x::iop::AudioPlaybackQueue m_queue;
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_ready{false};
        std::atomic<bool> m_failed{false};
        std::atomic<uint64_t> m_devicePosition{0};
        std::thread m_thread;
    };
}
