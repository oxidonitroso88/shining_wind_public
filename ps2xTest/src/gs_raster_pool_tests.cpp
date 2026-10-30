// Stress test for GSRasterPool (band-parallel helper pool of the software rasteriser).
// Every Run() must execute each band exactly once and return; a lost or phantom band used to hang the GS worker forever
// (B-0010: caller spinning in Run() while all helpers sleep). Runs back-to-back Runs with alternating band counts, with extra
// busy threads so helpers are often descheduled mid-loop, and a watchdog that fails the test instead of hanging.
//   ps2_gs_pool_tests [seconds=8]
#include "runtime/gs/gs_raster_pool.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace
{
    struct Probe
    {
        std::vector<std::atomic<int>> hits;
        std::atomic<int> outOfRange{0};
        int begin = 0, end = 0, rows = 1;
        explicit Probe(size_t n) : hits(n) {}
    };

    void band(void *user, unsigned, int y0, int y1)
    {
        auto *p = static_cast<Probe *>(user);
        if (y0 < p->begin || y1 > p->end || y0 >= y1)
        {
            p->outOfRange.fetch_add(1);
            return;
        }
        p->hits[static_cast<size_t>((y0 - p->begin) / p->rows)].fetch_add(1);
        // a little work so helpers overlap with the caller
        volatile uint32_t x = 0;
        for (int i = 0; i < 50; ++i)
            x += static_cast<uint32_t>(i);
    }
}

int main(int argc, char **argv)
{
    const double seconds = argc > 1 ? std::atof(argv[1]) : 8.0;
    const unsigned helpers = 6;
    GSRasterPool pool(helpers);
    Probe probe(512);
    std::atomic<uint64_t> runs{0};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::string failure;

    // Oversubscribe the machine so that pool helpers get preempted at awkward moments.
    std::vector<std::thread> spinners;
    const unsigned spin = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned i = 0; i < spin; ++i)
        spinners.emplace_back([&]() { volatile uint64_t x = 0; while (!done.load(std::memory_order_relaxed)) x += 1; });

    std::thread watchdog([&]() {
        uint64_t last = 0;
        int stuck = 0;
        while (!done.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t now = runs.load();
            stuck = (now == last) ? stuck + 1 : 0;
            last = now;
            if (stuck >= 10)
            {
                std::printf("FAIL: GSRasterPool::Run made no progress for 10 s after %llu runs (lost/phantom band: caller waits forever)\n", static_cast<unsigned long long>(now));
                std::fflush(stdout);
                std::_Exit(1);
            }
        }
    });

    std::mt19937 rng(12345);
    const auto stop = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < stop && !failed.load())
    {
        // Alternate small and large band counts: a new Run with MORE bands than the previous one is the dangerous case.
        const int bands = (rng() % 3 == 0) ? 1 + static_cast<int>(rng() % 4) : 8 + static_cast<int>(rng() % 56);
        probe.rows = 1 + static_cast<int>(rng() % 4);
        probe.begin = static_cast<int>(rng() % 8);
        probe.end = probe.begin + bands * probe.rows;
        for (auto &h : probe.hits) h.store(0);
        pool.Run(probe.begin, probe.end, probe.rows, band, &probe);
        for (int i = 0; i < bands; ++i)
            if (probe.hits[static_cast<size_t>(i)].load() != 1)
            {
                std::printf("FAIL: run %llu band %d/%d executed %d times\n", static_cast<unsigned long long>(runs.load()), i, bands, probe.hits[static_cast<size_t>(i)].load());
                failed = true;
                break;
            }
        if (probe.outOfRange.load() != 0)
        {
            std::printf("FAIL: run %llu a band was executed with a range outside the current Run (stale parameters)\n", static_cast<unsigned long long>(runs.load()));
            failed = true;
        }
        runs.fetch_add(1);
    }
    done = true;
    watchdog.join();
    for (auto &t : spinners) t.join();
    if (failed)
        return 1;
    std::printf("OK: %llu runs, every band executed exactly once, no hang\n", static_cast<unsigned long long>(runs.load()));
    return 0;
}
