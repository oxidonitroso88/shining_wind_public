// Diagnostic sampling profiler for the host thread that executes the recompiled guest code (Windows only).
// PS2X_RIP_PROFILE=1 samples the thread's RIP every millisecond; at exit the hottest module-relative addresses are
// printed as "[rip] rva=0x... n=...". Resolve them with the linker map (/MAP) of ps2EntryRunner.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace
{
    std::atomic<bool> g_stop{false};
    std::thread g_thread;
    std::unordered_map<uint64_t, uint32_t> g_histogram;
    std::unordered_map<uint64_t, uint32_t> g_chains; // key = FNV hash of the first 5 return addresses into the image
    std::unordered_map<uint64_t, std::vector<uint64_t>> g_chainFrames;
    std::unordered_map<uint64_t, uint32_t> g_callers;
    std::unordered_map<uint64_t, uint32_t> g_windowChains; // same key as g_chains, reset every 10 s // exe RVA of the first return address found on the stack of a sample outside the exe
} // namespace

void ps2StartRipProfiler(void *threadHandle)
{
#ifdef _WIN32
    if (threadHandle == nullptr || g_thread.joinable())
        return;
    g_stop.store(false);
    g_thread = std::thread([threadHandle]() {
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        auto lastDump = std::chrono::steady_clock::now();
        std::unordered_map<uint64_t, uint32_t> window;
        std::unordered_map<uint64_t, uint32_t> allWindow;
        while (!g_stop.load(std::memory_order_relaxed))
        {
            // Every 10 s print the hottest addresses of the last window ("[rip:window]"), so a hang can be located live.
            if (std::chrono::steady_clock::now() - lastDump > std::chrono::seconds(10))
            {
                lastDump = std::chrono::steady_clock::now();
                std::vector<std::pair<uint64_t, uint32_t>> top(window.begin(), window.end());
                std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                for (size_t i = 0; i < top.size() && i < 12; ++i)
                    std::fprintf(stderr, "[rip:window] t=%lld rva=0x%llx n=%u\n",
                                 static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(lastDump.time_since_epoch()).count() % 100000),
                                 static_cast<unsigned long long>(top[i].first), top[i].second);
                std::fflush(stderr);
                window.clear();
                {
                    std::vector<std::pair<uint64_t, uint32_t>> wc(g_windowChains.begin(), g_windowChains.end());
                    std::sort(wc.begin(), wc.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    for (size_t i = 0; i < wc.size() && i < 30; ++i)
                    {
                        std::fprintf(stderr, "[rip:wchain] t=%lld n=%u", static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(lastDump.time_since_epoch()).count() % 100000), wc[i].second);
                        for (const uint64_t f : g_chainFrames[wc[i].first])
                            std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(f));
                        std::fprintf(stderr, "\n");
                    }
                    g_windowChains.clear();
                }
                std::vector<std::pair<uint64_t, uint32_t>> all(allWindow.begin(), allWindow.end());
                std::sort(all.begin(), all.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                for (size_t i = 0; i < all.size() && i < 40; ++i)
                    std::fprintf(stderr, "[rip:thr] tid=%llu rva=0x%llx n=%u\n", static_cast<unsigned long long>(all[i].first >> 40),
                                 static_cast<unsigned long long>(all[i].first & 0xFFFFFFFFFFull), all[i].second);
                std::fflush(stderr);
                allWindow.clear();
            }
            // PS2X_RIP_ALL=1: also sample every other thread of the process (host UI, GS worker, ...) every ~20 ms.
            static const bool sampleAll = std::getenv("PS2X_RIP_ALL") != nullptr;
            if (sampleAll)
            {
                static int divider = 0;
                if ((++divider % 20) == 0)
                {
                    const DWORD self = GetCurrentThreadId();
                    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                    if (snap != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 te;
                        te.dwSize = sizeof(te);
                        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
                        {
                            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
                                continue;
                            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                            if (!th)
                                continue;
                            if (SuspendThread(th) != static_cast<DWORD>(-1))
                            {
                                CONTEXT c;
                                c.ContextFlags = CONTEXT_CONTROL;
                                const BOOL got = GetThreadContext(th, &c);
                                ResumeThread(th);
                                if (got && c.Rip >= base)
                                    ++allWindow[(static_cast<uint64_t>(te.th32ThreadID) << 40) | (c.Rip - base)];
                            }
                            CloseHandle(th);
                        }
                        CloseHandle(snap);
                    }
                }
            }
            HANDLE h = static_cast<HANDLE>(threadHandle);
            if (SuspendThread(h) != static_cast<DWORD>(-1))
            {
                CONTEXT ctx;
                ctx.ContextFlags = CONTEXT_CONTROL;
                const BOOL ok = GetThreadContext(h, &ctx);
                const uint64_t rip = ok ? ctx.Rip : 0;
                ResumeThread(h);
                if (rip >= base)
                {
                    ++g_histogram[rip - base];
                    ++window[rip - base];
                }
                // Sample outside the executable (a system DLL): remember which function of the executable called into it.
                // The thread is suspended again briefly to read the stack words above RSP and take the first one that points into the image.
                static const uint64_t imageSize = [base]() -> uint64_t {
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
                    return nt->OptionalHeader.SizeOfImage;
                }();
                if (ok && (rip < base || rip - base >= imageSize))
                {
                    if (SuspendThread(h) != static_cast<DWORD>(-1))
                    {
                        CONTEXT c2;
                        c2.ContextFlags = CONTEXT_FULL;
                        if (GetThreadContext(h, &c2))
                        {
                            // Correct x64 unwinding with the unwind tables (no symbols needed): keep the first 5 frames inside the image.
                            std::vector<uint64_t> frames;
                            for (int depth = 0; depth < 40 && frames.size() < 5 && c2.Rip != 0; ++depth)
                            {
                                if (c2.Rip >= base && c2.Rip - base < imageSize)
                                    frames.push_back(c2.Rip - base);
                                DWORD64 imageBase = 0;
                                RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(c2.Rip, &imageBase, nullptr);
                                if (!entry)
                                    break;
                                void *handlerData = nullptr;
                                DWORD64 establisher = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c2.Rip, entry, &c2, &handlerData, &establisher, nullptr);
                            }
                            if (!frames.empty())
                            {
                                ++g_callers[frames[0]];
                                uint64_t key = 1469598103934665603ull;
                                for (const uint64_t f : frames)
                                    key = (key ^ f) * 1099511628211ull;
                                ++g_chains[key];
                                ++g_windowChains[key];
                                g_chainFrames.emplace(key, frames);
                            }
                        }
                        ResumeThread(h);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
#else
    (void)threadHandle;
#endif
}

void ps2StopRipProfiler()
{
    if (!g_thread.joinable())
        return;
    g_stop.store(true);
    g_thread.join();
    std::vector<std::pair<uint64_t, uint32_t>> rows(g_histogram.begin(), g_histogram.end());
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    uint64_t total = 0;
    for (const auto &r : rows)
        total += r.second;
    std::fprintf(stderr, "[rip] samples=%llu\n", static_cast<unsigned long long>(total));
    for (size_t i = 0; i < rows.size() && i < 400; ++i)
        std::fprintf(stderr, "[rip] rva=0x%llx n=%u\n", static_cast<unsigned long long>(rows[i].first), rows[i].second);
    {
        std::vector<std::pair<uint64_t, uint32_t>> chains(g_chains.begin(), g_chains.end());
        std::sort(chains.begin(), chains.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < chains.size() && i < 40; ++i)
        {
            std::fprintf(stderr, "[rip:chain] n=%u", chains[i].second);
            for (const uint64_t f : g_chainFrames[chains[i].first])
                std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(f));
            std::fprintf(stderr, "\n");
        }
    }
    {
        std::vector<std::pair<uint64_t, uint32_t>> callers(g_callers.begin(), g_callers.end());
        std::sort(callers.begin(), callers.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < callers.size() && i < 60; ++i)
            std::fprintf(stderr, "[rip:caller] rva=0x%llx n=%u\n", static_cast<unsigned long long>(callers[i].first), callers[i].second);
    }
#ifdef _WIN32
    // Addresses outside the executable (system DLLs, the C runtime): name the module and the offset inside it.
    {
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        for (size_t i = 0; i < rows.size() && i < 12; ++i)
        {
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(base + rows[i].first), &module) || module == nullptr)
                continue;
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            char narrow[MAX_PATH] = {};
            WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
            if (module != reinterpret_cast<HMODULE>(base))
                std::fprintf(stderr, "[rip:mod] rva=0x%llx n=%u module=%s offset=0x%llx\n", static_cast<unsigned long long>(rows[i].first), rows[i].second,
                             narrow, static_cast<unsigned long long>(base + rows[i].first - reinterpret_cast<uint64_t>(module)));
        }
    }
#endif
    std::fflush(stderr);
}
