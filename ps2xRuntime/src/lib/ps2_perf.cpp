// Implementation of ps2_perf.h: accumulators, report window (stderr block + CSV row), process/thread CPU times. Nothing here runs unless PS2X_PERF is set.
#include "ps2_perf.h"
#include "ps2_perf_irq.h"
#if __has_include("ps2x_diag.hpp")
#include "ps2x_diag.hpp" // parallel-gs diagnostic counters (third_party_patches/parallel-gs/experimental/0006); without that patch the columns stay 0
#define PS2X_HAVE_GS_DIAG 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace ps2perf
{
    namespace
    {
        int readLevel()
        {
            const char *v = std::getenv("PS2X_PERF");
            if (!v || !v[0] || v[0] == '0')
                return 0;
            return v[0] == '2' ? 2 : 1;
        }
    } // namespace

    int g_level = readLevel();
    bool g_on = g_level > 0;
    std::atomic<uint64_t> g_ns[int(S::Count)];
    std::atomic<uint64_t> g_cnt[int(S::Count)];
    std::atomic<uint64_t> g_bytes[int(S::Count)];
    std::atomic<uint64_t> g_sifEmitted{0};
    std::atomic<uint64_t> g_sifSuppressed{0};
    IrqTableStats g_irq[2];
    GsFlushStats g_gsFlush;

    const char *name(S s)
    {
        switch (s)
        {
        case S::EeGsFrontend: return "ee.gs_frontend_total";
        case S::EeFrontendLock: return "ee.wait_frontend_mutex";
        case S::EeTapLock: return "ee.wait_backend_mutex";
        case S::EeTapGif: return "ee.pgs_gif_transfer";
        case S::EeTapRegister: return "ee.pgs_write_register";
        case S::EeTapImage: return "ee.pgs_image_upload";
        case S::EeAsyncWait: return "ee.wait_async_queue";
        case S::MainLoopBody: return "main.loop_busy";
        case S::MainUpload: return "main.upload_frame";
        case S::MainPresentLock: return "main.wait_backend_mutex";
        case S::MainFlushSync: return "main.flush_sync";
        case S::MainBackendPresent: return "main.backend_present";
        case S::MainMirror: return "main.mirror_vram";
        case S::MainVsync: return "main.pgs_vsync_call";
        case S::MainWaitIdle: return "main.wait_gpu_idle";
        case S::MainHostCopy: return "main.host_copy";
        case S::MainFrameCopy: return "main.frame_copy_latched";
        case S::MainTexUpdate: return "main.gl_texture_update";
        case S::MainCpuPresent: return "main.cpu_crtc_present";
        case S::MainDraw: return "main.raylib_draw";
        case S::MainEndDrawing: return "main.enddrawing_incl_pacing";
        case S::PushVram: return "push_vram";
        case S::GpuShading: return "gpu.shading";
        case S::GpuBinning: return "gpu.binning";
        case S::GpuSetup: return "gpu.triangle_setup";
        case S::GpuVsync: return "gpu.vsync";
        case S::GpuTexUpload: return "gpu.texture_upload";
        case S::GpuCopyVram: return "gpu.copy_vram";
        case S::GpuReadback: return "gpu.pgs_readback";
        case S::GpuPresentCopy: return "gpu.present_blit_copy";
        default: return "?";
        }
    }

    namespace
    {
        std::mutex g_infoMutex;
        std::map<std::string, std::string> g_info;

        struct Window
        {
            bool started = false;
            uint64_t startNs = 0, lastLoopNs = 0, startTick = 0;
            uint32_t loops = 0;
            std::vector<double> periods;
            uint64_t prevNs[int(S::Count)] = {}, prevCnt[int(S::Count)] = {}, prevBytes[int(S::Count)] = {};
            uint64_t prevProcCpu100ns = 0;
            std::map<uint32_t, uint64_t> prevThread;
            uint32_t windows = 0;
            uint64_t firstNs = 0; // start of the first window: t_end_s counts from here
            bool csvHeader = false;
        } g_w;

        double windowSeconds()
        {
            static const double v = [] {
                const char *t = std::getenv("PS2X_PERF_WINDOW");
                const double d = t ? std::atof(t) : 5.0;
                return d >= 1.0 ? d : 5.0;
            }();
            return v;
        }

        struct ThreadCpu
        {
            std::string name;
            double pct = 0;
        };

#if defined(_WIN32)
        uint64_t ft64(const FILETIME &f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

        uint64_t processCpu100ns()
        {
            FILETIME c, e, k, u;
            if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
                return 0;
            return ft64(k) + ft64(u);
        }

        std::vector<ThreadCpu> threadCpu(double wallSeconds)
        {
            std::vector<ThreadCpu> out;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return out;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            const DWORD pid = GetCurrentProcessId();
            std::map<uint32_t, uint64_t> now;
            if (Thread32First(snap, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != pid)
                        continue;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (!h)
                        continue;
                    FILETIME c, e, k, u;
                    if (GetThreadTimes(h, &c, &e, &k, &u))
                    {
                        const uint64_t total = ft64(k) + ft64(u);
                        now[te.th32ThreadID] = total;
                        const auto it = g_w.prevThread.find(te.th32ThreadID);
                        const uint64_t before = it == g_w.prevThread.end() ? 0u : it->second;
                        const double pct = 100.0 * double(total - before) / 1.0e7 / wallSeconds;
                        if (pct >= 0.5)
                        {
                            ThreadCpu t;
                            t.pct = pct;
                            PWSTR desc = nullptr;
                            if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc)
                            {
                                char buf[96] = {};
                                WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, int(sizeof(buf)) - 1, nullptr, nullptr);
                                t.name = buf;
                                LocalFree(desc);
                            }
                            if (t.name.empty())
                                t.name = "tid" + std::to_string(te.th32ThreadID);
                            out.push_back(std::move(t));
                        }
                    }
                    CloseHandle(h);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
            g_w.prevThread = std::move(now);
            std::sort(out.begin(), out.end(), [](const ThreadCpu &a, const ThreadCpu &b) { return a.pct > b.pct; });
            return out;
        }
#else
        uint64_t processCpu100ns() { return 0; }
        std::vector<ThreadCpu> threadCpu(double) { return {}; }
#endif


        // PS2X_PERF_SAMPLE=1: samples the game thread (the one that runs the recompiled code and the emulated EE/IOP) every 2 ms and counts WHERE its instruction
        // pointer is, by module (the game exe, ntdll/kernelbase = blocked in a wait or a system call, the GPU driver DLL, the C runtime ...). For samples inside the
        // system DLLs it also keeps the first return address inside the exe (offset to resolve with tools/perf/resolve_map.py and the .map of the same build): who
        // is waiting. It answers "is the EE thread running, or waiting for something that is not instrumented?" per report window. Only the thread context is read
        // while the thread is suspended (no allocation, no locks); everything else happens after it resumes.
#if defined(_WIN32)
        struct Sampler
        {
            std::mutex m;
            std::map<std::string, uint32_t> modules;
            std::map<uint64_t, uint32_t> waitCallers;
            struct FuncStat
            {
                uint32_t cpuSelf = 0; // sample's RIP was inside this function (running its own code)
                uint32_t extSelf = 0; // RIP was outside the exe (system call, wait, driver, C runtime) and this was the innermost exe function on the stack
                uint32_t incl = 0;    // this function was anywhere on the stack
            };
            std::unordered_map<uint64_t, FuncStat> funcs; // PS2X_PERF_SAMPLE=2: function start RVA -> counts (per window)
            std::unordered_map<std::string, uint32_t> sysFns; // PS2X_PERF_SAMPLE=2: "module!nearest export" where the thread was when outside the exe
            std::unordered_map<std::string, uint32_t> chains; // PS2X_PERF_SAMPLE=2: innermost exe functions (start RVAs, "a>b>c>d") above an outside-the-exe sample
            std::map<uintptr_t, std::vector<std::pair<uint32_t, std::string>>> exports; // module base -> (export RVA, name) sorted; sampler thread only
            // PS2X_PERF_SAMPLE=2 (0016): wait episodes = runs of consecutive samples (2 ms apart) that were in a system wait with the same game call chain. Per chain:
            // histogram of the run length in samples (1, 2, 3-4, 5-8, 9-16, >16). A wait shorter than the sampling period can be missed or counted as 1.
            std::unordered_map<std::string, std::array<uint32_t, 6>> waitHist;
            std::string waitKey;
            uint32_t waitRun = 0;
            bool deep = false;
            uint32_t total = 0;
            std::atomic<bool> stop{false};
            std::thread thread;
            std::map<uintptr_t, std::string> names; // allocation base -> module file name (only the sampler thread touches it)
        } g_samp;

        struct Range
        {
            uint64_t lo = 0, hi = 0;
        };
        Range moduleRange(const wchar_t *name)
        {
            Range r;
            HMODULE h = GetModuleHandleW(name);
            if (!h)
                return r;
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(h);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(reinterpret_cast<const uint8_t *>(h) + dos->e_lfanew);
            r.lo = reinterpret_cast<uint64_t>(h);
            r.hi = r.lo + nt->OptionalHeader.SizeOfImage;
            return r;
        }

        std::string moduleNameAt(uint64_t address)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void *>(address), &mbi, sizeof(mbi)) || !mbi.AllocationBase)
                return "other";
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            auto it = g_samp.names.find(base);
            if (it != g_samp.names.end())
                return it->second;
            std::string name = "other";
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(reinterpret_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH))
            {
                char utf8[MAX_PATH * 2] = {};
                WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, int(sizeof(utf8)) - 1, nullptr, nullptr);
                name = utf8;
                const size_t slash = name.find_last_of("\\/");
                if (slash != std::string::npos)
                    name = name.substr(slash + 1);
            }
            g_samp.names[base] = name;
            return name;
        }

        // "kernelbase.dll!WriteFile+0x42": the nearest exported symbol at or before `address` (exports only; no debug symbols needed).
        std::string exportNameAt(uint64_t address)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void *>(address), &mbi, sizeof(mbi)) || !mbi.AllocationBase)
                return "?";
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            auto it = g_samp.exports.find(base);
            if (it == g_samp.exports.end())
            {
                std::vector<std::pair<uint32_t, std::string>> list;
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
                if (dos->e_magic == 0x5A4D)
                {
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
                    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                    if (dir.VirtualAddress && dir.Size)
                    {
                        const auto *ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + dir.VirtualAddress);
                        const auto *names = reinterpret_cast<const DWORD *>(base + ex->AddressOfNames);
                        const auto *ords = reinterpret_cast<const WORD *>(base + ex->AddressOfNameOrdinals);
                        const auto *funcs = reinterpret_cast<const DWORD *>(base + ex->AddressOfFunctions);
                        for (DWORD i = 0; i < ex->NumberOfNames; ++i)
                        {
                            const DWORD rva = funcs[ords[i]];
                            if (rva < dir.VirtualAddress || rva >= dir.VirtualAddress + dir.Size) // skip forwarders
                                list.emplace_back(rva, reinterpret_cast<const char *>(base + names[i]));
                        }
                        std::sort(list.begin(), list.end());
                    }
                }
                it = g_samp.exports.emplace(base, std::move(list)).first;
            }
            const uint32_t rva = static_cast<uint32_t>(address - base);
            const auto &list = it->second;
            auto pos = std::upper_bound(list.begin(), list.end(), std::make_pair(rva, std::string(1, '\xff')));
            std::string mod = moduleNameAt(address);
            if (pos == list.begin())
                return mod + "!+0x" + [&] { char b[16]; std::snprintf(b, sizeof(b), "%x", rva); return std::string(b); }();
            --pos;
            char off[24];
            std::snprintf(off, sizeof(off), "+0x%x", rva - pos->first);
            return mod + "!" + pos->second + off;
        }

        void samplerLoop(HANDLE h)
        {
            const Range exe = moduleRange(nullptr);
            const Range sys[4] = {moduleRange(L"ntdll.dll"), moduleRange(L"kernelbase.dll"), moduleRange(L"win32u.dll"), moduleRange(L"kernel32.dll")};
            while (!g_samp.stop.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                uint64_t rip = 0, caller = 0;
                uint64_t fr[64];
                int nfr = 0;
                bool cpuSample = false;
                if (SuspendThread(h) == static_cast<DWORD>(-1))
                    continue;
                CONTEXT c;
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(h, &c))
                {
                    rip = c.Rip;
                    bool inSys = false;
                    for (const Range &r : sys)
                        inSys = inSys || (rip >= r.lo && rip < r.hi);
                    cpuSample = rip >= exe.lo && rip < exe.hi;
                    if (inSys || g_samp.deep)
                    {
                        // Level 1: first return address inside the exe (who is waiting). Level 2: the whole stack as exe function start RVAs (self + inclusive).
                        for (int depth = 0; depth < 64 && c.Rip != 0 && (g_samp.deep || !caller); ++depth)
                        {
                            DWORD64 imageBase = 0;
                            RUNTIME_FUNCTION *entry = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                            if (c.Rip >= exe.lo && c.Rip < exe.hi)
                            {
                                if (!caller)
                                    caller = c.Rip - exe.lo;
                                if (g_samp.deep && nfr < 64)
                                    fr[nfr++] = (entry ? imageBase + entry->BeginAddress : c.Rip) - exe.lo;
                                if (!g_samp.deep)
                                    break;
                            }
                            if (!entry)
                            {
                                // frame 0 in a leaf function without unwind data: the return address is at the top of the stack
                                uint64_t ret = 0;
                                SIZE_T got = 0;
                                if (depth != 0 || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(c.Rsp), &ret, sizeof(ret), &got) || got != sizeof(ret))
                                    break;
                                c.Rip = ret;
                                c.Rsp += 8;
                                continue;
                            }
                            void *handlerData = nullptr;
                            DWORD64 establisher = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, entry, &c, &handlerData, &establisher, nullptr);
                        }
                    }
                }
                ResumeThread(h);
                if (!rip)
                    continue;
                const std::string name = moduleNameAt(rip);
                std::lock_guard<std::mutex> lock(g_samp.m);
                ++g_samp.modules[name];
                ++g_samp.total;
                if (caller)
                    ++g_samp.waitCallers[caller];
                bool inWait = false;
                std::string waitKeyNow;
                if (g_samp.deep && !cpuSample)
                {
                    // Outside the exe: which system / driver function, and which game functions are above it.
                    std::string sysName = exportNameAt(rip), chain;
                    char b[24];
                    for (int i = 0; i < nfr && i < 6; ++i)
                    {
                        std::snprintf(b, sizeof(b), "%s%llx", i ? ">" : "", static_cast<unsigned long long>(fr[i]));
                        chain += b;
                    }
                    ++g_samp.sysFns[sysName];
                    if (!chain.empty())
                        ++g_samp.chains[chain];
                    if (!chain.empty() && sysName.find("Wait") != std::string::npos)
                    {
                        inWait = true;
                        waitKeyNow = sysName + "|" + chain;
                    }
                }
                if (g_samp.deep)
                {
                    if (inWait && g_samp.waitRun && waitKeyNow == g_samp.waitKey)
                        ++g_samp.waitRun;
                    else
                    {
                        if (g_samp.waitRun)
                        {
                            const uint32_t n = g_samp.waitRun;
                            auto &hist = g_samp.waitHist[g_samp.waitKey.substr(g_samp.waitKey.find('|') + 1)];
                            ++hist[n == 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5];
                        }
                        g_samp.waitRun = inWait ? 1u : 0u;
                        g_samp.waitKey = waitKeyNow;
                    }
                }
                for (int i = 0; i < nfr; ++i)
                {
                    bool seen = false;
                    for (int j = 0; j < i && !seen; ++j)
                        seen = fr[j] == fr[i];
                    if (seen)
                        continue;
                    auto &st = g_samp.funcs[fr[i]];
                    ++st.incl;
                    if (i == 0)
                        ++(cpuSample ? st.cpuSelf : st.extSelf);
                }
            }
        }
