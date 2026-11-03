#pragma once
// Low-overhead performance instrumentation of the frame pipeline (EE thread -> GS frontend -> parallel-gs -> presentation -> window).
// Off unless PS2X_PERF is set (1 = CPU timers only, 2 = also GPU timestamps of parallel-gs, which can slightly perturb the GPU timeline).
// Disabled cost: one load and one branch per instrumented point; nothing is allocated, written or logged.
// Report: every PS2X_PERF_WINDOW seconds (default 5) one block on stderr and one CSV row (PS2X_PERF_CSV, default %APPDATA%\ShiningWind\perf.csv,
// PS2X_PERF_LABEL tags the rows). See docs/perf-vega-instrumentation-2026-10-30.md for how to read it.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace ps2perf
{
    // Every stage is a wall-clock interval measured on the thread named in the comment. Intervals of DIFFERENT threads overlap in time: compare each one
    // with the wall time of the window (the report prints it as % of one thread), never add them up.
    enum class S : int
    {
        // --- EE thread (the game thread that produces GS work)
        EeGsFrontend,    // GS::processGIFPacket / writeRegister / uploadImageNative as a whole (includes everything below that runs inside it)
        EeFrontendLock,  //   waiting for the frontend state mutex (held by the presenter while it presents)
        EeTapLock,       //   waiting for the parallel-gs backend mutex (held by the presenter during gs.vsync + wait_idle)
        EeTapGif,        //   parallel-gs gif_transfer (CPU side of the renderer, may include its internal flushes/submits)
        EeTapRegister,   //   parallel-gs write_register
        EeTapImage,      //   parallel-gs image upload (OnImageUpload)
        EeAsyncWait,     // software backend only: EE blocked by the full command queue
        // --- host / presenter thread (the main thread: window loop)
        MainLoopBody,    // one pass of the window loop from the start of UploadFrame to the end of EndDrawing, minus EndDrawing's pacing wait
        MainUpload,      // UploadFrame: GS present + copy of the frame + texture upload
        MainPresentLock, //   backend Present: waiting for the backend mutex (the EE holds it while it feeds the GPU)
        MainFlushSync,   // GS::latchHostPresentationFrame: backend Flush + Sync before Present (software path: waits for the rasteriser queue)
        MainBackendPresent, // GSBackend::Present as a whole (flush + vsync + readback)
        MainMirror,      //   mirrorHostVram: GPU->host copy of the 4 MiB VRAM (flush + map_vram_read + memcpy)
        MainVsync,       //   gs.vsync call (CPU cost of recording the scanout)
        MainWaitIdle,    //   submit + wait_idle: the CPU waits for the GPU (all the rendering queued so far + the readback)
        MainHostCopy,    //   copy of the read-back image into the frame (18 MiB at x4)
        MainFrameCopy,   // copyLatchedHostPresentationFrame: second copy of the frame into the buffer of the window loop
        MainTexUpdate,   // UploadFrame: UpdateTexture (OpenGL upload of the frame) and, if the size differs, the clear + row copy before it
        MainCpuPresent,  //   1x path: CRTC conversion of the mirrored VRAM on the CPU
        MainDraw,        // raylib: BeginDrawing .. DrawTexturePro .. overlay (before EndDrawing)
        MainEndDrawing,  // EndDrawing (buffer swap; includes the SetTargetFPS(60) sleep: idle time on purpose)
        // --- other
        PushVram,        // host->GPU VRAM push (after host-side VRAM writes)
        // --- GPU timestamps of parallel-gs (PS2X_PERF=2), real GPU execution time (not waiting)
        GpuShading,
        GpuBinning,
        GpuSetup,
        GpuVsync,
        GpuTexUpload,
        GpuCopyVram,
        GpuReadback,
        GpuPresentCopy,  // blit + image->buffer copy recorded by the presenter
        Count
    };

    extern bool g_on;    // PS2X_PERF set
    extern int g_level;  // 1 or 2
    extern std::atomic<uint64_t> g_ns[int(S::Count)];
    extern std::atomic<uint64_t> g_cnt[int(S::Count)];
    extern std::atomic<uint64_t> g_bytes[int(S::Count)];

    [[nodiscard]] inline bool enabled() noexcept { return g_on; }

    // PS2X_SIF_TRACE=0 silences the always-on "[SIF:...]" diagnostic lines (guest-resume / guest-write / guest-dispatch / wire) that are written to the
    // unbuffered std::cerr from the emulation thread. Default (unset or anything else) keeps them. Only the printing is skipped; the counters and every
    // other statement around the trace stay identical. sifTrace() returns whether to print and counts the line either way (emitted / suppressed), so the
    // perf report can show lines/s for both settings.
    extern std::atomic<uint64_t> g_sifEmitted;
    extern std::atomic<uint64_t> g_sifSuppressed;
    [[nodiscard]] inline bool sifTraceEnabled() noexcept
    {
        static const bool on = []() {
            const char *v = std::getenv("PS2X_SIF_TRACE");
            return !(v && v[0] == '0' && !v[1]);
        }();
        return on;
    }
    [[nodiscard]] inline bool sifTrace() noexcept
    {
        if (sifTraceEnabled())
        {
            g_sifEmitted.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_sifSuppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] inline uint64_t nowNs() noexcept
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline void add(S s, uint64_t ns, uint64_t count = 1, uint64_t bytes = 0) noexcept
    {
        g_ns[int(s)].fetch_add(ns, std::memory_order_relaxed);
        g_cnt[int(s)].fetch_add(count, std::memory_order_relaxed);
        if (bytes)
            g_bytes[int(s)].fetch_add(bytes, std::memory_order_relaxed);
    }

    // RAII interval. Cheap when disabled.
    struct Scope
    {
        S stage;
        uint64_t t0;
        uint64_t bytes;
        explicit Scope(S s, uint64_t b = 0) noexcept : stage(s), t0(g_on ? nowNs() : 0u), bytes(b) {}
        ~Scope()
        {
            if (t0)
                add(stage, nowNs() - t0, 1, bytes);
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };

    // lock_guard that charges to `waitStage` only the time spent WAITING for the mutex (try_lock first, so the clock is read only on contention).
    template <class Mutex>
    struct TimedGuard
    {
        Mutex &m;
        TimedGuard(Mutex &mutex, S waitStage) : m(mutex)
        {
            if (g_on && !m.try_lock())
            {
                const uint64_t t = nowNs();
                m.lock();
                add(waitStage, nowNs() - t);
            }
            else if (!g_on)
                m.lock();
        }
        ~TimedGuard() { m.unlock(); }
        TimedGuard(const TimedGuard &) = delete;
        TimedGuard &operator=(const TimedGuard &) = delete;
    };

    // Context shown in the header of every report/CSV row (internal scale, scanout, GPU name, backend). Safe to call from any thread.
    void setInfo(const char *key, const char *value);

    // Called by the window loop once per iteration (after EndDrawing) with the guest VBlank counter and the size of the picture shown.
    // Closes a report window when PS2X_PERF_WINDOW seconds have passed.
    void frameDone(uint64_t guestVsyncTick, uint32_t frameWidth, uint32_t frameHeight);

    // PS2X_PERF_SAMPLE=1: samples the game thread (handle of the thread that runs the guest) every 2 ms and adds a per-window module breakdown + who-waits
    // callers to the report/CSV. No-op unless PS2X_PERF and PS2X_PERF_SAMPLE are set. stopSampler() must run before the thread handle is closed.
    void startSampler(void *threadHandle);
    void stopSampler();

    // Stage names for the report (also used by the CSV header).
    const char *name(S s);
} // namespace ps2perf
