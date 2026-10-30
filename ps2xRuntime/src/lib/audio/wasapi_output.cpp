#include "wasapi_output.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <propsys.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace ps2x::audio
{
    namespace
    {
        // PKEY_Device_FriendlyName ({a45c254e-df1c-4efd-8020-67d146a850e0}, 14), spelled out to avoid the INITGUID dance.
        const PROPERTYKEY kFriendlyNameKey = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

        template <class T>
        void release(T *&p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        std::string narrow(const wchar_t *text)
        {
            if (!text)
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string out(n > 1 ? static_cast<size_t>(n - 1) : 0u, '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
            return out;
        }
    }

    struct WasapiOutput::Impl
    {
        IMMDeviceEnumerator *enumerator = nullptr;
        IMMDevice *device = nullptr;
        IAudioClient *client = nullptr;
        IAudioRenderClient *render = nullptr;
        IAudioClock *clock = nullptr;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
    };

    WasapiOutput::WasapiOutput(const WasapiConfig &config)
        : m_impl(std::make_unique<Impl>()), m_config(config),
          m_queue(static_cast<uint32_t>(static_cast<uint64_t>(config.targetLatencyMs) * 48u))
    {
    }

    std::unique_ptr<WasapiOutput> WasapiOutput::create(const WasapiConfig &config, std::string *error)
    {
        std::unique_ptr<WasapiOutput> out(new WasapiOutput(config));
        out->m_thread = std::thread([raw = out.get()] { raw->renderLoop(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!out->m_ready.load(std::memory_order_acquire) && !out->m_failed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!out->m_ready.load(std::memory_order_acquire))
        {
            if (error)
                *error = out->m_info.error.empty() ? "audio device initialisation timed out" : out->m_info.error;
            out->stop();
            return nullptr;
        }
        return out;
    }

    void WasapiOutput::stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_impl && m_impl->event)
            SetEvent(m_impl->event);
        if (m_thread.joinable())
            m_thread.join();
    }

    WasapiOutput::~WasapiOutput()
    {
        stop();
    }

    void WasapiOutput::renderLoop()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Impl &d = *m_impl;
        const auto fail = [&](const char *what, HRESULT hr) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
            m_info.error = text;
            m_failed.store(true, std::memory_order_release);
        };

        bool ok = false;
        HANDLE task = nullptr;
        do
        {
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&d.enumerator));
            if (FAILED(hr)) { fail("MMDeviceEnumerator", hr); break; }
            hr = d.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d.device);
            if (FAILED(hr)) { fail("GetDefaultAudioEndpoint", hr); break; }
            IPropertyStore *props = nullptr;
            if (SUCCEEDED(d.device->OpenPropertyStore(STGM_READ, &props)))
            {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(kFriendlyNameKey, &name)) && name.vt == VT_LPWSTR)
                    m_info.device = narrow(name.pwszVal);
                PropVariantClear(&name);
                props->Release();
            }
            hr = d.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&d.client));
            if (FAILED(hr)) { fail("IAudioClient", hr); break; }

            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = 48000;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = 48000u * 4u;
            const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(m_config.deviceBufferMs) * 10000;
            hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      duration, 0, &format, nullptr);
            if (FAILED(hr)) { fail("IAudioClient::Initialize", hr); break; }
            d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!d.event) { fail("CreateEvent", E_FAIL); break; }
            hr = d.client->SetEventHandle(d.event);
            if (FAILED(hr)) { fail("SetEventHandle", hr); break; }
            hr = d.client->GetBufferSize(&d.bufferFrames);
            if (FAILED(hr)) { fail("GetBufferSize", hr); break; }
            hr = d.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&d.render));
            if (FAILED(hr)) { fail("IAudioRenderClient", hr); break; }
            if (SUCCEEDED(d.client->GetService(__uuidof(IAudioClock), reinterpret_cast<void **>(&d.clock))))
            {
            }
            REFERENCE_TIME latency = 0;
            if (SUCCEEDED(d.client->GetStreamLatency(&latency)))
                m_info.streamLatencyMs = static_cast<uint32_t>(latency / 10000);
            m_info.bufferFrames = d.bufferFrames;
            {
                ISimpleAudioVolume *session = nullptr;
                if (SUCCEEDED(d.client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void **>(&session))))
                {
                    float v = -1.0f;
                    BOOL m = FALSE;
                    if (SUCCEEDED(session->GetMasterVolume(&v))) m_info.sessionVolume = v;
                    if (SUCCEEDED(session->GetMute(&m))) m_info.sessionMuted = m != FALSE;
                    session->Release();
                }
                IAudioEndpointVolume *endpoint = nullptr;
                if (SUCCEEDED(d.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&endpoint))))
                {
                    float v = -1.0f;
                    BOOL m = FALSE;
                    if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&v))) m_info.endpointVolume = v;
                    if (SUCCEEDED(endpoint->GetMute(&m))) m_info.endpointMuted = m != FALSE;
                    endpoint->Release();
                }
            }

            // Start with a silent buffer so the first period is valid.
            BYTE *data = nullptr;
            hr = d.render->GetBuffer(d.bufferFrames, &data);
            if (FAILED(hr)) { fail("GetBuffer (prime)", hr); break; }
            std::memset(data, 0, static_cast<size_t>(d.bufferFrames) * 4u);
            d.render->ReleaseBuffer(d.bufferFrames, 0);

            DWORD taskIndex = 0;
            task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            hr = d.client->Start();
            if (FAILED(hr)) { fail("IAudioClient::Start", hr); break; }
            m_info.running = true;
            ok = true;
        } while (false);

        if (ok)
        {
            m_ready.store(true, std::memory_order_release);
            auto lastLog = std::chrono::steady_clock::now();
            while (!m_stop.load(std::memory_order_acquire))
            {
                const DWORD wait = WaitForSingleObject(d.event, 200);
                if (m_stop.load(std::memory_order_acquire))
                    break;
                UINT32 padding = 0;
                if (FAILED(d.client->GetCurrentPadding(&padding)))
                    break; // device lost: stay silent, the emulation is unaffected
                const UINT32 room = d.bufferFrames - padding;
                if (wait == WAIT_TIMEOUT && room == 0)
                    continue;
                if (room != 0)
                {
                    BYTE *data = nullptr;
                    if (FAILED(d.render->GetBuffer(room, &data)))
                        break;
                    m_queue.pull(reinterpret_cast<int16_t *>(data), room);
                    d.render->ReleaseBuffer(room, 0);
                }
                if (d.clock)
                {
                    UINT64 pos = 0, freq = 0;
                    if (SUCCEEDED(d.clock->GetFrequency(&freq)) && SUCCEEDED(d.clock->GetPosition(&pos, nullptr)) && freq != 0)
                        m_devicePosition.store(static_cast<uint64_t>(static_cast<double>(pos) * 48000.0 / static_cast<double>(freq)), std::memory_order_relaxed);
                }
                if (m_config.logStats && std::chrono::steady_clock::now() - lastLog > std::chrono::seconds(5))
                {
                    lastLog = std::chrono::steady_clock::now();
                    const auto s = m_queue.stats();
                    std::fprintf(stderr, "[audio] fill=%u (%.1f ms) avg=%.1f ms ratio=%.5f underruns=%llu silence=%llu dropped=%llu skipped=%llu pushed=%llu\n", s.fillFrames,
                                 s.fillFrames / 48.0, s.averageFillFrames / 48.0, s.ratio, static_cast<unsigned long long>(s.underruns),
                                 static_cast<unsigned long long>(s.silenceFrames), static_cast<unsigned long long>(s.droppedFrames), static_cast<unsigned long long>(s.skippedFrames),
                                 static_cast<unsigned long long>(s.pushedFrames));
                }
            }
            if (d.client)
                d.client->Stop();
        }

        if (task)
            AvRevertMmThreadCharacteristics(task);
        release(d.clock);
        release(d.render);
        release(d.client);
        release(d.device);
        release(d.enumerator);
        if (d.event)
        {
            CloseHandle(d.event);
            d.event = nullptr;
        }
        if (SUCCEEDED(com))
            CoUninitialize();
    }
}

#else // !_WIN32

namespace ps2x::audio
{
    struct WasapiOutput::Impl {};
    WasapiOutput::WasapiOutput(const WasapiConfig &config) : m_impl(std::make_unique<Impl>()), m_config(config) {}
    std::unique_ptr<WasapiOutput> WasapiOutput::create(const WasapiConfig &, std::string *error)
    {
        if (error)
            *error = "WASAPI is only available on Windows";
        return nullptr;
    }
    void WasapiOutput::stop() {}
    WasapiOutput::~WasapiOutput() = default;
    void WasapiOutput::renderLoop() {}
}

#endif
