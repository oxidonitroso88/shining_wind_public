#pragma once
// Pure helpers for the EE PMFHL instruction family (no runtime dependencies so they can be unit tested on their own).
#include <cstdint>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif

// PMFHL: move from the 128 bit HI/LO pair. The recompiled context stores the two 64 bit halves of each register
// separately (hi/lo = lanes 0..1, hi1/lo1 = lanes 2..3, every lane sign extended from 32 bits by MULT/DIV), so the word
// lanes are LO.w0 = lo[31:0], LO.w1 = lo[63:32], LO.w2 = lo1[31:0], LO.w3 = lo1[63:32] and likewise for HI. The generated
// code passes only (ctx->hi, ctx->lo); the upper lanes are read from ctx->hi1/ctx->lo1, which every call site has in scope.
inline __m128i ps2_pmfhl_words(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3)
{
    return _mm_set_epi32(static_cast<int>(w3), static_cast<int>(w2), static_cast<int>(w1), static_cast<int>(w0));
}

inline int64_t ps2_pmfhl_sat32(uint32_t hiWord, uint32_t loWord)
{
    const int64_t value = static_cast<int64_t>((static_cast<uint64_t>(hiWord) << 32) | loWord);
    if (value > 0x7FFFFFFFll)
        return 0x7FFFFFFFll;
    if (value < -0x80000000ll)
        return static_cast<int64_t>(0xFFFFFFFF80000000ull);
    return static_cast<int64_t>(static_cast<int32_t>(value));
}

inline int16_t ps2_pmfhl_sat16(uint32_t word)
{
    const int32_t value = static_cast<int32_t>(word);
    return static_cast<int16_t>(value > 0x7FFF ? 0x7FFF : (value < -0x8000 ? -0x8000 : value));
}

inline __m128i ps2_pmfhl_lw(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    // rd = {HI.w2, LO.w2, HI.w0, LO.w0} (word 0 is the least significant)
    return ps2_pmfhl_words(static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), static_cast<uint32_t>(lo1), static_cast<uint32_t>(hi1));
}

inline __m128i ps2_pmfhl_uw(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    return ps2_pmfhl_words(static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(hi >> 32), static_cast<uint32_t>(lo1 >> 32), static_cast<uint32_t>(hi1 >> 32));
}

inline __m128i ps2_pmfhl_slw(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    const int64_t low = ps2_pmfhl_sat32(static_cast<uint32_t>(hi), static_cast<uint32_t>(lo));
    const int64_t high = ps2_pmfhl_sat32(static_cast<uint32_t>(hi1), static_cast<uint32_t>(lo1));
    return _mm_set_epi64x(high, low);
}

inline __m128i ps2_pmfhl_lh(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    // rd.h[2k] = LO.h[2k], rd.h[2k+1] = HI.h[2k]
    const uint16_t l0 = static_cast<uint16_t>(lo), l2 = static_cast<uint16_t>(lo >> 32), l4 = static_cast<uint16_t>(lo1), l6 = static_cast<uint16_t>(lo1 >> 32);
    const uint16_t h0 = static_cast<uint16_t>(hi), h2 = static_cast<uint16_t>(hi >> 32), h4 = static_cast<uint16_t>(hi1), h6 = static_cast<uint16_t>(hi1 >> 32);
    return _mm_set_epi16(static_cast<short>(h6), static_cast<short>(l6), static_cast<short>(h4), static_cast<short>(l4), static_cast<short>(h2),
                         static_cast<short>(l2), static_cast<short>(h0), static_cast<short>(l0));
}

inline __m128i ps2_pmfhl_sh(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    // rd.h0..h7 = sat16(LO.w0), sat16(LO.w1), sat16(HI.w0), sat16(HI.w1), sat16(LO.w2), sat16(LO.w3), sat16(HI.w2), sat16(HI.w3)
    return _mm_set_epi16(ps2_pmfhl_sat16(static_cast<uint32_t>(hi1 >> 32)), ps2_pmfhl_sat16(static_cast<uint32_t>(hi1)), ps2_pmfhl_sat16(static_cast<uint32_t>(lo1 >> 32)),
                         ps2_pmfhl_sat16(static_cast<uint32_t>(lo1)), ps2_pmfhl_sat16(static_cast<uint32_t>(hi >> 32)), ps2_pmfhl_sat16(static_cast<uint32_t>(hi)),
                         ps2_pmfhl_sat16(static_cast<uint32_t>(lo >> 32)), ps2_pmfhl_sat16(static_cast<uint32_t>(lo)));
}

