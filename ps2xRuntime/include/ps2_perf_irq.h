#pragma once
// Read-only counters of the EE interrupt-handler tables (perf 0014). They are written by the executor thread with relaxed atomics next to the existing
// add / remove / dispatch code and read by the perf reporter; nothing here influences scheduling or interrupt delivery. Kept out of ps2_perf.h so that
// changing it does not rebuild the whole runtime.
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ps2perf
{
    struct IrqTableStats
    {
        std::atomic<uint64_t> adds{0};         // handlers registered
        std::atomic<uint64_t> removes{0};      // handlers actually erased
        std::atomic<uint64_t> dispatches{0};   // dispatchIrq calls that got past the cause mask
        std::atomic<uint64_t> iterated{0};     // table entries walked by those calls
        std::atomic<uint64_t> matched{0};      // guest invocations queued by those calls
        std::atomic<uint64_t> sifCalls{0};     // queueSifPacket calls (DMAC cause 5 walk of the table, DMAC entry only)
        std::atomic<uint64_t> sifIterated{0};
        std::atomic<uint64_t> sifMatched{0};
        std::atomic<uint32_t> active{0};       // entries in the table now
        std::atomic<uint32_t> maxActive{0};    // largest table since start
        std::atomic<uint32_t> maxMatched{0};   // most invocations queued by one dispatch
        std::atomic<uint32_t> byCause[16]{};   // entries per cause (cause < 16) now
        std::atomic<uint64_t> dispatchByCause[16]{}; // dispatchIrq calls per cause (cause < 16)
    };
    extern IrqTableStats g_irq[2]; // [0] = INTC, [1] = DMAC

    // Per-window work of the parallel-gs renderer (0016, level A): GSInterface::consume_flush_stats() summed once per present while PS2X_PERF is on.
    struct GsFlushStats
    {
        std::atomic<uint64_t> primitives{0}, renderPasses{0}, paletteUpdates{0}, copies{0}, copyBarriers{0}, presents{0};
    };
    extern GsFlushStats g_gsFlush;

    inline void irqAdded(bool dmac, uint32_t cause) noexcept
    {
        IrqTableStats &s = g_irq[dmac ? 1 : 0];
        s.adds.fetch_add(1, std::memory_order_relaxed);
        const uint32_t a = s.active.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (a > s.maxActive.load(std::memory_order_relaxed))
            s.maxActive.store(a, std::memory_order_relaxed);
        if (cause < 16u)
            s.byCause[cause].fetch_add(1, std::memory_order_relaxed);
    }
    inline void irqRemoved(bool dmac, uint32_t cause) noexcept
    {
        IrqTableStats &s = g_irq[dmac ? 1 : 0];
        s.removes.fetch_add(1, std::memory_order_relaxed);
        s.active.fetch_sub(1, std::memory_order_relaxed);
        if (cause < 16u)
            s.byCause[cause].fetch_sub(1, std::memory_order_relaxed);
    }
    inline void irqCleared() noexcept
    {
        for (IrqTableStats &s : g_irq)
        {
            s.active.store(0, std::memory_order_relaxed);
            for (auto &c : s.byCause)
                c.store(0, std::memory_order_relaxed);
        }
    }
    inline void irqDispatched(bool dmac, uint32_t cause, size_t tableSize, uint32_t matched) noexcept
    {
        IrqTableStats &s = g_irq[dmac ? 1 : 0];
        if (cause < 16u)
            s.dispatchByCause[cause].fetch_add(1, std::memory_order_relaxed);
        s.dispatches.fetch_add(1, std::memory_order_relaxed);
        s.iterated.fetch_add(tableSize, std::memory_order_relaxed);
        s.matched.fetch_add(matched, std::memory_order_relaxed);
        if (matched > s.maxMatched.load(std::memory_order_relaxed))
            s.maxMatched.store(matched, std::memory_order_relaxed);
    }
}
