#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#define GS_POOL_PAUSE() _mm_pause()
#else
#define GS_POOL_PAUSE() ((void)0)
#endif

// Splits the rows of one primitive into bands that a small group of helper threads and the calling thread draw
// concurrently. Bands are disjoint, so every pixel is still written by exactly one thread and the result is identical to
// drawing the rows in order. Run() returns when all bands are done. Helpers spin briefly for new work and then sleep.
//
// Claiming protocol: one 64-bit ticket = [generation:32][band count:16][next band:16]. A helper reads generation, count and next
// from the SAME load and claims by CAS on the whole word, so a straggler left over from the previous Run can never claim a band
// of the next Run (an earlier version kept the count in a separate atomic: a straggler holding the old, exhausted ticket could pass
// the count check of the new Run, run a phantom band with stale parameters and push m_done past the band total, hanging Run()).
class GSRasterPool
{
public:
    using BandFn = void (*)(void *user, unsigned context, int yBegin, int yEnd);

    explicit GSRasterPool(unsigned helpers)
    {
        for (unsigned i = 0; i < helpers; ++i)
            m_threads.emplace_back([this, i]() { workerLoop(i + 1u); });
    }

    ~GSRasterPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop.store(true, std::memory_order_release);
        }
        m_cv.notify_all();
        for (auto &t : m_threads)
            t.join();
    }

    unsigned helpers() const { return static_cast<unsigned>(m_threads.size()); }

    // fn(user, context, yBegin, yEnd): context 0 is the caller, 1..helpers() the helpers.
    void Run(int begin, int end, int bandRows, BandFn fn, void *user)
    {
        if (end <= begin)
            return;
        bandRows = bandRows < 1 ? 1 : bandRows;
        if ((end - begin + bandRows - 1) / bandRows > 0xFFFF) // the ticket holds 16 bits of band count
            bandRows = (end - begin + 0xFFFE) / 0xFFFF;
        const int bands = (end - begin + bandRows - 1) / bandRows;
        m_begin = begin;
        m_end = end;
        m_bandRows = bandRows;
        m_fn = fn;
        m_user = user;
        m_done.store(0, std::memory_order_relaxed);
        const uint32_t g = m_generation.load(std::memory_order_relaxed) + 1u;
        // Publish the parameters above together with generation + band count + first band (release: a helper that claims a band
        // through an acquire load of the ticket sees them).
        m_ticket.store((static_cast<uint64_t>(g) << 32) | (static_cast<uint64_t>(bands) << 16), std::memory_order_release);
        m_generation.store(g); // seq_cst: pairs with the sleepers handshake below
        if (m_sleepers.load() != 0)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cv.notify_all();
        }

        claimLoop(0u, g);
        while (m_done.load(std::memory_order_acquire) < static_cast<uint32_t>(bands))
            GS_POOL_PAUSE();
    }

private:
    // Claims bands of generation g until none are left. Parameters are read only after a successful claim: while a band
    // is claimed, Run() has not returned, so they are stable.
    void claimLoop(unsigned context, uint32_t g)
    {
        for (;;)
        {
            uint64_t ticket = m_ticket.load(std::memory_order_acquire);
            if (static_cast<uint32_t>(ticket >> 32) != g)
                return;
            const uint32_t band = static_cast<uint32_t>(ticket & 0xFFFFu);
            const uint32_t count = static_cast<uint32_t>((ticket >> 16) & 0xFFFFu);
            if (band >= count)
                return;
            if (!m_ticket.compare_exchange_weak(ticket, ticket + 1u, std::memory_order_acq_rel))
                continue;
            const int y0 = m_begin + static_cast<int>(band) * m_bandRows;
            const int y1 = y0 + m_bandRows < m_end ? y0 + m_bandRows : m_end;
            m_fn(m_user, context, y0, y1);
            m_done.fetch_add(1, std::memory_order_release);
        }
    }

    void workerLoop(unsigned context)
    {
        uint32_t seen = 0;
        for (;;)
        {
            uint32_t g = m_generation.load(std::memory_order_acquire);
            if (g == seen)
            {
                // Spin a little (primitives arrive back to back while a frame is drawn), then sleep.
                for (int i = 0; i < 4000 && g == seen; ++i)
                {
                    GS_POOL_PAUSE();
                    g = m_generation.load(std::memory_order_acquire);
                }
                if (g == seen)
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_sleepers.fetch_add(1);
                    m_cv.wait(lock, [&]() { return m_stop.load(std::memory_order_acquire) || m_generation.load(std::memory_order_acquire) != seen; });
                    m_sleepers.fetch_sub(1);
                    g = m_generation.load(std::memory_order_acquire);
                }
            }
            if (m_stop.load(std::memory_order_acquire))
                return;
            seen = g;
            claimLoop(context, g);
        }
    }

    std::vector<std::thread> m_threads;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_generation{0};
    std::atomic<uint32_t> m_sleepers{0};
    std::atomic<uint64_t> m_ticket{0};
    std::atomic<uint32_t> m_done{0};
    int m_begin = 0, m_end = 0, m_bandRows = 1;
    BandFn m_fn = nullptr;
    void *m_user = nullptr;
};
