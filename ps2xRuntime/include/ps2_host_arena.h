#ifndef PS2_HOST_ARENA_H
#define PS2_HOST_ARENA_H

// Host-owned guest memory ("arena"). The runtime keeps a heap in guest RAM for its own needs (RPC staging buffers, host
// stubs, fallback thread stacks, ...). By default that heap starts after the ELF and spans the rest of RAM, exactly where a game
// that manages its own memory (Shining Wind: allocator at 0x100830/0x100A20 owning [_end, 0x1EFE000)) keeps its heap: the
// IOP's CDVD replies were written over the game's first heap block and, once the allocator wrapped around the free list, the
// game spun forever at 0x100A9C (docs/heap-corruption-host-arena-2026-10-13.md).
//
// A pinned arena lives in memory the game never touches (below the ELF, 0x80000-0xFFFFF) and SetupHeap/EndOfHeap no longer
// repoint it to the game's heap.

#include <cstdint>

class PS2Runtime;

namespace ps2_host_arena
{
    // Moves the host heap to [base, limit) and makes it immune to SetupHeap. Returns false if the range is invalid.
    bool pin(PS2Runtime &runtime, uint32_t base, uint32_t limit);
    bool pinned();
    void unpinForTests();
    // Kernel view of the program heap end (what EndOfHeap reports to the game), independent of the host arena.
    constexpr uint32_t kKernelHeapEnd = 0x01F00000u;
}

#endif // PS2_HOST_ARENA_H
