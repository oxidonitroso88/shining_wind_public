#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// EE scheduler regression tests that need a running scheduler thread (interrupt handlers versus thread priorities).

namespace
{
    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
        }
    };

    template <typename Predicate>
    bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    }

    constexpr uint32_t kStarveMainPc = 0x00160600u;
    constexpr uint32_t kStarveHighPc = 0x00160610u;
    constexpr uint32_t kStarveHighResumePc = 0x00160620u;
    constexpr uint32_t kStarveSpinPc = 0x00160630u;
    constexpr uint32_t kStarveLowPc = 0x00160640u;
    constexpr uint32_t kStarveLowSpinPc = 0x00160650u;
    constexpr uint32_t kStarveHandlerPc = 0x00160660u;
    constexpr uint32_t kStarveHandlerTailPc = 0x00160670u;

    // Regression: an interrupt handler running on top of a low-priority thread must run to completion even when the
    // handler wakes a higher-priority thread part-way (the kernel applies the reschedule when the handler returns).
    // Before the fix the scheduler preempted the thread with the handler half done; a busy thread of medium priority
    // then starved it forever (real case: the game's vsync tick kept its re-entrancy lock set, "Now Loading" hung).
    int g_starveSemHigh = 0;
    int g_starveSemMain = 0;
    int g_starveSpinThread = 0;
    std::atomic<bool> g_starveHandlerDone{false};
    std::atomic<uint32_t> g_starveHighRuns{0u};

    void schedulerStarveMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        g_starveSemHigh = scheduler.createSemaphore(0, 1, 0u, 0u);
        g_starveSemMain = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(false, 14u, kStarveHandlerPc, true, 0u, 0u, 0u);

        EeThreadCreateParams high{};
        high.entry = kStarveHighPc;
        high.stack = 0x20000u;
        high.stackSize = 0x1000u;
        high.priority = 10;
        const int highId = scheduler.createThread(high);

        EeThreadCreateParams spin{};
        spin.entry = kStarveSpinPc;
        spin.stack = 0x21000u;
        spin.stackSize = 0x1000u;
        spin.priority = 24;
        g_starveSpinThread = scheduler.createThread(spin);

        EeThreadCreateParams low{};
        low.entry = kStarveLowPc;
        low.stack = 0x22000u;
        low.stackSize = 0x1000u;
        low.priority = 60;
        const int lowId = scheduler.createThread(low);

        scheduler.startThread(highId, 0u, *ctx, false);
        scheduler.startThread(lowId, 0u, *ctx, false);
        ctx->pc = kStarveMainPc + 0x8u; // never reached: the main thread blocks for good
        scheduler.waitSemaphore(g_starveSemMain);
    }

    void schedulerStarveHigh(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = kStarveHighResumePc;
        runtime->eeScheduler().waitSemaphore(g_starveSemHigh);
    }

    void schedulerStarveHighResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Woken by the handler: release the medium-priority busy thread, then wait again.
        g_starveHighRuns.fetch_add(1u, std::memory_order_relaxed);
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.startThread(g_starveSpinThread, 0u, *ctx, false);
        ctx->pc = kStarveHighResumePc;
        scheduler.waitSemaphore(g_starveSemHigh);
    }

    void schedulerStarveSpin(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = kStarveSpinPc; // busy loop that never blocks
    }

    void schedulerStarveLow(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->eeScheduler().dispatchIrq(false, 14u); // a cause the host never raises on its own
        ctx->pc = kStarveLowSpinPc;
    }

    void schedulerStarveLowSpin(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = kStarveLowSpinPc;
    }

    void schedulerStarveHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        // First half of the handler: wake the high-priority thread (interrupt-safe) and yield to the scheduler.
        runtime->eeScheduler().signalSemaphore(g_starveSemHigh, true);
        ctx->pc = kStarveHandlerTailPc;
    }

    void schedulerStarveHandlerTail(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_starveHandlerDone.store(true, std::memory_order_release);
        ctx->pc = 0u;
    }


    // Host-clock catch-up: a guest that accounts almost no cycles (1 per checkpoint) while the host clock passes the VBlank deadline.
    constexpr uint32_t kCatchupMainPc = 0x00160700u;
    std::atomic<bool> g_catchupDone{false};

    void schedulerCatchupMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(60))
            (void)runtime->eeScheduler().checkpointDue(1u);
        g_catchupDone.store(true, std::memory_order_release);
        ctx->pc = kCatchupMainPc; // keep spinning until the test stops the runtime
    }

    // Device-time batching: 100 cycles accounted one at a time (below the 256-cycle batch) must still show up exactly in a timer read.
    constexpr uint32_t kTimerMainPc = 0x00160800u;
    std::atomic<uint32_t> g_timerCountSeen{0xFFFFFFFFu};

    void schedulerTimerMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2Memory &memory = runtime->memory();
        memory.writeIORegister(0x10000010u, 0x80u); // timer 0: MODE = CUE, bus clock (147.456 MHz = EE clock / 2)
        memory.writeIORegister(0x10000000u, 0u);    // COUNT = 0
        for (int i = 0; i < 100; ++i)
            (void)runtime->eeScheduler().checkpointDue(1u);
        g_timerCountSeen.store(memory.readIORegister(0x10000000u) & 0xFFFFu, std::memory_order_release);
        ctx->pc = kTimerMainPc;
    }

    uint32_t runTimerScenario()
    {
        TestEnv env;
        env.runtime.memory().initialize();
        env.runtime.registerFunction(kTimerMainPc, schedulerTimerMain);
        g_timerCountSeen.store(0xFFFFFFFFu, std::memory_order_release);
        R5900Context mainContext{};
        mainContext.pc = kTimerMainPc;
        std::thread gameThread([&]()
        {
            try
            {
                env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                env.runtime.eeScheduler().setIdleSkip(false); // the loop below is a deliberate pure spin: count cycles exactly
                env.runtime.eeScheduler().run();
            }
            catch (...)
            {
            }
        });
        waitUntil([]() { return g_timerCountSeen.load(std::memory_order_acquire) != 0xFFFFFFFFu; }, std::chrono::milliseconds(2000));
        env.runtime.requestStop();
        gameThread.join();
        return g_timerCountSeen.load(std::memory_order_acquire);
    }

    uint64_t runCatchupScenario(bool catchup)
    {
        TestEnv env;
        env.runtime.memory().initialize();
        env.runtime.registerFunction(kCatchupMainPc, schedulerCatchupMain);
        g_catchupDone.store(false, std::memory_order_release);
        R5900Context mainContext{};
        mainContext.pc = kCatchupMainPc;
        std::atomic<uint64_t> credited{0u};
        std::thread gameThread([&]()
        {
            try
            {
                env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                env.runtime.eeScheduler().setHostCatchup(catchup);
                env.runtime.eeScheduler().setIdleSkip(false);
                env.runtime.eeScheduler().run();
            }
            catch (...)
            {
            }
        });
        waitUntil([]() { return g_catchupDone.load(std::memory_order_acquire); }, std::chrono::milliseconds(2000));
        credited.store(env.runtime.eeScheduler().hostCatchupCycles(), std::memory_order_release);
        env.runtime.requestStop();
        gameThread.join();
        return credited.load(std::memory_order_acquire);
    }

    // Idle-spin fast-forward: the only (lowest-priority) thread runs a polling loop. With the register file identical at every visit of the
    // loop checkpoint the scheduler advances guest time instead of running the iterations; a loop whose state changes each time must be left alone.
    constexpr uint32_t kIdleSpinPc = 0x00160900u;
    struct IdleSpinResult
    {
        uint64_t iterations = 0, skips = 0, skippedCycles = 0, guestCycles = 0;
    };
    std::atomic<bool> g_idleSpinDone{false};
    std::atomic<bool> g_idleSpinCounting{false};
    IdleSpinResult g_idleSpinResult;

    std::atomic<uint64_t> g_idleSpinIterations{0u};
    std::chrono::steady_clock::time_point g_idleSpinStart;

    void schedulerIdleSpinMain(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        if (g_idleSpinIterations.load(std::memory_order_relaxed) == 0u)
            g_idleSpinStart = std::chrono::steady_clock::now();
        // 14 cycles per iteration like the game's idle loop; like generated code, return to the scheduler whenever the checkpoint asks for it
        while (std::chrono::steady_clock::now() - g_idleSpinStart < std::chrono::milliseconds(300))
        {
            ctx->pc = kIdleSpinPc;
            if (g_idleSpinCounting.load(std::memory_order_relaxed))
                ctx->r[8] = _mm_add_epi32(ctx->r[8], _mm_set1_epi32(1)); // a counter changes every iteration: not a pure spin
            g_idleSpinIterations.fetch_add(1u, std::memory_order_relaxed);
            if (scheduler.checkpointDue(14u))
                return; // the scheduler resumes the function at ctx->pc
        }
        g_idleSpinResult.iterations = g_idleSpinIterations.load(std::memory_order_relaxed);
        g_idleSpinResult.skips = scheduler.m_idleSkips;
        g_idleSpinResult.skippedCycles = scheduler.m_idleSkippedCycles;
        g_idleSpinDone.store(true, std::memory_order_release);
        ctx->pc = kIdleSpinPc + 0x10u;
    }

    void schedulerIdleSpinPark(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = kIdleSpinPc + 0x10u;
    }

    IdleSpinResult runIdleSpinScenario(bool changingState)
    {
        TestEnv env;
        env.runtime.memory().initialize();
        env.runtime.registerFunction(kIdleSpinPc, schedulerIdleSpinMain);
        env.runtime.registerFunction(kIdleSpinPc + 0x10u, schedulerIdleSpinPark);
        g_idleSpinDone.store(false, std::memory_order_release);
        g_idleSpinCounting.store(changingState, std::memory_order_relaxed);
        g_idleSpinResult = {};
        g_idleSpinIterations.store(0u, std::memory_order_relaxed);
        R5900Context mainContext{};
        mainContext.pc = kIdleSpinPc;
        std::thread gameThread([&]()
        {
            try
            {
                env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                env.runtime.eeScheduler().setIdleSkip(true);
                env.runtime.eeScheduler().run();
            }
            catch (...)
            {
            }
        });
        waitUntil([]() { return g_idleSpinDone.load(std::memory_order_acquire); }, std::chrono::milliseconds(5000));
        env.runtime.requestStop();
        gameThread.join();
        return g_idleSpinResult;
    }

}

