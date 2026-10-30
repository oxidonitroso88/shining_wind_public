// GS local memory footprint of a transfer or a buffer, in 256 byte blocks. Used by the texture provenance ledger to decide which tracked uploads a later write
// really overwrites (see tex_provenance.cpp and docs/hd-block-span-exact-2026-10-29.md).
#pragma once

#include <algorithm>
#include <cstdint>

namespace texprov
{
    // Page size in pixels of a storage mode (64x32 for the 32 bit family, the default).
    inline void pageSize(uint32_t psm, uint32_t &w, uint32_t &h)
    {
        switch (psm)
        {
        case 0x02: case 0x0A: case 0x32: case 0x3A: w = 64; h = 64; break;
        case 0x13: w = 128; h = 64; break;
        case 0x14: w = 128; h = 128; break;
        default: w = 64; h = 32; break;
        }
    }

    // Block layout of a pixel storage mode: block size in pixels, page size in pixels and the block number inside a page (the tables of ps2_gs_memory.cpp).
    struct BlockLayout
    {
        uint32_t blockW, blockH, pageW, pageH;
        const uint8_t *order; // [pageH / blockH][pageW / blockW] block numbers inside a 32 block page
    };

    inline constexpr uint8_t kOrder32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23}, {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
    inline constexpr uint8_t kOrderZ32[4][8] = {{24, 25, 28, 29, 8, 9, 12, 13}, {26, 27, 30, 31, 10, 11, 14, 15}, {16, 17, 20, 21, 0, 1, 4, 5}, {18, 19, 22, 23, 2, 3, 6, 7}};
    inline constexpr uint8_t kOrder16[8][4] = {{0, 2, 8, 10}, {1, 3, 9, 11}, {4, 6, 12, 14}, {5, 7, 13, 15}, {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
    inline constexpr uint8_t kOrder16S[8][4] = {{0, 2, 16, 18}, {1, 3, 17, 19}, {8, 10, 24, 26}, {9, 11, 25, 27}, {4, 6, 20, 22}, {5, 7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31}};
    inline constexpr uint8_t kOrderZ16[8][4] = {{24, 26, 16, 18}, {25, 27, 17, 19}, {28, 30, 20, 22}, {29, 31, 21, 23}, {8, 10, 0, 2}, {9, 11, 1, 3}, {12, 14, 4, 6}, {13, 15, 5, 7}};
    inline constexpr uint8_t kOrderZ16S[8][4] = {{24, 26, 8, 10}, {25, 27, 9, 11}, {16, 18, 0, 2}, {17, 19, 1, 3}, {28, 30, 12, 14}, {29, 31, 13, 15}, {20, 22, 4, 6}, {21, 23, 5, 7}};

    inline bool blockLayoutOf(uint32_t psm, BlockLayout &l)
    {
        switch (psm)
        {
        case 0x00: case 0x01: case 0x1B: case 0x24: case 0x2C: l = {8, 8, 64, 32, &kOrder32[0][0]}; return true;
        case 0x30: case 0x31: l = {8, 8, 64, 32, &kOrderZ32[0][0]}; return true;
        case 0x02: l = {16, 8, 64, 64, &kOrder16[0][0]}; return true;
        case 0x0A: l = {16, 8, 64, 64, &kOrder16S[0][0]}; return true;
        case 0x32: l = {16, 8, 64, 64, &kOrderZ16[0][0]}; return true;
        case 0x3A: l = {16, 8, 64, 64, &kOrderZ16S[0][0]}; return true;
        case 0x13: l = {16, 16, 128, 64, &kOrder32[0][0]}; return true;
        case 0x14: l = {32, 16, 128, 128, &kOrder16[0][0]}; return true;
        default: return false;
        }
    }

    // Range of block addresses (256 byte blocks) touched by a width x height region starting at (x, y) of a buffer: the first and last block the pixels really
    // land on, so small uploads that share a page with other data (a palette next to its texture) do not erase each other. An unknown storage mode, or a
    // region too big to walk block by block, falls back to whole pages.
    inline void blockSpan(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t &lo, uint32_t &hi)
    {
        w = std::max(w, 1u);
        h = std::max(h, 1u);
        BlockLayout l{};
        if (!blockLayoutOf(psm, l))
        {
            uint32_t pw = 64, ph = 32;
            pageSize(psm, pw, ph);
            const uint32_t stride = std::max(1u, (std::max(bw, 1u) * 64u + pw - 1u) / pw);
            const uint32_t pagesX = std::max(1u, (x + w + pw - 1u) / pw);
            const uint32_t pagesY = std::max(1u, (y + h + ph - 1u) / ph);
            lo = bp;
            // pages are 32 blocks; a base that is not page aligned shares its first page with whatever precedes it, so it costs one more page
            hi = bp + (stride * (pagesY - 1u) + std::min(pagesX, stride)) * 32u + ((bp & 31u) ? 32u : 0u);
            return;
        }
        const uint32_t stride = std::max(1u, (std::max(bw, 1u) * 64u + l.pageW - 1u) / l.pageW); // pages per row of the buffer
        const uint32_t perPageX = l.pageW / l.blockW, perPageY = l.pageH / l.blockH;
        const uint32_t bx0 = x / l.blockW, bx1 = (x + w - 1u) / l.blockW, by0 = y / l.blockH, by1 = (y + h - 1u) / l.blockH;
        uint32_t first = ~0u, last = 0u;
        if (static_cast<uint64_t>(bx1 - bx0 + 1u) * (by1 - by0 + 1u) > 16384u)
        {
            first = ((by0 / perPageY) * stride + bx0 / perPageX) * 32u;
            last = ((by1 / perPageY) * stride + bx1 / perPageX) * 32u + 31u;
        }
        else
        {
            for (uint32_t by = by0; by <= by1; ++by)
                for (uint32_t bx = bx0; bx <= bx1; ++bx)
                {
                    const uint32_t a = ((by / perPageY) * stride + bx / perPageX) * 32u + l.order[(by % perPageY) * perPageX + bx % perPageX];
                    first = std::min(first, a);
                    last = std::max(last, a);
                }
        }
        lo = bp + first;
        hi = bp + last + 1u;
    }
}
