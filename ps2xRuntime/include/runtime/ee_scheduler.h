#pragma once

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    // Deferred transports can validate their reset generation and commit the
    // packet only when the guest invocation actually starts.
    std::function<bool()> onBegin;
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    std::vector<GuestInvocation> invocations;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
public:
    // Speed Up: run unpaced (like PS2X_FAST_FORWARD, audio not sent to the device) while on. Called by the host input layer; applied on the next VBlank.
    void setUserSpeedUp(bool on) { m_userSpeedUp.store(on, std::memory_order_relaxed); }
    bool userSpeedUp() const { return m_userSpeedUp.load(std::memory_order_relaxed); }
    // PC Extras: stop the emulation at the next VBlank while on; when it resumes the pending timers are re-based on the host clock so nothing catches up.
    void setUserPause(bool on) { m_userPause.store(on, std::memory_order_relaxed); }
    bool userPause() const { return m_userPause.load(std::memory_order_relaxed); }

    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    // Host-clock catch-up of the guest clock. By default, when the host clock passes the real-time deadline of the next event (VBlank,
    // alarm) before the accounted EE cycles reach it, the EE cycle counter - and with it the IOP and everything timed in cycles - jumps
    // forward to the event. That keeps a slow host from starving timers, but it makes guest time depend on host speed: in the opening
    // movie it credits ~10 ms of guest time in the middle of the audio pipeline's work and the ADX/PCM stream starves (docs/
    // audio-ee-iop-timing-2026-10-14.md). With catch-up disabled (setHostCatchup(false) or PS2X_NO_HOST_CATCHUP=1) guest time only
    // follows the executed work and idle jumps; the real-time pacing wait when the host is ahead is unchanged, so a slow host makes the
    // game run slower instead of making it skip guest time.
    // Always = the original behaviour; IdleOnly = credit the time only while the running thread is the lowest-priority live thread (the
    // game's idle/poll loop), so work threads - the CRI/ADX servers, the demuxer - never see guest time jump under them; Never = off.
    enum class HostCatchupMode : uint8_t { Always, IdleOnly, Never };
    // Elapsed cycles reach the EE timers and the IOP in batches (kDeviceBatchCycles) instead of at every one of the ~8 million guest
    // checkpoints per second; anything that observes them (timer registers, IOP RPC/SIF/memory access, idle waits) calls this first, so
    // they are never behind at an observation point. The batching depends on cycles only, not on host time: it is deterministic.
    void flushDeviceTime() noexcept;
    static constexpr uint64_t kDeviceBatchCycles = 256ull;
    void setHostCatchupMode(HostCatchupMode mode) noexcept { m_hostCatchupMode = mode; }
    [[nodiscard]] HostCatchupMode hostCatchupMode() const noexcept { return m_hostCatchupMode; }
    void setHostCatchup(bool enabled) noexcept { m_hostCatchupMode = enabled ? HostCatchupMode::Always : HostCatchupMode::Never; }
    [[nodiscard]] bool hostCatchup() const noexcept { return m_hostCatchupMode != HostCatchupMode::Never; }
    [[nodiscard]] uint64_t hostCatchupBusyCycles() const noexcept { return m_hostCatchupBusyCycles; }
    [[nodiscard]] uint64_t hostCatchupCycles() const noexcept { return m_hostCatchupCycles; }
    // Idle-spin fast-forward (see tryIdleSkip). On by default; tests that spin on purpose and measure raw cycle accounting switch it off.
    void setIdleSkip(bool on) noexcept { m_idleSkipEnabled = on; }
    void accountCycles(uint32_t cycles) noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    bool queueSifPacket(std::function<bool(bool)> begin, std::function<void()> complete);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    // The snapshot is a diagnostics view (debug panel, aggressive logs, tests). Rebuilding it on every scheduler primitive (~40 call sites)
    // took >50 % of the EE thread's host time in the game's field phase (docs/ee-thread-performance-2026-10-14.md), so it is rebuilt
    // when a consumer asked for it since the last build, when forced (the scheduler goes idle) and otherwise once every 64 calls.
    void publishSnapshot(bool force = false);

private:
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    // True while the running thread is executing an interrupt-context invocation (INTC/DMAC handler, alarm, GS callback,
    // SIF command). The EE kernel never switches threads inside such a handler: a wakeup raised by it takes effect when it returns.
    [[nodiscard]] bool runningInInterruptContext() const;
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Runtime &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    GuestThread *m_threadCache = nullptr; // last thread found by id (unordered_map nodes are stable); reset when threads are erased
    int m_threadCacheId = 0;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    std::thread::id m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};
    // Host deadline (steady_clock ticks) of the event in m_nextDeadlineCycle and a poll counter: when the guest
    // runs slower than real time its cycle counter lags the host clock, so a due host deadline warps the
    // cycle counter forward (see checkpointDue) instead of starving VBlank and timers.
    std::atomic<int64_t> m_nextDeadlineHostTicks{0};
    uint32_t m_hostDeadlinePoll = 0;
    HostCatchupMode m_hostCatchupMode = HostCatchupMode::Never;
    uint64_t m_hostCatchupBusyCycles = 0; // part of the credit that landed while a non-idle thread was running
    [[nodiscard]] bool runningThreadIsIdle() const;
    // Idle-spin fast-forward (PS2X_IDLE_SKIP=0 disables it): the lowest-priority thread of this game polls flags in a tight loop and eats most of
    // the host time. When such a thread reaches the same loop checkpoint with exactly the same register file twice in a row, the next iteration
    // is provably identical until some event changes memory, so guest time is advanced (at most kIdleSkipCycles at a time, never past the next
    // scheduled event) instead of executing the iterations. Guest-visible state is the same as after running them.
    struct IdleSlot
    {
        uint32_t pc = 0;
        uint32_t same = 0;
        bool valid = false;
        __m128i r[32];
        uint64_t hi = 0, lo = 0, hi1 = 0, lo1 = 0;
    };
    static constexpr uint32_t kIdleSkipCycles = 60000u;
    std::atomic<bool> m_fastForward{false}; // PS2X_FAST_FORWARD=<N>
    std::atomic<bool> m_userPause{false};   // PC Extras is in front (see setUserPause)
    std::atomic<bool> m_userSpeedUp{false}; // the user holds / toggled Speed Up (global function of the port, see setUserSpeedUp)
    uint64_t m_fastForwardUntil = 0, m_vblankCount = 0;
    std::chrono::steady_clock::time_point m_fastForwardStart{};
    bool m_idleSkipEnabled = true;
    bool m_idleCandidate = false;
    uint32_t m_idleProbe = 0;
    IdleSlot m_idleSlots[8];
public:
    uint64_t m_idleSkips = 0, m_idleSkippedCycles = 0; // diagnostics
private:
    void tryIdleSkip() noexcept;
    [[nodiscard]] bool pendingEventWork() const noexcept;
    uint64_t m_pendingDeviceCycles = 0;
    uint64_t m_hostCatchupCycles = 0; // EE cycles credited by the host-clock catch-up (diagnostics and tests)
    // EE timer clock: total EE cycles delivered to the EE timers, and the host-time reference they are
    // kept up with (the guest accounts few cycles per dispatch, so timers would otherwise run far slower
    // than real time and starve middleware that is driven by them).
    uint32_t m_timerPoll = 0;
    uint64_t m_timerCyclesTotal = 0;
    uint64_t m_timerEpochBase = 0;
    std::chrono::steady_clock::time_point m_timerHostEpoch{};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;
    mutable std::atomic<bool> m_snapshotWanted{true};
    uint32_t m_snapshotLazy = 0;
};
