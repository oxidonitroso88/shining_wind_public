#ifndef PS2_OVERLAY_H
#define PS2_OVERLAY_H

// Code overlays: several code images that are loaded one at a time at the same guest address (Shining Wind: OVL00..OVL10
// at 0x452200). Each overlay is recompiled on its own (`overlay_name` in the recompiler config) into a Descriptor that
// registers itself here. The functions of an overlay are NOT in the dense main table (they would overwrite each other);
// a guest PC inside the overlay region is resolved against the overlay whose code is in guest memory right now.
//
// The active overlay is never inferred from "a file was opened": it is the registered overlay whose compiled code bytes
// (FNV-1a 64 over the code range) are exactly what the guest memory holds. A cheap sample check runs on every resolve;
// the full hash runs when the samples differ, on the first resolve, and after the guest flushed the instruction cache
// (FlushCache/iFlushCache: the point where real hardware makes newly copied code executable).
// Design and results: docs/overlays-design-2026-10-12.md

#include "ps2_runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ps2_overlay
{
    struct Function
    {
        uint32_t address;
        PS2Runtime::RecompiledFunction function;
    };

    struct Descriptor
    {
        const char *name;
        uint32_t codeBegin; // executable range the functions were generated from
        uint32_t codeEnd;
        uint64_t codeHash;           // hashBytes() of the code bytes
        const uint32_t *sampleWords; // word at codeBegin + i * sampleStride
        uint32_t sampleCount;
        uint32_t sampleStride; // bytes
        const Function *functions; // sorted by address (includes resume entries)
        uint32_t functionCount;
        // Optional second executable range (0/0 = none): code that the linker left after the data of the overlay (static
        // constructors). It is not part of the identity (hash/samples): only the first range identifies the overlay.
        uint32_t extraBegin;
        uint32_t extraEnd;
    };

    // FNV-1a 64. The recompiler emits the same hash (function_table_emitter.cpp).
    inline uint64_t hashBytes(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    // Called by the generated registration code (static initialisation).
    void registerOverlay(const Descriptor *descriptor);

    // Extra functions of the main program that the function map missed (generated with `supplement_name`): written into the
    // dense main table at start-up, only into slots that are still empty. Returns the number of slots set.
    uint32_t registerSupplement(const char *name, const Function *functions, uint32_t count);

    // True if `address` lies in the union of the registered overlay code ranges.
    bool inOverlayRegion(uint32_t address);

    // Function registered at exactly `address` in the overlay whose code is currently in `rdram`, or nullptr
    // (no overlay installed, installed code unknown, or `address` is not an entry of the installed overlay).
    PS2Runtime::RecompiledFunction resolve(const uint8_t *rdram, uint32_t address);

    // The guest made newly written code executable (instruction-cache flush): re-verify the installed overlay.
    void notifyCodeFlush();

    // Overlay currently installed according to the last verification (nullptr if none). Does not re-verify.
    const Descriptor *active();

    // One-line state for diagnostics (active overlay, switch count, header words of the region).
    std::string describe(const uint8_t *rdram);

    // Test support.
    void resetForTests();
    uint32_t registeredCount();
    uint64_t switchCount();
} // namespace ps2_overlay

#endif // PS2_OVERLAY_H
