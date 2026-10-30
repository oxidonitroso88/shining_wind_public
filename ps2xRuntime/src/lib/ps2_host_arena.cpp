#include "ps2_host_arena.h"
#include "ps2_runtime.h"

#include <atomic>
#include <iostream>

namespace
{
    std::atomic<bool> g_pinned{false};
}

namespace ps2_host_arena
{
    bool pin(PS2Runtime &runtime, uint32_t base, uint32_t limit)
    {
        if (base == 0u || limit <= base || (base & 0xFu) != 0u)
        {
            return false;
        }
        runtime.configureGuestHeap(base, limit);
        g_pinned.store(true, std::memory_order_release);
        std::cerr << "[host-arena] host heap pinned to [0x" << std::hex << base << ", 0x" << limit << ")" << std::dec
                  << ": SetupHeap/EndOfHeap keep describing the program heap" << std::endl;
        return true;
    }

    void unpinForTests()
    {
        g_pinned.store(false, std::memory_order_release);
    }

    bool pinned()
    {
        return g_pinned.load(std::memory_order_acquire);
    }
}