void register_ee_scheduler_irq_tests()
{
    MiniTest::Case("EeSchedulerIrq", [](TestCase &tc)
    {
        tc.Run("an interrupt handler preempted by a higher-priority wakeup still runs to completion", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kStarveMainPc, schedulerStarveMain);
            env.runtime.registerFunction(kStarveHighPc, schedulerStarveHigh);
            env.runtime.registerFunction(kStarveHighResumePc, schedulerStarveHighResume);
            env.runtime.registerFunction(kStarveSpinPc, schedulerStarveSpin);
            env.runtime.registerFunction(kStarveLowPc, schedulerStarveLow);
            env.runtime.registerFunction(kStarveLowSpinPc, schedulerStarveLowSpin);
            env.runtime.registerFunction(kStarveHandlerPc, schedulerStarveHandler);
            env.runtime.registerFunction(kStarveHandlerTailPc, schedulerStarveHandlerTail);

            g_starveHandlerDone.store(false, std::memory_order_release);
            g_starveHighRuns.store(0u, std::memory_order_release);
            R5900Context mainContext{};
            mainContext.pc = kStarveMainPc;
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
            });

            const bool completed = waitUntil([]()
            {
                return g_starveHandlerDone.load(std::memory_order_acquire);
            }, std::chrono::milliseconds(300));
            env.runtime.requestStop();
            gameThread.join();

            t.IsFalse(schedulerThrew.load(std::memory_order_acquire), "the starvation scenario should not throw");
            t.IsTrue(completed,
                     "a handler must finish before the thread it interrupted can be preempted (a busy medium-priority thread starved it)");
            t.IsTrue(g_starveHighRuns.load(std::memory_order_acquire) >= 1u,
                     "the high-priority thread woken by the handler must still run after the handler returns");
        });

        tc.Run("a timer read sees cycles that are still in the device-time batch", [](TestCase &t)
        {
            t.Equals(runTimerScenario(), 50u, "100 EE cycles on a 147.456 MHz timer are exactly 50 ticks, batched or not");
        });

        tc.Run("the host-clock catch-up credits guest cycles when the host is ahead and can be switched off", [](TestCase &t)
        {
            const uint64_t withCatchup = runCatchupScenario(true);
            t.IsTrue(withCatchup > 100000u, "by default a host clock past the VBlank deadline credits the missing EE cycles");
            const uint64_t without = runCatchupScenario(false);
            t.Equals(without, uint64_t{0}, "with the catch-up disabled the guest clock only follows executed work");
        });

        tc.Run("a polling loop with an identical register file is fast-forwarded and guest time still advances", [](TestCase &t)
        {
            const IdleSpinResult r = runIdleSpinScenario(false);
            t.IsTrue(r.skips > 0u && r.skippedCycles > 0u, "identical state at the loop checkpoint triggers the skip");
            t.IsTrue(r.skippedCycles > r.iterations * 14u * 20u, "far more guest cycles were skipped than the executed iterations account for (the skip is the point)");
        });

        tc.Run("a loop whose registers change on every iteration is never fast-forwarded", [](TestCase &t)
        {
            const IdleSpinResult r = runIdleSpinScenario(true);
            t.Equals(r.skips, uint64_t{0}, "a changing counter means real work: no skip");
            t.IsTrue(r.iterations > 1000u, "the loop did run");
        });

    });
}
