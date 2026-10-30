// Host arena (ps2_host_arena.h): regression for the heap corruption that froze Shining Wind at 0x100A9C. The runtime's own guest
// heap (RPC staging buffers etc.) used to start right after the ELF, inside the heap that the game's allocator manages itself, so
// IOP replies landed on the game's allocator headers.
#include "MiniTest.h"
#include "ps2_host_arena.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"

#include <cstdint>
#include <cstring>

void register_ps2_host_arena_tests()
{
    MiniTest::Case("PS2HostArena", [](TestCase &tc)
    {
        tc.Run("by default the host heap follows the ELF, SetupHeap reconfigures it (other games rely on that)", [](TestCase &t)
        {
            ps2_host_arena::unpinForTests();
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            R5900Context ctx{};
            SET_GPR_U32(&ctx, 4, 0x45C980u);
            SET_GPR_U32(&ctx, 5, 0xFFFFFFFFu);
            ps2_syscalls::dispatchNumericSyscall(0x3Du, runtime.memory().getRDRAM(), &ctx, &runtime);
            t.Equals(runtime.guestHeapBase(), 0x45C980u, "SetupHeap defines the heap base");
            const uint32_t block = runtime.guestMalloc(64u, 16u);
            t.IsTrue(block >= 0x45C980u, "host allocations come from the program heap in this mode");
        });

        tc.Run("a pinned arena keeps host allocations out of the game heap and SetupHeap/EndOfHeap keep describing the program heap", [](TestCase &t)
        {
            ps2_host_arena::unpinForTests();
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "runtime memory initialize should succeed");
            t.IsFalse(ps2_host_arena::pin(runtime, 0u, 0x100000u), "base 0 is rejected");
            t.IsFalse(ps2_host_arena::pin(runtime, 0x100000u, 0x80000u), "empty range is rejected");
            t.IsTrue(ps2_host_arena::pin(runtime, 0x80000u, 0x100000u), "arena below the ELF");
            t.IsTrue(ps2_host_arena::pinned(), "pinned");

            R5900Context ctx{};
            SET_GPR_U32(&ctx, 4, 0x45C980u); // the game: SetupHeap(_end, -1)
            SET_GPR_U32(&ctx, 5, 0xFFFFFFFFu);
            ps2_syscalls::dispatchNumericSyscall(0x3Du, runtime.memory().getRDRAM(), &ctx, &runtime);
            t.Equals(getRegU32(&ctx, 2), 0x45C980u, "SetupHeap returns the base the game asked for");
            t.Equals(runtime.guestHeapBase(), 0x80000u, "...and does not move the host heap into the game's memory");

            ps2_syscalls::dispatchNumericSyscall(0x3Eu, runtime.memory().getRDRAM(), &ctx, &runtime);
            t.Equals(getRegU32(&ctx, 2), 0x01F00000u, "EndOfHeap still reports the program heap end");

            for (uint32_t i = 0; i < 200u; ++i)
            {
                const uint32_t block = runtime.guestMalloc(256u, 16u);
                t.IsTrue(block >= 0x80000u && block + 256u <= 0x100000u, "every host allocation stays inside the arena");
                if (block == 0u)
                    break;
            }
            // exhaustion fails cleanly instead of spilling into game memory
            uint32_t last = 1u;
            for (int i = 0; i < 4096 && last != 0u; ++i)
                last = runtime.guestMalloc(0x10000u, 16u);
            t.Equals(last, 0u, "an exhausted arena returns 0 and never an address outside it");
            ps2_host_arena::unpinForTests();
        });
    });
}
