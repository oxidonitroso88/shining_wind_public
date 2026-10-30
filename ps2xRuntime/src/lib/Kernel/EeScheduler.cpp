#include <sstream>
#include <tuple>
#include <map>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>
#include <iostream>
#include <cstdlib>
#include <string>
#include <string_view>
#include <set>
#include "runtime/ee_scheduler.h"
#include "ps2x/iop/audio_output.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
    void timeline(const char *what, uint64_t eeCycle, int64_t a, int64_t b);
    void schedStat(const char *what, int actor, int object);
}

namespace
{
    // Every EE thread transfer unwinds the recompiled call stack with this exception; the counter (printed with PS2X_SCHED_STATS) tells how often.
    std::atomic<uint64_t> g_eeTransferThrows{0};

    [[noreturn]] void throwEeTransfer()
    {
        g_eeTransferThrows.fetch_add(1, std::memory_order_relaxed);
        throw EeDispatcherTransfer{};
    }
}

namespace
{
    thread_local uint32_t sifResumeTraceRemaining = 0;
    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

namespace
{
    void traceThread(const char *what, int actor, int target, int result, int status, int suspendCount, uint32_t wakeups);
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
    m_runtime.memory().setDeviceTimeSync({}); // the hook captures this scheduler
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_threadCache = nullptr;
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    // PS2X_HOST_CATCHUP=always|idle|never selects the mode (PS2X_NO_HOST_CATCHUP=1 is "never"); see setHostCatchupMode().
    // Default since the M3 stable checkpoint (2026-10-15): Never. Guest time follows executed work only; with the idle-spin skip the game still runs at
    // 1.00x real time and the audio is bit-exact (docs/audio-m3-wasapi-2026-10-15.md). "always" (the former default) credits the host clock to the guest
    // and corrupts the ADX/AHX input timing of movies.
    m_hostCatchupMode = HostCatchupMode::Never;
    if (const char *mode = std::getenv("PS2X_HOST_CATCHUP"))
    {
        if (std::string_view(mode) == "idle")
            m_hostCatchupMode = HostCatchupMode::IdleOnly;
        else if (std::string_view(mode) == "never")
            m_hostCatchupMode = HostCatchupMode::Never;
    }
    if (std::getenv("PS2X_NO_HOST_CATCHUP") != nullptr)
        m_hostCatchupMode = HostCatchupMode::Never;
    m_hostCatchupCycles = 0u;
    m_vblankCount = 0u;
    if (const char *ff = std::getenv("PS2X_FAST_FORWARD"); ff != nullptr && std::atoll(ff) > 0)
    {
        m_fastForwardUntil = static_cast<uint64_t>(std::atoll(ff));
        m_fastForward.store(true, std::memory_order_relaxed);
        ps2x::iop::setAudioFastForward(true);
        m_fastForwardStart = std::chrono::steady_clock::now();
    }
    {
        const char *idleSkip = std::getenv("PS2X_IDLE_SKIP");
        m_idleSkipEnabled = !(idleSkip != nullptr && idleSkip[0] == '0');
        m_idleCandidate = false;
        for (IdleSlot &slot : m_idleSlots)
            slot.valid = false;
    }
    m_hostCatchupBusyCycles = 0u;
    m_pendingDeviceCycles = 0u;
    m_runtime.memory().setDeviceTimeSync([this]() { flushDeviceTime(); });
    m_timerCyclesTotal = 0u;
    m_timerEpochBase = 0u;
    m_timerHostEpoch = {};
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        if (pendingEventWork())
            processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                if (invocation.onBegin && !invocation.onBegin()) continue;
                GuestThread *owner = &acquireInvocationThread();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            {
                static std::atomic<uint32_t> reported{0};
                if (running->id > 0 && reported.fetch_add(1) < 20u)
                {
                    std::cerr << "[sched] thread " << running->id << " reached pc=0 with no pending invocation and became dormant"
                              << " (ra=0x" << std::hex << getRegU32(&context, 31) << " sp=0x" << getRegU32(&context, 29)
                              << " a0=0x" << getRegU32(&context, 4) << " v0=0x" << getRegU32(&context, 2) << std::dec << ")" << std::endl;
                }
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (invocation.onBegin && !invocation.onBegin()) continue;
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
if (context.pc == 0x116400u)
{
    std::cerr << "[TEST] SCHEDULER reached PC=0x116400" << std::endl;
}
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);
        if (sifResumeTraceRemaining && running->invocations.empty())
        {
            --sifResumeTraceRemaining;
            std::cerr << "[SIF:guest-resume] pc=0x" << std::hex << context.pc
                      << " ra=0x" << getRegU32(&context,31) << std::dec << '\n';
        }

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        struct SifTraceScope
        {
            ~SifTraceScope() { ps2SifGuestTraceActive = false; }
        } traceScope;
        ps2SifGuestTraceActive = !running->invocations.empty() &&
            running->invocations.back().kind == GuestInvocationKind::SifCommand;
        if (ps2SifGuestTraceActive && ps2SifGuestTraceDispatches++ < 96)
            std::cerr << "[SIF:guest-dispatch] pc=0x" << std::hex << context.pc
                      << " a0=0x" << getRegU32(&context,4) << " a1=0x" << getRegU32(&context,5)
                      << std::dec << '\n';
        try
        {
            m_insideInterrupt = !running->invocations.empty() &&
                (running->invocations.back().kind == GuestInvocationKind::Interrupt ||
                 running->invocations.back().kind == GuestInvocationKind::SifCommand);
            m_guestExecuting.store(true, std::memory_order_release);
            {
                // PS2X_THREAD_PROFILE=1: host time spent executing guest code, per EE thread, printed every ~5 s.
                static const bool profile = std::getenv("PS2X_THREAD_PROFILE") != nullptr;
                if (profile)
                {
                    static uint64_t nanos[64] = {};
                    static uint64_t calls[64] = {};
                    static auto lastPrint = std::chrono::steady_clock::now();
                    const auto t0 = std::chrono::steady_clock::now();
                    struct Done
                    {
                        std::chrono::steady_clock::time_point t0;
                        int id;
                        ~Done()
                        {
                            const auto t1 = std::chrono::steady_clock::now();
                            const int i = (id >= 0 && id < 64) ? id : 0;
                            nanos[i] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
                            ++calls[i];
                        }
                    } done{t0, running->id};
                    function(m_rdram, &context, &m_runtime);
                    (void)done;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastPrint > std::chrono::seconds(5))
                    {
                        lastPrint = now;
                        std::cerr << "[threadprof]";
                        for (int i = 0; i < 64; ++i)
                            if (calls[i])
                                std::cerr << " t" << i << "=" << (nanos[i] / 1000000u) << "ms/" << calls[i];
                        std::cerr << std::endl;
                    }
                }
                else
                {
                    function(m_rdram, &context, &m_runtime);
                }
            }
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        if (pendingEventWork())
            processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0 && !runningInInterruptContext())
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

// processPendingEvents() locks the event mutex three times, reads the host clock twice and builds a std::deque on every call, and the
// run loop used to call it after EVERY guest invocation (~240 000 times per second in the game's idle thread), which made the scheduler
// and not the guest code the dominant host cost of the EE thread in the field phase (docs/ee-thread-performance-2026-10-14.md). It only
// acts on the conditions below, so the loop calls it only when one of them holds.
bool EeScheduler::pendingEventWork() const noexcept
{
    if (m_checkpointPending.load(std::memory_order_acquire) || m_rescheduleRequested || m_pendingEeTimerInterrupts != 0u)
        return true;
    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    return nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

// See the comment in ee_scheduler.h. Called at every checkpoint of the idle thread.
void EeScheduler::tryIdleSkip() noexcept
{
    GuestThread *const running = currentThread();
    if (running == nullptr)
        return;
    const R5900Context &ctx = running->activeContext();
    IdleSlot &slot = m_idleSlots[(ctx.pc >> 2) & 7u];
    if (slot.valid && slot.pc == ctx.pc && slot.hi == ctx.hi && slot.lo == ctx.lo && slot.hi1 == ctx.hi1 && slot.lo1 == ctx.lo1 &&
        std::memcmp(slot.r, ctx.r, sizeof(slot.r)) == 0)
    {
        if (++slot.same < 2u)
            return;
        slot.same = 0u;
        const uint64_t next = m_nextDeadlineCycle.load(std::memory_order_acquire);
        if (next == 0u || next <= m_eeCycle)
            return;
        const uint64_t gap = std::min<uint64_t>(next - m_eeCycle, kIdleSkipCycles);
        ++m_idleSkips;
        m_idleSkippedCycles += gap;
        accountCycles(static_cast<uint32_t>(gap));
        return;
    }
    slot.valid = true;
    slot.pc = ctx.pc;
    slot.same = 0u;
    slot.hi = ctx.hi; slot.lo = ctx.lo; slot.hi1 = ctx.hi1; slot.lo1 = ctx.lo1;
    std::memcpy(slot.r, ctx.r, sizeof(slot.r));
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    // PS2X_CKPT_SCALE=<f> (experiment): scale the cycles charged per checkpoint (back-edge = 32, dispatch = 8, whatever the body length)
    // to see how much of the guest-time work budget the charging model eats (audio starvation of the opening, docs/audio-battle-static-investigation).
    static const double ckptScale = []() { const char *v = std::getenv("PS2X_CKPT_SCALE"); return v ? std::atof(v) : 1.0; }();
    if (ckptScale != 1.0)
    {
        static thread_local double carry = 0.0;
        carry += static_cast<double>(cycles) * ckptScale;
        const uint32_t whole = static_cast<uint32_t>(carry);
        carry -= static_cast<double>(whole);
        cycles = whole;
    }
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    if (m_idleSkipEnabled)
    {
        // Re-evaluated every 1024 checkpoints: is the running thread the idle loop (nothing with a lower priority)?
        if ((++m_idleProbe & 0x3FFu) == 1u)
            m_idleCandidate = currentThread() != nullptr && !runningInInterruptContext() && runningThreadIsIdle();
        if (m_idleCandidate)
            tryIdleSkip();
    }

    if ((++m_timerPoll & 0x3FFu) == 0u)
    {
        {
            // EXPERIMENT (diagnostic only): PS2X_EXPERIMENT_ARM_TIMER1=<seconds> programs EE timer 1 once, <seconds> after the
            // first call, with what the game's own start routine (0x1cea40) writes: MODE=0x1C3 COMP=0 COUNT=0.
            static const char *arm = std::getenv("PS2X_EXPERIMENT_ARM_TIMER1");
            static const auto armStart = std::chrono::steady_clock::now();
            static bool armed = false;
            if (arm && !armed && std::chrono::duration<double>(std::chrono::steady_clock::now() - armStart).count() >= std::atof(arm))
            {
                armed = true;
                m_runtime.memory().writeIORegister(0x10000810u, 0x1C3u);
                m_runtime.memory().writeIORegister(0x10000820u, 0u);
                m_runtime.memory().writeIORegister(0x10000800u, 0u);
                std::cerr << "[experiment] timer1 armed" << std::endl;
            }
        }
        static const bool timerDiag = std::getenv("PS2X_SCHED_STATS") != nullptr;
        if (timerDiag)
        {
            static auto lastTimerDiag = std::chrono::steady_clock::now();
            const auto nowDiag = std::chrono::steady_clock::now();
            if (nowDiag - lastTimerDiag > std::chrono::seconds(10))
            {
                lastTimerDiag = nowDiag;
                std::cerr << "[idleskip] skips=" << m_idleSkips << " skippedCycles=" << m_idleSkippedCycles << std::endl;
                std::cerr << "[timers]" << m_runtime.memory().describeEeTimers() << " intcMask=0x" << std::hex << m_enabledIntcMask << std::dec << std::endl;
            }
        }
        // Keep the EE timers at real-time rate. Real EE time is host time; the accounted cycles only lag
        // behind it. Deliver the missing time to the timers (not to the cycle counter, the IOP or the
        // deadlines, which have their own catch-up) so timer-driven middleware sees a sane clock.
        flushDeviceTime(); // the host-time catch-up below compares against the cycles already delivered
        constexpr uint64_t kEeHz = 294912000ull;
        const auto now = std::chrono::steady_clock::now();
        if (m_timerHostEpoch.time_since_epoch().count() == 0)
        {
            m_timerHostEpoch = now;
            m_timerEpochBase = m_timerCyclesTotal;
        }
        const uint64_t hostNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_timerHostEpoch).count());
        const uint64_t hostTotal = m_timerEpochBase + hostNs * (kEeHz / 1000000ull) / 1000ull;
        if (hostTotal > m_timerCyclesTotal + 100000ull)
        {
            const uint64_t missing = hostTotal - m_timerCyclesTotal;
            m_timerCyclesTotal = hostTotal;
            static const double catchUpScale = []() { const char *v = std::getenv("PS2X_TIMER_SCALE"); return v ? std::atof(v) : 1.0; }();
            m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(static_cast<uint64_t>(static_cast<double>(missing) * catchUpScale));
            if (m_pendingEeTimerInterrupts != 0u)
            {
                m_checkpointPending.store(true, std::memory_order_release);
            }
        }
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (m_hostCatchupMode != HostCatchupMode::Never && nextEventCycle != 0u && m_eeCycle < nextEventCycle && (++m_hostDeadlinePoll & 0x3FFu) == 0u)
    {
        // The guest is behind the host clock: the event's real-time deadline has passed although the
        // accounted cycles have not reached it. Let the EE clock catch up so the event is delivered.
        const int64_t hostTicks = m_nextDeadlineHostTicks.load(std::memory_order_acquire);
        if (hostTicks != 0 && std::chrono::steady_clock::now().time_since_epoch().count() >= hostTicks)
        {
            const uint64_t gap = nextEventCycle - m_eeCycle;
            const bool idle = runningThreadIsIdle();
            if (m_hostCatchupMode == HostCatchupMode::IdleOnly && !idle)
            {
                // a work thread is running: guest time does not jump under it; the credit is taken once the idle loop runs
                timeline("catchup-deferred", m_eeCycle, static_cast<int64_t>(gap), m_currentThreadId);
            }
            else
            {
                timeline("hostcatchup", m_eeCycle, static_cast<int64_t>(gap), idle ? 1 : m_currentThreadId);
                m_hostCatchupCycles += gap;
                if (!idle)
                    m_hostCatchupBusyCycles += gap;
                accountCycles(static_cast<uint32_t>(std::min<uint64_t>(gap, 0x7FFFFFFFu)));
            }
        }
    }
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority) && !runningInInterruptContext())
    {
        // Inside an interrupt handler the slice just restarts: the thread switch waits until the handler has returned.
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

// The running thread is "idle" when no live thread has a lower priority (larger number): the game's lowest-priority loop, which only
// polls while the work threads sleep. With no running thread (interrupt context on an idle CPU) the CPU is idle too.
bool EeScheduler::runningThreadIsIdle() const
{
    const auto current = m_threads.find(m_currentThreadId);
    if (current == m_threads.end())
        return true;
    for (const auto &[id, other] : m_threads)
        if (other.status != EeThreadStatus::Dormant && other.currentPriority > current->second.currentPriority)
            return false;
    return true;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_timerCyclesTotal += elapsed;
    m_pendingDeviceCycles += elapsed;
    if (m_pendingDeviceCycles >= kDeviceBatchCycles)
        flushDeviceTime();
}

void EeScheduler::flushDeviceTime() noexcept
{
    const uint64_t elapsed = m_pendingDeviceCycles;
    if (elapsed == 0u)
        return;
    m_pendingDeviceCycles = 0u;
    // PS2X_TIMER_SCALE=<f> (experiment): slow the EE timers relative to the accounted cycles.
    static const double timerScale = []() { const char *v = std::getenv("PS2X_TIMER_SCALE"); return v ? std::atof(v) : 1.0; }();
    if (timerScale != 1.0)
    {
        static double carry = 0.0;
        carry += static_cast<double>(elapsed) * timerScale;
        const uint64_t whole = static_cast<uint64_t>(carry);
        carry -= static_cast<double>(whole);
        if (whole != 0u)
            m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(whole);
    }
    else
    {
        m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    }
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    // Priority 0 is accepted: the main thread itself runs at 0 and titles (observed: Shining Wind's server threads)
    // create helper threads with the priority ReferThreadStatus reports for the creating thread.
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    m_threadCache = nullptr;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    traceThread(deleteThreadRecord ? "exit+delete" : "exit", id, id, KE_OK, static_cast<int>(exiting->status), exiting->suspendCount, exiting->wakeupCount);
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
        m_threadCache = nullptr;
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throwEeTransfer();
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    traceThread("terminate", m_currentThreadId, id, KE_OK, static_cast<int>(target->status), target->suspendCount, target->wakeupCount);
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

namespace
{
    // PS2X_THREAD_TRACE=<id[,id...]|all> logs Suspend/Resume/Wakeup/Sleep events that touch those threads, with the
    // acting thread and the resulting state. Diagnostic only; capped.
    bool threadTraceWants(int id)
    {
        static const std::string spec = []() { const char *v = std::getenv("PS2X_THREAD_TRACE"); return v ? std::string(v) : std::string(); }();
        if (spec.empty())
            return false;
        if (spec == "all")
            return true;
        size_t at = 0;
        while (at < spec.size())
        {
            size_t end = spec.find(',', at);
            if (end == std::string::npos)
                end = spec.size();
            if (std::atoi(spec.substr(at, end - at).c_str()) == id)
                return true;
            at = end + 1;
        }
        return false;
    }

    void traceThread(const char *what, int actor, int target, int result, int status, int suspendCount, uint32_t wakeups)
    {
        if (!threadTraceWants(target))
            return;
        // PS2X_THREAD_TRACE_AFTER=<seconds>: ignore events earlier than that host time (diagnostic only).
        static const double after = []() { const char *v = std::getenv("PS2X_THREAD_TRACE_AFTER"); return v ? std::atof(v) : 0.0; }();
        static const auto traceStart = std::chrono::steady_clock::now();
        if (after > 0.0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - traceStart).count() < after)
            return;
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1) >= 30000u)
            return;
        std::cerr << "[thr] " << what << " actor=" << actor << " target=" << target << " ret=" << result
                  << " status=" << status << " susp=" << suspendCount << " wake=" << wakeups << std::endl;
    }

    // PS2X_TIMELINE=1 (diagnostics): one line per scheduler event with the host clock (ms, same base as the [adma]/[cd] lines), the
    // accounted EE cycle and two event-specific values. Used to relate EE threads, VBlank, idle jumps and host catch-ups to the audio.
    void timeline(const char *what, uint64_t eeCycle, int64_t a, int64_t b)
    {
        static const bool on = std::getenv("PS2X_TIMELINE") != nullptr;
        if (!on)
            return;
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1) >= 3000000u)
            return;
        const long long wall = static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                          std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000000ll);
        std::fprintf(stderr, "[tl] w=%lld ee=%llu %s %lld %lld\n", wall, static_cast<unsigned long long>(eeCycle), what,
                     static_cast<long long>(a), static_cast<long long>(b));
    }

    // PS2X_SCHED_STATS=1: counts of scheduler primitives per (acting thread, primitive, object), printed every ~10 s.
    void schedStat(const char *what, int actor, int object)
    {
        static const bool on = std::getenv("PS2X_SCHED_STATS") != nullptr;
        if (!on)
            return;
        static std::map<std::tuple<std::string, int, int>, uint64_t> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[{what, actor, object}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(10))
        {
            last = now;
            std::ostringstream line;
            line << "[schedstat]";
            line << " transfer-throws=" << g_eeTransferThrows.exchange(0, std::memory_order_relaxed);
            for (const auto &item : counts)
                if (item.second > 100u)
                    line << " " << std::get<0>(item.first) << "(t" << std::get<1>(item.first) << ",#" << std::get<2>(item.first) << ")=" << item.second;
            std::cerr << line.str() << std::endl;
            counts.clear();
        }
    }
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    schedStat("suspend", m_currentThreadId, id);
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    traceThread("suspend", m_currentThreadId, id, KE_OK, static_cast<int>(target->status), target->suspendCount, target->wakeupCount);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    schedStat("resume", m_currentThreadId, id);
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        traceThread("resume(still suspended)", m_currentThreadId, id, KE_OK, static_cast<int>(target->status), target->suspendCount, target->wakeupCount);
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    traceThread("resume", m_currentThreadId, id, KE_OK, static_cast<int>(target->status), target->suspendCount, target->wakeupCount);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    schedStat("sleep", m_currentThreadId, 0);
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        traceThread("sleep(consumed queued wakeup)", self->id, self->id, KE_OK, static_cast<int>(self->status), self->suspendCount, self->wakeupCount);
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    traceThread("sleep(blocks)", self->id, self->id, KE_OK, static_cast<int>(self->status), self->suspendCount, self->wakeupCount);
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    schedStat("wakeup", m_currentThreadId, id);
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    const bool wasSleeping = (target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
                             target->wait.reason == EeWaitReason::Sleep;
    if (wasSleeping)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    traceThread(wasSleeping ? "wakeup(sleeping)" : "wakeup(queued)", m_currentThreadId, id, KE_OK, static_cast<int>(target->status), target->suspendCount, target->wakeupCount);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    schedStat("chprio", m_currentThreadId, priority);
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    schedStat("rotate", m_currentThreadId, priority);
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throwEeTransfer();
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    schedStat("signal", m_currentThreadId, id);
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    schedStat("poll", m_currentThreadId, id);
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    schedStat("wait", m_currentThreadId, id);
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    schedStat("setef", m_currentThreadId, id);
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    schedStat("pollef", m_currentThreadId, id);
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    schedStat("waitef", m_currentThreadId, id);
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throwEeTransfer();
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throwEeTransfer();
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    std::cerr << "[irq] add " << (dmac ? "DMAC" : "INTC") << " handler id=" << id << " cause=" << cause << " handler=0x" << std::hex << handler
              << std::dec << " registered=" << (m_runtime.hasFunction(handler) ? 1 : 0) << std::endl;
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

bool EeScheduler::queueSifPacket(std::function<bool(bool)> begin, std::function<void()> complete)
{
    assertExecutor();
    if (!(m_enabledDmacMask & (1u << 5))) return false;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : m_dmacHandlers)
        if (handler.enabled && handler.cause == 5 && handler.handler && m_runtime.hasFunction(handler.handler))
            matching.push_back(handler);
    if (matching.empty()) return false;
    std::sort(matching.begin(), matching.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
    auto started = std::make_shared<bool>(false);
    for (size_t i = 0; i < matching.size(); ++i)
    {
        const auto &handler = matching[i];
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::SifCommand;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, 5);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        invocation.onBegin = [begin, started, i]() {
            const bool valid = begin(i == 0);
            if (i == 0) *started = valid;
            return *started && valid;
        };
        if (i + 1 == matching.size())
            invocation.onComplete = [complete](const auto &, auto &) {
                sifResumeTraceRemaining = 64;
                complete();
            };
        queueInvocation(std::move(invocation));
    }
    return true;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    schedStat(dmac ? "dmacirq" : "intcirq", static_cast<int>(cause), 0);
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    schedStat(dmac ? "dmacirq-enabled" : "intcirq-enabled", static_cast<int>(cause), 0);
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
        else if (handler.enabled && handler.cause == cause && handler.handler != 0u)
        {
            static std::set<uint32_t> reported;
            if (reported.insert(handler.handler).second)
                std::cerr << "[irq] handler 0x" << std::hex << handler.handler << std::dec << " is not a registered function: interrupt skipped" << std::endl;
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    // The run loop asks for the running thread several times per guest invocation (~1 million per second in the idle thread).
    if (m_threadCache != nullptr && m_threadCacheId == id)
        return m_threadCache;
    auto it = m_threads.find(id);
    if (it == m_threads.end())
        return nullptr;
    m_threadCache = &it->second;
    m_threadCacheId = id;
    return m_threadCache;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    m_snapshotWanted.store(true, std::memory_order_relaxed); // ask the executor thread to rebuild at its next scheduler primitive
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot(bool force)
{
    if (!force && !m_snapshotWanted.exchange(false, std::memory_order_relaxed) && (++m_snapshotLazy & 0x3Fu) != 0u)
        return;
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    timeline("run", m_eeCycle, item.id, item.currentPriority);
    renewTimeSlice();
}

void EeScheduler::makeDormant(GuestThread &item)
{
    traceThread("makeDormant", m_currentThreadId, item.id, 0, static_cast<int>(item.status), item.suspendCount, item.wakeupCount);
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    // PS2X_BLOCK_LOG=1 (diagnostic): why each guest thread blocks (thread id, reason, semaphore / event flag id).
    if (static const bool blockLog = std::getenv("PS2X_BLOCK_LOG") != nullptr; blockLog)
    {
        long long arg = -1;
        if (const auto *s = std::get_if<EeSemaphoreWait>(&wait.payload)) arg = s->id;
        else if (const auto *e = std::get_if<EeEventFlagWait>(&wait.payload)) arg = e->id;
        std::fprintf(stderr, "[blk] ee=%llu thread=%d reason=%d id=%lld pc=%x ra=%x a0=%x\n", static_cast<unsigned long long>(m_eeCycle), m_currentThreadId, static_cast<int>(wait.reason), arg, self->activeContext().pc, getRegU32(&self->activeContext(), 31), getRegU32(&self->activeContext(), 4));
    }
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throwEeTransfer();
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    if (static const bool blockLog = std::getenv("PS2X_BLOCK_LOG") != nullptr; blockLog)
        std::fprintf(stderr, "[wake] ee=%llu thread=%d by=%d result=%d\n", static_cast<unsigned long long>(m_eeCycle), static_cast<int>(item.id), m_currentThreadId, result);
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::runningInInterruptContext() const
{
    const GuestThread *running = currentThread();
    if (!running)
    {
        return false;
    }
    for (const GuestInvocation &invocation : running->invocations)
    {
        switch (invocation.kind)
        {
        case GuestInvocationKind::Interrupt:
        case GuestInvocationKind::Alarm:
        case GuestInvocationKind::GsCallback:
        case GuestInvocationKind::SifCommand:
            return true;
        default:
            break;
        }
    }
    return false;
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (runningInInterruptContext())
    {
        // Keep the request pending: it is applied as soon as the handler has returned.
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (!m_fastForward.load(std::memory_order_relaxed) && now < pacingDeadline)
            {
                timeline("pacewait", m_eeCycle, std::chrono::duration_cast<std::chrono::microseconds>(pacingDeadline - now).count(), 0);
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            const bool fastForward = m_fastForward.load(std::memory_order_relaxed);
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow, fastForward](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       (fastForward || item.hostDeadline <= pacedNow); });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    timeline("event", m_eeCycle, static_cast<int64_t>(event.type), static_cast<int64_t>(event.id));
    // PS2X_STATE_HASH=<N> (diagnostic): every N-th VBlank start print 64-bit FNV-1a hashes of the EE RAM and the IOP RAM with the EE cycle, to compare
    // two runs (or a run and a restored one) and find the first point where they differ.
    if (static const uint32_t hashEvery = []() { const char *v = std::getenv("PS2X_STATE_HASH"); return v ? static_cast<uint32_t>(std::atoi(v)) : 0u; }();
        hashEvery != 0u && event.type == EeEventType::VBlankStart)
    {
        static uint32_t counter = 0;
        if ((counter++ % hashEvery) == 0u)
        {
            const auto fnv = [](const uint8_t *data, size_t size) {
                uint64_t h = 1469598103934665603ull;
                const uint64_t *w = reinterpret_cast<const uint64_t *>(data);
                for (size_t i = 0; i < size / 8u; ++i) { h ^= w[i]; h *= 1099511628211ull; }
                return h;
            };
            if (std::getenv("PS2X_STATE_HASH_PAGES") != nullptr)
            {
                std::fprintf(stderr, "[statepages] vblank=%u", counter - 1u);
                for (size_t page = 0; page < PS2_RAM_SIZE / 65536u; ++page)
                    std::fprintf(stderr, " %04x", static_cast<unsigned>(fnv(m_runtime.memory().getRDRAM() + page * 65536u, 65536u) & 0xFFFFu));
                std::fprintf(stderr, "%c", 10);
            }
            if (const char *dump = std::getenv("PS2X_STATE_DUMP"); dump != nullptr && (counter - 1u) == static_cast<uint32_t>(std::atoi(std::getenv("PS2X_STATE_DUMP_AT") ? std::getenv("PS2X_STATE_DUMP_AT") : "0")))
            {
                if (std::FILE *f = std::fopen(dump, "wb")) { std::fwrite(m_runtime.memory().getRDRAM(), 1, PS2_RAM_SIZE, f); std::fclose(f); }
            }
            std::vector<uint8_t> iop(2u << 20);
            const bool iopOk = m_runtime.readIopMemory(0u, iop.data(), iop.size());
            std::fprintf(stderr, "[statehash] vblank=%u ee=%llu eeram=%016llx iopram=%016llx\n", counter - 1u, static_cast<unsigned long long>(m_eeCycle),
                         static_cast<unsigned long long>(fnv(m_runtime.memory().getRDRAM(), PS2_RAM_SIZE)), static_cast<unsigned long long>(iopOk ? fnv(iop.data(), iop.size()) : 0ull));
        }
    }
    if (event.type == EeEventType::VBlankStart)
    {
        extern std::atomic<unsigned long long> g_ps2xGsVblank;
        g_ps2xGsVblank.store(m_vblankCount + 1u, std::memory_order_relaxed);
    }
    // PS2X_SHOT=<prefix> with PS2X_SHOT_VBLANKS=<n>[,<n>...] and/or PS2X_SHOT_EVERY=<k>: save the displayed frame as <prefix>_<vblank>.png at those VBlanks
    // (scenario captures tied to guest time, valid in fast-forward too).
    if (event.type == EeEventType::VBlankStart)
    {
        static const char *shotPrefix = std::getenv("PS2X_SHOT");
        if (shotPrefix != nullptr && shotPrefix[0] != 0)
        {
            static const std::set<uint64_t> shotAt = []() {
                std::set<uint64_t> s;
                if (const char *v = std::getenv("PS2X_SHOT_VBLANKS"))
                {
                    const char *p = v;
                    while (*p != 0)
                    {
                        char *end = nullptr;
                        const unsigned long long n = std::strtoull(p, &end, 10);
                        if (end == p) break;
                        s.insert(n);
                        p = (*end == ',') ? end + 1 : end;
                    }
                }
                return s;
            }();
            static const uint64_t shotEvery = []() { const char *v = std::getenv("PS2X_SHOT_EVERY"); return v ? static_cast<uint64_t>(std::strtoull(v, nullptr, 10)) : 0ull; }();
            const uint64_t nextVblank = m_vblankCount + 1u; // m_vblankCount is advanced below
            if (shotAt.count(nextVblank) != 0u || (shotEvery != 0u && (nextVblank % shotEvery) == 0u))
            {
                extern bool ps2x_dumpPresentationFramePng(PS2Runtime &, const char *);
                const std::string path = std::string(shotPrefix) + "_" + std::to_string(nextVblank) + ".png";
                ps2x_dumpPresentationFramePng(m_runtime, path.c_str());
            }
        }
    }
    // PS2X_FAST_FORWARD=<N>: run unpaced (no waiting for the host clock, idle time skipped) until the N-th VBlank, then resume real-time pacing.
    // The guest sees the same sequence of events (pacing never reorders them), so with deterministic clocks the state at VBlank N is the same as in a
    // normal run; it is the cheap way to reach a recorded point (see docs/savestates-investigation-2026-10-15.md).
    if (event.type == EeEventType::VBlankStart)
    {
        if (m_userPause.load(std::memory_order_relaxed))
        {
            // PC Extras is in front: hold the emulation here (the guest waits for this VBlank) and, once released, re-base every pending deadline on the
            // host clock like the end of a fast-forward does, so the time spent paused is not paid back as a burst.
            while (m_userPause.load(std::memory_order_relaxed) && !m_stopRequested.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(m_eventMutex);
                for (ScheduledEvent &item : m_deadlines)
                    item.hostDeadline = now + eeCyclesToHostDuration(item.deadlineCycle > m_eeCycle ? item.deadlineCycle - m_eeCycle : 0u);
            }
            m_timerHostEpoch = std::chrono::steady_clock::time_point{};
        }
        ++m_vblankCount;
        // User Speed Up (setUserSpeedUp: a global function of the port, never a pad button): the same unpaced mode as the start up fast-forward while the user
        // holds / toggles it, applied and released on VBlank boundaries; it never ends before the start up fast-forward (if any) is over.
        if (m_userSpeedUp.load(std::memory_order_relaxed) && !m_fastForward.load(std::memory_order_relaxed))
        {
            m_fastForward.store(true, std::memory_order_relaxed);
            ps2x::iop::setAudioFastForward(true);
            m_fastForwardStart = std::chrono::steady_clock::now();
        }
        if (m_fastForward.load(std::memory_order_relaxed) && m_vblankCount >= m_fastForwardUntil && !m_userSpeedUp.load(std::memory_order_relaxed))
        {
            m_fastForward.store(false, std::memory_order_relaxed);
            ps2x::iop::setAudioFastForward(false);
            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard lock(m_eventMutex);
                for (ScheduledEvent &item : m_deadlines)
                    item.hostDeadline = now + eeCyclesToHostDuration(item.deadlineCycle > m_eeCycle ? item.deadlineCycle - m_eeCycle : 0u);
            }
            m_timerHostEpoch = std::chrono::steady_clock::time_point{};
            std::fprintf(stderr, "[fast-forward] reached VBlank %llu at EE cycle %llu after %.2f s of host time; pacing resumes\n", static_cast<unsigned long long>(m_vblankCount),
                         static_cast<unsigned long long>(m_eeCycle), std::chrono::duration<double>(now - m_fastForwardStart).count());
        }
    }
    // PS2X_EE_SAMPLE=<hexaddr>:<words>[,...] (diagnostic): at every VBlank start print those guest words with the EE cycle.
    if (static const char *sample = std::getenv("PS2X_EE_SAMPLE"); sample && event.type == EeEventType::VBlankStart)
    {
        std::string spec(sample);
        size_t at = 0;
        while (at < spec.size())
        {
            size_t end = spec.find(',', at);
            if (end == std::string::npos) end = spec.size();
            const std::string item = spec.substr(at, end - at);
            at = end + 1;
            const size_t colon = item.find(':');
            const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)) & 0x1FFFFFFFu;
            const uint32_t words = colon == std::string::npos ? 1u : static_cast<uint32_t>(std::strtoul(item.c_str() + colon + 1, nullptr, 10));
            std::fprintf(stderr, "[ee-sample] ee=%llu @%x:", static_cast<unsigned long long>(m_eeCycle), address);
            for (uint32_t i = 0; i < words && address + 4u * i + 4u <= PS2_RAM_SIZE; ++i)
            {
                uint32_t value = 0;
                std::memcpy(&value, m_runtime.memory().getRDRAM() + address + 4u * i, 4);
                std::fprintf(stderr, " %x", value);
            }
            std::fprintf(stderr, "%c", 10);
        }
    }
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    publishSnapshot(true); // going idle: let the diagnostics see the stable state
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        // Fast-forward: host deadlines are meaningless (the guest runs far ahead of them), so the earliest event is decided by guest cycles.
        const bool timerFirst = m_fastForward.load(std::memory_order_relaxed) ? (deadlineCycle == 0u || m_eeCycle + timerCycles < deadlineCycle)
                                                                                : (timerHostDeadline < hostDeadline);
        if (timerFirst)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    if (m_fastForward.load(std::memory_order_relaxed))
        hostDeadline = std::chrono::steady_clock::now(); // fast-forward: guest idle time is skipped, not waited for
    timeline("idlewait", m_eeCycle,
             hostDeadline == std::chrono::steady_clock::time_point::max()
                 ? -1
                 : std::chrono::duration_cast<std::chrono::microseconds>(hostDeadline - std::chrono::steady_clock::now()).count(),
             static_cast<int64_t>(deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u));
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        timeline("idlejump", m_eeCycle, static_cast<int64_t>(elapsed), 0);
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        m_nextDeadlineHostTicks.store(0, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineHostTicks.store(it->hostDeadline.time_since_epoch().count(), std::memory_order_release);
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
