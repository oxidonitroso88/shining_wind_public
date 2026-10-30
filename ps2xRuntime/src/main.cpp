#include "ps2_soul_session.h"
#include "extras/extras_host.h"
#include "ps2_runtime.h"
#include "lib/audio/wasapi_output.h"
#include "games_database.h"
#include "launch_options.h"
#include "disc_picker_win.h"
#include "error_log.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(const ps2x::launch::Options &options)
    {
        if (!options.bootPath.empty())
        {
            std::cout << "Using argv boot path" << std::endl;
            return options.bootPath;
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    setupTerminateLogger();

    try
    {
        std::vector<std::string> launchArgs;
        bool standaloneExtras = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            // PC Extras Soul Tree viewer session: --soul-event=<character>:<door> (the parent selector starts this throw-away process)
            if (arg.rfind("--soul-event=", 0) == 0)
            {
                pc_extras::soul_session::Request request;
                if (!pc_extras::soul_session::parseRequest(std::string_view(arg).substr(13), request))
                {
                    std::cerr << "Invalid --soul-event (expected <character>:<door>): " << arg << std::endl;
                    return 1;
                }
                pc_extras::soul_session::setRequest(request);
                continue;
            }
            if (arg == "--extras") // PC Extras in its own window, without starting the emulation
            {
                standaloneExtras = true;
                continue;
            }
            launchArgs.emplace_back(arg);
        }
        ps2x::launch::Options launchOptions = ps2x::launch::parse(launchArgs);
        if (standaloneExtras)
        {
            // The menu needs no game; the game's ELF (when given) is only what event sessions are started with.
            if (launchOptions.error.empty() && launchOptions.discGiven)
                ps2x::launch::resolveImage(launchOptions);
            std::error_code ec;
            const std::filesystem::path exe = std::filesystem::absolute(argc > 0 && argv[0] ? argv[0] : "", ec);
            pc_extras::host::setLaunchContext(exe.string(), launchOptions.error.empty() ? launchOptions.bootPath.string() : std::string());
            return pc_extras::host::runStandalone();
        }
        [[maybe_unused]] bool doubleClickLaunch = false;
#if defined(_WIN32) && !defined(PS2X_DEFAULT_BOOT_ELF)
        // Double-click: no --disc and no ELF -> remembered game (folder or disc image) or a file / folder dialog.
        if (launchOptions.error.empty() && launchOptions.bootPath.empty() && launchOptions.isoPath.empty())
        {
            doubleClickLaunch = true;
            ps2x::launch::chooseDiscInteractively(launchOptions, ps2x::launch::pickDisc);
        }
#endif
#if defined(_WIN32)
        // Distributed use (--disc / double click): a small capped error log in <data>/logs (the development launch is unchanged).
        if (!launchOptions.dataDir.empty())
        {
            const std::string logHeader = std::string("exe=") + (argc > 0 && argv[0] ? argv[0] : "?") + "\nbuilt=" + __DATE__ + " " + __TIME__ +
                                          "\ndata=" + launchOptions.dataDir.string() + "\nboot=" + launchOptions.bootPath.string() +
                                          "\nimage=" + launchOptions.isoPath.string();
            ps2x::errlog::install(launchOptions.dataDir, logHeader, !ps2x::launch::noUsableErrorOutput());
        }
#endif
        // --disc <image>: check the ELF inside the image, then copy the game files once into the data folder.
        ps2x::launch::resolveImage(launchOptions);
        if (launchOptions.error.empty() && launchOptions.discGiven && launchOptions.verify)
        {
            launchOptions.error = ps2x::launch::verifyBootElf(launchOptions.bootPath);
        }
        if (!launchOptions.error.empty())
        {
            std::cerr << launchOptions.error << std::endl;
#if defined(_WIN32)
            if (doubleClickLaunch || ps2x::launch::noUsableErrorOutput()) // scripted runs (stderr is a console, file or pipe) never block on a dialog
            {
                ps2x::launch::showLaunchError(launchOptions.error);
            }
#endif
            return 1;
        }
        ps2x::launch::applyDataDir(launchOptions);
        if (!launchOptions.dataDir.empty())
        {
            std::cout << "User data folder: " << launchOptions.dataDir.string() << std::endl;
        }

        std::filesystem::path pathObj = getExecutablePath(launchOptions);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        {
            std::error_code ec;
            const std::filesystem::path exe = std::filesystem::absolute(argc > 0 && argv[0] ? argv[0] : "", ec);
            pc_extras::host::setLaunchContext(exe.string(), filePathStr);
        }

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        if (!pc_extras::soul_session::requested()) // an event session shows only the event: no debugger panel over it
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

        // Real-time audio (milestone M3): WASAPI render thread fed by the SPU2 mix. PS2X_AUDIO=0 disables it; the emulation never waits for it.
        // PS2X_AUDIO_LATENCY_MS (queue target, default 60), PS2X_AUDIO_BUFFER_MS (device buffer, default 20), PS2X_AUDIO_STATS=1.
#if defined(_WIN32)
        // PS2X_AUDIO=0 with PS2X_AUDIO_STATS=1: no device, only a counter that prints how fast the emulation produces audio (1.0 = real time).
        struct RateMonitor final : ps2x::iop::AudioSink
        {
            std::atomic<uint64_t> frames{0};
            std::thread thread;
            std::atomic<bool> stop{false};
            void submit(const int16_t *, size_t n) noexcept override { frames.fetch_add(n, std::memory_order_relaxed); }
            RateMonitor()
            {
                thread = std::thread([this] {
                    uint64_t last = 0;
                    auto lastTime = std::chrono::steady_clock::now();
                    while (!stop.load())
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        const auto now = std::chrono::steady_clock::now();
                        if (now - lastTime >= std::chrono::seconds(5))
                        {
                            const uint64_t total = frames.load();
                            const double seconds = std::chrono::duration<double>(now - lastTime).count();
                            std::fprintf(stderr, "[audio-rate] emulation produced %.3f x real time\n", static_cast<double>(total - last) / 48000.0 / seconds);
                            last = total;
                            lastTime = now;
                        }
                    }
                });
            }
            ~RateMonitor() override { stop = true; if (thread.joinable()) thread.join(); }
        };
        std::unique_ptr<RateMonitor> rateMonitor;
        std::unique_ptr<ps2x::audio::WasapiOutput> audioOut;
        if (const char *audioEnv = std::getenv("PS2X_AUDIO"); audioEnv != nullptr && std::string(audioEnv) == "0")
        {
            if (std::getenv("PS2X_AUDIO_STATS") != nullptr)
            {
                rateMonitor = std::make_unique<RateMonitor>();
                ps2x::iop::setAudioSink(rateMonitor.get());
            }
        }
        else
        {
            ps2x::audio::WasapiConfig audioConfig;
            if (const char *v = std::getenv("PS2X_AUDIO_LATENCY_MS")) audioConfig.targetLatencyMs = static_cast<uint32_t>(std::max(10, std::atoi(v)));
            if (const char *v = std::getenv("PS2X_AUDIO_BUFFER_MS")) audioConfig.deviceBufferMs = static_cast<uint32_t>(std::max(5, std::atoi(v)));
            audioConfig.logStats = std::getenv("PS2X_AUDIO_STATS") != nullptr;
            std::string audioError;
            audioOut = ps2x::audio::WasapiOutput::create(audioConfig, &audioError);
            if (audioOut)
            {
                ps2x::iop::setAudioSink(audioOut.get());
                std::cerr << "[audio] WASAPI output on '" << audioOut->info().device << "' (device buffer " << audioOut->info().bufferFrames
                          << " frames, stream latency " << audioOut->info().streamLatencyMs << " ms, queue target " << audioConfig.targetLatencyMs << " ms)" << std::endl;
            }
            else
                std::cerr << "[audio] no audio device (" << audioError << "); running silent" << std::endl;
        }
#endif
        runtime.run();
#if defined(_WIN32)
        ps2x::iop::setAudioSink(nullptr);
        audioOut.reset();
        rateMonitor.reset();
#endif

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