#endif

        // "module pct;module pct | waits_from(pct of samples) 0xRVA:pct;..." for the window that just ended; resets the counters.
        std::string csvPath();
        std::string sampleSummary(unsigned windowIndex)
        {
#if defined(_WIN32)
            std::map<std::string, uint32_t> modules;
            std::map<uint64_t, uint32_t> callers;
            std::unordered_map<uint64_t, Sampler::FuncStat> funcs;
            std::unordered_map<std::string, uint32_t> sysFns, chains;
            std::unordered_map<std::string, std::array<uint32_t, 6>> waitHist;
            uint32_t total = 0;
            {
                std::lock_guard<std::mutex> lock(g_samp.m);
                waitHist.swap(g_samp.waitHist);
                modules.swap(g_samp.modules);
                callers.swap(g_samp.waitCallers);
                funcs.swap(g_samp.funcs);
                sysFns.swap(g_samp.sysFns);
                chains.swap(g_samp.chains);
                total = g_samp.total;
                g_samp.total = 0;
            }
            if (!total)
                return std::string();
            std::vector<std::pair<std::string, uint32_t>> mv(modules.begin(), modules.end());
            std::sort(mv.begin(), mv.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::string out;
            char b[160];
            for (size_t i = 0; i < mv.size() && i < 6; ++i)
            {
                std::snprintf(b, sizeof(b), "%s%s %.0f", i ? ";" : "", mv[i].first.c_str(), 100.0 * mv[i].second / total);
                out += b;
            }
            std::vector<std::pair<uint64_t, uint32_t>> cv(callers.begin(), callers.end());
            std::sort(cv.begin(), cv.end(), [](const auto &a, const auto &b2) { return a.second > b2.second; });
            for (size_t i = 0; i < cv.size() && i < 4; ++i)
            {
                std::snprintf(b, sizeof(b), "%s0x%llx:%.0f", i ? ";" : " | waits_from(pct of samples) ", static_cast<unsigned long long>(cv[i].first), 100.0 * cv[i].second / total);
                out += b;
            }
            std::snprintf(b, sizeof(b), " [n=%u]", total);
            if (g_samp.deep && !funcs.empty())
            {
                // <csv>.hot.csv: per window, the hottest exe functions (function start RVA; resolve with tools/perf/hot_report.py + the .map of this build).
                // kind: cpu = % of samples whose RIP was in that function; ext = % in system/driver code called from it (waits, calls); incl = % with it anywhere on the stack.
                const std::string hotPath = csvPath() + ".hot.csv";
                if (FILE *hf = std::fopen(hotPath.c_str(), "ab"))
                {
                    std::fseek(hf, 0, SEEK_END);
                    if (std::ftell(hf) == 0)
                        std::fprintf(hf, "label,window,samples,kind,rva,pct\n");
                    const char *label = std::getenv("PS2X_PERF_LABEL");
                    struct Pick
                    {
                        const char *kind;
                        uint32_t Sampler::FuncStat::*field;
                        size_t keep;
                    };
                    const Pick picks[3] = {{"cpu", &Sampler::FuncStat::cpuSelf, 40}, {"ext", &Sampler::FuncStat::extSelf, 25}, {"incl", &Sampler::FuncStat::incl, 60}};
                    for (const Pick &pk : picks)
                    {
                        std::vector<std::pair<uint64_t, uint32_t>> v;
                        for (const auto &kv : funcs)
                            if (kv.second.*pk.field)
                                v.emplace_back(kv.first, kv.second.*pk.field);
                        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
                        for (size_t i = 0; i < v.size() && i < pk.keep; ++i)
                            std::fprintf(hf, "%s,%u,%u,%s,%llx,%.2f\n", label ? label : "", windowIndex, total, pk.kind, static_cast<unsigned long long>(v[i].first), 100.0 * v[i].second / total);
                    }
                    // kind sys: where the thread was outside the exe ("module!export+off", the name is in the rva column);
                    // kind chain: the innermost game functions above such a sample ("a>b>c>d", start RVAs, innermost first).
                    for (int k = 0; k < 2; ++k)
                    {
                        const auto &src = k == 0 ? sysFns : chains;
                        std::vector<std::pair<std::string, uint32_t>> v(src.begin(), src.end());
                        std::sort(v.begin(), v.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
                        for (size_t i = 0; i < v.size() && i < (k == 0 ? 20u : 30u); ++i)
                            std::fprintf(hf, "%s,%u,%u,%s,%s,%.2f\n", label ? label : "", windowIndex, total, k == 0 ? "sys" : "chain", v[i].first.c_str(), 100.0 * v[i].second / total);
                    }
                    // kind waitep: wait episodes per game call chain: "chain|n1/n2/n3-4/n5-8/n9-16/n>16" (episodes by run length in samples), pct = % of samples inside those episodes
                    {
                        std::vector<std::pair<std::string, std::array<uint32_t, 6>>> v(waitHist.begin(), waitHist.end());
                        const auto weight = [](const std::array<uint32_t, 6> &h) { return h[0] + 2u * h[1] + 3u * h[2] + 6u * h[3] + 12u * h[4] + 20u * h[5]; };
                        std::sort(v.begin(), v.end(), [&](const auto &x, const auto &y) { return weight(x.second) > weight(y.second); });
                        for (size_t i = 0; i < v.size() && i < 8; ++i)
                            std::fprintf(hf, "%s,%u,%u,waitep,%s|%u/%u/%u/%u/%u/%u,%.2f\n", label ? label : "", windowIndex, total, v[i].first.c_str(), v[i].second[0], v[i].second[1], v[i].second[2],
                                         v[i].second[3], v[i].second[4], v[i].second[5], 100.0 * weight(v[i].second) / total);
                    }
                    std::fclose(hf);
                }
            }
            return out + b;
#else
            return std::string();
#endif
        }

        std::string csvPath()
        {
            if (const char *p = std::getenv("PS2X_PERF_CSV"))
                if (p[0])
                    return p;
            const char *appdata = std::getenv("APPDATA");
            return std::string(appdata ? appdata : ".") + "\\ShiningWind\\perf.csv";
        }

        std::string info(const char *key)
        {
            std::lock_guard<std::mutex> lock(g_infoMutex);
            const auto it = g_info.find(key);
            return it == g_info.end() ? std::string() : it->second;
        }

        double pct(std::vector<double> &v, double q)
        {
            if (v.empty())
                return 0.0;
            const size_t i = std::min(v.size() - 1, size_t(q * double(v.size())));
            return v[i];
        }

        void closeWindow(uint64_t now, uint64_t tick, uint32_t fw, uint32_t fh)
        {
            const double wall = double(now - g_w.startNs) / 1.0e9;
            if (wall <= 0.0)
                return;
            std::vector<double> sorted = g_w.periods;
            std::sort(sorted.begin(), sorted.end());
            uint32_t late20 = 0, late33 = 0;
            for (double p : sorted)
            {
                late20 += p > 20.0;
                late33 += p > 33.4;
            }
            const double hostFps = g_w.loops / wall;
            const double gameTicks = double(tick - g_w.startTick);
            const double gameSpeed = 100.0 * (gameTicks / wall) / 60.0; // NTSC fields: the guest VBlank counter advances 60 times per second at full speed
            const uint64_t procNow = processCpu100ns();
            const double procPct = 100.0 * double(procNow - g_w.prevProcCpu100ns) / 1.0e7 / wall;
            g_w.prevProcCpu100ns = procNow;
            const std::vector<ThreadCpu> threads = threadCpu(wall);

            double busy[int(S::Count)], perCall[int(S::Count)], callsPerS[int(S::Count)];
            uint64_t dBytes[int(S::Count)];
            for (int i = 0; i < int(S::Count); ++i)
            {
                const uint64_t ns = g_ns[i].load(std::memory_order_relaxed), cnt = g_cnt[i].load(std::memory_order_relaxed), by = g_bytes[i].load(std::memory_order_relaxed);
                const uint64_t dns = ns - g_w.prevNs[i], dcnt = cnt - g_w.prevCnt[i];
                dBytes[i] = by - g_w.prevBytes[i];
                g_w.prevNs[i] = ns;
                g_w.prevCnt[i] = cnt;
                g_w.prevBytes[i] = by;
                busy[i] = 100.0 * double(dns) / 1.0e9 / wall;
                perCall[i] = dcnt ? double(dns) / 1.0e6 / double(dcnt) : 0.0;
                callsPerS[i] = double(dcnt) / wall;
            }
            ++g_w.windows;
            const double tEnd = double(now - g_w.firstNs) / 1.0e9;
            if (gameSpeed < 10.0)
                std::fprintf(stderr, "[perf] WARNING: the guest advanced only %.1f%% of real time in this window: the game is stalled or not running (boot/disc files missing, loading). The metrics of this window say nothing about the renderer.\n", gameSpeed);

            std::fprintf(stderr,
                         "[perf] #%u t=%.0fs %.1fs  scale=%s output=%s  frame=%ux%u  host loops %.1f/s  game speed %.0f%% (%.1f VBlank/s)  loop period ms: p50 %.1f p95 %.1f p99 %.1f max %.1f  >20ms:%u >33ms:%u  process CPU %.0f%% of one core\n",
                         g_w.windows, tEnd, wall, info("scale").c_str(), info("output").c_str(), fw, fh, hostFps, gameSpeed, gameTicks / wall, pct(sorted, 0.50), pct(sorted, 0.95), pct(sorted, 0.99),
                         sorted.empty() ? 0.0 : sorted.back(), late20, late33, procPct);
            for (int i = 0; i < int(S::Count); ++i)
            {
                if (!g_cnt[i].load(std::memory_order_relaxed) && busy[i] == 0.0)
                    continue;
                if (callsPerS[i] == 0.0)
                    continue;
                std::fprintf(stderr, "[perf]   %-30s busy %5.1f%% of wall  %8.0f calls/s  %7.3f ms/call", name(S(i)), busy[i], callsPerS[i], perCall[i]);
                if (dBytes[i])
                    std::fprintf(stderr, "  %.1f MiB/s", double(dBytes[i]) / 1048576.0 / wall);
                std::fprintf(stderr, "\n");
            }
            std::string threadStr;
            for (size_t i = 0; i < threads.size() && i < 8; ++i)
            {
                char b[128];
                std::snprintf(b, sizeof(b), "%s%s:%.0f", i ? ";" : "", threads[i].name.c_str(), threads[i].pct);
                threadStr += b;
            }
            std::fprintf(stderr, "[perf]   threads (CPU%% of one core): %s\n", threadStr.c_str());
            static uint64_t prevSifEmitted = 0, prevSifSuppressed = 0;
            const uint64_t sifE = g_sifEmitted.load(std::memory_order_relaxed), sifS = g_sifSuppressed.load(std::memory_order_relaxed);
            const double sifEps = double(sifE - prevSifEmitted) / wall, sifSps = double(sifS - prevSifSuppressed) / wall;
            prevSifEmitted = sifE;
            prevSifSuppressed = sifS;
            std::fprintf(stderr, "[perf]   SIF trace lines: written %.0f/s, suppressed %.0f/s (PS2X_SIF_TRACE=%s)\n", sifEps, sifSps, sifTraceEnabled() ? "on" : "0");
            // EE interrupt-handler tables (0014): does the DMAC table keep growing? per-window rates, current/max size and the work each IRQ does.
            static uint64_t prevAdds[2] = {0, 0}, prevRem[2] = {0, 0}, prevDisp[2] = {0, 0}, prevIter[2] = {0, 0}, prevMatch[2] = {0, 0}, prevSifCalls = 0, prevSifMatch = 0;
            static uint64_t prevCause[2][16] = {};
            char irqCols[768];
            {
                const IrqTableStats &d = g_irq[1], &n = g_irq[0];
                const uint64_t dAdd = d.adds.load(), dRem = d.removes.load(), dDisp = d.dispatches.load(), dIter = d.iterated.load(), dMatch = d.matched.load();
                const uint64_t sCalls = d.sifCalls.load(), sMatch = d.sifMatched.load();
                const double dDispW = double(dDisp - prevDisp[1]), sCallsW = double(sCalls - prevSifCalls);
                std::string byCause;
                for (int c = 0; c < 16; ++c)
                    if (const uint32_t v = d.byCause[c].load(std::memory_order_relaxed); v)
                    {
                        char b[24];
                        std::snprintf(b, sizeof(b), "%sc%d=%u", byCause.empty() ? "" : "/", c, v);
                        byCause += b;
                    }
                // IRQs/s per cause: DMAC (d) and INTC (i), causes < 16 with >= 1/s
                std::string perCause;
                for (int t = 1; t >= 0; --t)
                    for (int c = 0; c < 16; ++c)
                    {
                        const uint64_t cur = g_irq[t].dispatchByCause[c].load(std::memory_order_relaxed);
                        const double rate = double(cur - prevCause[t][c]) / wall;
                        prevCause[t][c] = cur;
                        if (rate >= 1.0)
                        {
                            char b[40];
                            std::snprintf(b, sizeof(b), "%s%c%d=%.0f", perCause.empty() ? "" : "/", t ? 'd' : 'i', c, rate);
                            perCause += b;
                        }
                    }
                std::fprintf(stderr, "[perf]   irq/s per cause: %s\n", perCause.empty() ? "-" : perCause.c_str());
                std::fprintf(stderr, "[perf]   irq handlers DMAC: active %u (max %u; by cause %s) +%.1f/s -%.1f/s; %.1f IRQ/s walk %.1f entries and queue %.2f invocations each (max %u); SIF packets %.1f/s queue %.2f each | INTC: active %u (max %u)\n",
                             d.active.load(), d.maxActive.load(), byCause.empty() ? "-" : byCause.c_str(), double(dAdd - prevAdds[1]) / wall, double(dRem - prevRem[1]) / wall, dDispW / wall,
                             dDispW > 0 ? double(dIter - prevIter[1]) / dDispW : 0.0, dDispW > 0 ? double(dMatch - prevMatch[1]) / dDispW : 0.0, d.maxMatched.load(),
                             sCallsW / wall, sCallsW > 0 ? double(sMatch - prevSifMatch) / sCallsW : 0.0, n.active.load(), n.maxActive.load());
                std::snprintf(irqCols, sizeof(irqCols), "%u,%u,%.2f,%.2f,%.2f,%.2f,%.3f,%u,%.2f,%.3f,%u,%u,\"%s\",\"%s\"", d.active.load(), d.maxActive.load(), double(dAdd - prevAdds[1]) / wall,
                              double(dRem - prevRem[1]) / wall, dDispW / wall, dDispW > 0 ? double(dIter - prevIter[1]) / dDispW : 0.0, dDispW > 0 ? double(dMatch - prevMatch[1]) / dDispW : 0.0,
                              d.maxMatched.load(), sCallsW / wall, sCallsW > 0 ? double(sMatch - prevSifMatch) / sCallsW : 0.0, n.active.load(), n.maxActive.load(), byCause.c_str(), perCause.c_str());
                prevAdds[1] = dAdd; prevRem[1] = dRem; prevDisp[1] = dDisp; prevIter[1] = dIter; prevMatch[1] = dMatch; prevSifCalls = sCalls; prevSifMatch = sMatch;
            }
            // parallel-gs CPU/GPU synchronisation (0016): how often the feeding thread waits for the GPU and for how long; what triggers the flushes.
            char gsCols[900];
            {
                static uint64_t pSub = 0, pNfc = 0, pNfcNs = 0, pOver[4] = {0, 0, 0, 0}, pTl = 0, pTlNs = 0, pOrg[4] = {0, 0, 0, 0}, pPass[8] = {}, pNon[8] = {}, pPrim[8] = {};
                static uint64_t pFp = 0, pFr = 0, pFpal = 0, pFc = 0, pFb = 0, pFn = 0;
                double nfcRate = 0, nfcPct = 0, nfcAvg = 0, nfcMax = 0, over[4] = {0, 0, 0, 0}, tlRate = 0, tlPct = 0, subRate = 0, org[4] = {0, 0, 0, 0};
                std::string passes;
#if defined(PS2X_HAVE_GS_DIAG)
                {
                    auto &d = ps2xdiag::diag();
                    const uint64_t sub = d.submitCalls.load(), nfc = d.nfcCalls.load(), nfcNs = d.nfcNs.load(), tl = d.tlCalls.load(), tlNs = d.tlNs.load();
                    nfcRate = double(nfc - pNfc) / wall;
                    nfcPct = 100.0 * double(nfcNs - pNfcNs) / 1e9 / wall;
                    nfcAvg = nfc > pNfc ? double(nfcNs - pNfcNs) / 1e6 / double(nfc - pNfc) : 0.0;
                    nfcMax = double(d.nfcMaxNs.exchange(0)) / 1e6;
                    for (int i = 0; i < 4; ++i)
                    {
                        const uint64_t o = d.nfcOver[i].load();
                        over[i] = double(o - pOver[i]) / wall;
                        pOver[i] = o;
                        const uint64_t g = d.submitByOrigin[i].load();
                        org[i] = double(g - pOrg[i]) / wall;
                        pOrg[i] = g;
                    }
                    tlRate = double(tl - pTl) / wall;
                    tlPct = 100.0 * double(tlNs - pTlNs) / 1e9 / wall;
                    const double tlMax = double(d.tlMaxNs.exchange(0)) / 1e6;
                    subRate = double(sub - pSub) / wall;
                    static const char *const kReason[ps2xdiag::kReasons] = {"FBPointer", "Overflow", "TextureHazard", "CopyHazard", "Submission", "Pressure", "HostAccess"};
                    uint64_t prims = 0, nonEmpty = 0;
                    for (int r = 0; r < ps2xdiag::kReasons; ++r)
                    {
                        const uint64_t c = d.passCalls[r].load(), nn = d.passNonEmpty[r].load(), p = d.passPrims[r].load();
                        if (c > pPass[r] || nn > pNon[r])
                        {
                            char t[96];
                            std::snprintf(t, sizeof(t), "%s%s=%.1f(%.1f)", passes.empty() ? "" : "/", kReason[r], double(c - pPass[r]) / wall, double(nn - pNon[r]) / wall);
                            passes += t;
                        }
                        nonEmpty += nn - pNon[r];
                        prims += p - pPrim[r];
                        pPass[r] = c;
                        pNon[r] = nn;
                        pPrim[r] = p;
                    }
                    std::fprintf(stderr, "[perf]   parallel-gs sync: next_frame_context %.1f/s, wait %.1f%% of wall (avg %.2f ms, max %.1f ms; >0.2 ms %.1f/s, >1 ms %.1f/s, >4 ms %.1f/s, >16 ms %.1f/s) | timeline waits %.1f/s, %.1f%% of wall (max %.1f ms)\n",
                                 nfcRate, nfcPct, nfcAvg, nfcMax, over[0], over[1], over[2], over[3], tlRate, tlPct, tlMax);
                    std::fprintf(stderr, "[perf]   parallel-gs flushes: flush_submit %.1f/s (internal %.1f, explicit %.1f, pressure %.1f, host-access %.1f); render passes/s by reason total(non-empty): %s; %.1f primitives per non-empty pass\n",
                                 subRate, org[0], org[1], org[2], org[3], passes.empty() ? "-" : passes.c_str(), nonEmpty ? double(prims) / double(nonEmpty) : 0.0);
                    {
                        // 0017: which limit of check_flush_stats asked for the pressure flushes (prims / passes / image memory / scratch / copies / palette)
                        static uint64_t pPc = 0, pCause[6] = {};
                        static const char *const kCause[6] = {"prims", "passes", "image-mem", "scratch", "copies", "palette"};
                        const uint64_t pc = d.pressureCalls.load();
                        std::string causes;
                        for (int i = 0; i < 6; ++i)
                        {
                            const uint64_t c = d.pressureCause[i].load();
                            char t[64];
                            std::snprintf(t, sizeof(t), "%s%s %.1f", causes.empty() ? "" : ", ", kCause[i], double(c - pCause[i]) / wall);
                            causes += t;
                            pCause[i] = c;
                        }
                        std::fprintf(stderr, "[perf]   parallel-gs pressure: %.1f/s requested; limit exceeded (per s): %s\n", double(pc - pPc) / wall, causes.c_str());
                        pPc = pc;
                    }
                    pSub = sub; pNfc = nfc; pNfcNs = nfcNs; pTl = tl; pTlNs = tlNs;
                }
#endif
                const GsFlushStats &f = g_gsFlush;
                const uint64_t fp = f.primitives.load(), fr = f.renderPasses.load(), fpal = f.paletteUpdates.load(), fc = f.copies.load(), fb = f.copyBarriers.load(), fn = f.presents.load();
                const double presents = double(fn - pFn);
                if (fn > pFn)
                    std::fprintf(stderr, "[perf]   renderer work per present (consume_flush_stats, %.1f presents/s): %.0f primitives, %.2f render passes, %.2f palette updates, %.2f copies, %.2f copy barriers\n",
                                 presents / wall, double(fp - pFp) / presents, double(fr - pFr) / presents, double(fpal - pFpal) / presents, double(fc - pFc) / presents, double(fb - pFb) / presents);
                std::snprintf(gsCols, sizeof(gsCols), "%.2f,%.2f,%.3f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,\"%s\",%.1f,%.2f,%.2f,%.2f,%.2f", nfcRate, nfcPct, nfcAvg, nfcMax, over[0], over[1], over[2], over[3], tlRate, tlPct,
                              subRate, org[0], org[1], org[2], org[3], passes.c_str(), presents > 0 ? double(fp - pFp) / presents : 0.0, presents > 0 ? double(fr - pFr) / presents : 0.0,
                              presents > 0 ? double(fpal - pFpal) / presents : 0.0, presents > 0 ? double(fc - pFc) / presents : 0.0, presents > 0 ? double(fb - pFb) / presents : 0.0);
                pFp = fp; pFr = fr; pFpal = fpal; pFc = fc; pFb = fb; pFn = fn;
            }
            const std::string sampStr = sampleSummary(g_w.windows);
            if (!sampStr.empty())
                std::fprintf(stderr, "[perf]   game thread samples (%% of time by module): %s\n", sampStr.c_str());

            // CSV: one row per window
            const std::string path = csvPath();
            if (FILE *f = std::fopen(path.c_str(), "ab"))
            {
                std::fseek(f, 0, SEEK_END);
                if (std::ftell(f) == 0)
                {
                    std::fprintf(f, "label,window,t_end_s,vsync_tick,wall_s,scale,output,gpu,frame_w,frame_h,host_loops_per_s,game_speed_pct,p50_ms,p95_ms,p99_ms,max_ms,late_gt20,late_gt33,process_cpu_pct");
                    for (int i = 0; i < int(S::Count); ++i)
                        std::fprintf(f, ",%s_busy_pct,%s_ms_per_call,%s_calls_per_s", name(S(i)), name(S(i)), name(S(i)));
                    std::fprintf(f, ",threads,ee_samples,sif_trace_lines_per_s,sif_trace_suppressed_per_s,dmac_active,dmac_max,dmac_adds_per_s,dmac_removes_per_s,dmac_irq_per_s,dmac_entries_per_irq,dmac_invocations_per_irq,dmac_max_invocations,sif_packets_per_s,sif_invocations_per_packet,intc_active,intc_max,dmac_by_cause,irq_per_s_by_cause,nfc_per_s,nfc_wait_pct,nfc_avg_ms,nfc_max_ms,nfc_gt02ms_per_s,nfc_gt1ms_per_s,nfc_gt4ms_per_s,nfc_gt16ms_per_s,timeline_waits_per_s,timeline_wait_pct,flush_submit_per_s,submit_internal_per_s,submit_explicit_per_s,submit_pressure_per_s,submit_host_per_s,render_passes_by_reason,gs_primitives_per_present,gs_passes_per_present,gs_palette_per_present,gs_copies_per_present,gs_barriers_per_present\n");
                }
                const char *label = std::getenv("PS2X_PERF_LABEL");
                std::fprintf(f, "%s,%u,%.1f,%llu,%.2f,%s,%s,\"%s\",%u,%u,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%u,%u,%.1f", label ? label : "", g_w.windows, tEnd, static_cast<unsigned long long>(tick), wall, info("scale").c_str(), info("output").c_str(),
                             info("gpu").c_str(), fw, fh, hostFps, gameSpeed, pct(sorted, 0.50), pct(sorted, 0.95), pct(sorted, 0.99), sorted.empty() ? 0.0 : sorted.back(), late20, late33, procPct);
                for (int i = 0; i < int(S::Count); ++i)
                    std::fprintf(f, ",%.2f,%.4f,%.1f", busy[i], perCall[i], callsPerS[i]);
                std::fprintf(f, ",\"%s\",\"%s\",%.1f,%.1f,%s,%s\n", threadStr.c_str(), sampStr.c_str(), sifEps, sifSps, irqCols, gsCols);
                std::fclose(f);
            }
        }
    } // namespace

    void setInfo(const char *key, const char *value)
    {
        std::lock_guard<std::mutex> lock(g_infoMutex);
        g_info[key] = value ? value : "";
    }


    void startSampler(void *threadHandle)
    {
#if defined(_WIN32)
        const char *v = std::getenv("PS2X_PERF_SAMPLE");
        if (!g_on || !threadHandle || !v || !v[0] || v[0] == '0' || g_samp.thread.joinable())
            return;
        g_samp.stop.store(false);
        g_samp.deep = v[0] == '2';
        g_samp.thread = std::thread(samplerLoop, static_cast<HANDLE>(threadHandle));
        std::fprintf(stderr, "[perf] PS2X_PERF_SAMPLE=1: sampling the game thread every 2 ms (module breakdown per window%s)\n", g_samp.deep ? " + function-level stack samples -> <csv>.hot.csv" : "");
#else
        (void)threadHandle;
#endif
    }

    void stopSampler()
    {
#if defined(_WIN32)
        if (!g_samp.thread.joinable())
            return;
        g_samp.stop.store(true);
        g_samp.thread.join();
#endif
    }

    void frameDone(uint64_t guestVsyncTick, uint32_t frameWidth, uint32_t frameHeight)
    {
        if (!g_on)
            return;
        const uint64_t now = nowNs();
        Window &w = g_w;
        if (!w.started)
        {
            w.started = true;
            w.startNs = w.lastLoopNs = w.firstNs = now;
            w.startTick = guestVsyncTick;
            w.prevProcCpu100ns = processCpu100ns();
            threadCpu(1.0); // primes the per-thread baseline
            std::fprintf(stderr, "[perf] PS2X_PERF=%d active (window %.0f s, csv %s)\n", g_level, windowSeconds(), csvPath().c_str());
            return;
        }
        w.periods.push_back(double(now - w.lastLoopNs) / 1.0e6);
        w.lastLoopNs = now;
        ++w.loops;
        if (double(now - w.startNs) / 1.0e9 >= windowSeconds())
        {
            closeWindow(now, guestVsyncTick, frameWidth, frameHeight);
            w.startNs = now;
            w.startTick = guestVsyncTick;
            w.loops = 0;
            w.periods.clear();
        }
    }
} // namespace ps2perf
