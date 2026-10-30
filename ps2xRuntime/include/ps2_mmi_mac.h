#pragma once
// EE MMI parallel halfword multiply family (PMULTH, PMADDH, PMSUBH, PHMADH) as pure helpers (unit tested without the runtime).
// The 128 bit HI/LO pair lives in the recompiled context as four 64 bit halves: lo/hi = lanes 0..1, lo1/hi1 = lanes 2..3, i.e.
// LO.w0 = lo[31:0], LO.w1 = lo[63:32], LO.w2 = lo1[31:0], LO.w3 = lo1[63:32] (same for HI), exactly what ps2_pmfhl.h reads.
//
// Semantics (R5900): with p[i] = rs.h[i] * rt.h[i] (signed 16 x 16 -> 32)
//   PMULTH : LO.w0=p0 LO.w1=p1 HI.w0=p2 HI.w1=p3 LO.w2=p4 LO.w3=p5 HI.w2=p6 HI.w3=p7
//   PMADDH / PMSUBH : the same lanes, accumulated (+= / -=) onto the previous HI/LO
//   PHMADH : LO.w1=p1 LO.w0=p1+p0 HI.w1=p3 HI.w0=p3+p2 LO.w3=p5 LO.w2=p5+p4 HI.w3=p7 HI.w2=p7+p6
//   all four: rd = { LO.w0, HI.w0, LO.w2, HI.w2 } (word 0 first).
// The earlier generated code multiplied rs by a shuffled copy of rs (never rt), truncated the products to 16 bits and summed all lanes
// into one 64 bit value: AHX voice decoding (CRI MPEG audio, function 0x13b5d0) turned into full-scale noise.
#include "ps2_pmfhl.h"

#include <cstdint>

struct Ps2MmiHiLo
{
    uint32_t lo[4];
    uint32_t hi[4];
};

inline Ps2MmiHiLo ps2_mmi_unpack(uint64_t hi, uint64_t lo, uint64_t hi1, uint64_t lo1)
{
    Ps2MmiHiLo r;
    r.lo[0] = static_cast<uint32_t>(lo);
    r.lo[1] = static_cast<uint32_t>(lo >> 32);
    r.lo[2] = static_cast<uint32_t>(lo1);
    r.lo[3] = static_cast<uint32_t>(lo1 >> 32);
    r.hi[0] = static_cast<uint32_t>(hi);
    r.hi[1] = static_cast<uint32_t>(hi >> 32);
    r.hi[2] = static_cast<uint32_t>(hi1);
    r.hi[3] = static_cast<uint32_t>(hi1 >> 32);
    return r;
}

inline void ps2_mmi_pack(const Ps2MmiHiLo &r, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    lo = static_cast<uint64_t>(r.lo[0]) | (static_cast<uint64_t>(r.lo[1]) << 32);
    lo1 = static_cast<uint64_t>(r.lo[2]) | (static_cast<uint64_t>(r.lo[3]) << 32);
    hi = static_cast<uint64_t>(r.hi[0]) | (static_cast<uint64_t>(r.hi[1]) << 32);
    hi1 = static_cast<uint64_t>(r.hi[2]) | (static_cast<uint64_t>(r.hi[3]) << 32);
}

inline void ps2_mmi_halfword_products(__m128i rs, __m128i rt, int32_t p[8])
{
    alignas(16) int16_t a[8], b[8];
    _mm_store_si128(reinterpret_cast<__m128i *>(a), rs);
    _mm_store_si128(reinterpret_cast<__m128i *>(b), rt);
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
}

inline __m128i ps2_mmi_result(const Ps2MmiHiLo &r)
{
    return ps2_pmfhl_words(r.lo[0], r.hi[0], r.lo[2], r.hi[2]);
}

// mode: 0 = PMULTH, 1 = PMADDH, 2 = PMSUBH
inline __m128i ps2_pmac_h(__m128i rs, __m128i rt, int mode, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    int32_t p[8];
    ps2_mmi_halfword_products(rs, rt, p);
    Ps2MmiHiLo r = ps2_mmi_unpack(hi, lo, hi1, lo1);
    // lane order of the products inside LO/HI
    uint32_t *const dst[8] = {&r.lo[0], &r.lo[1], &r.hi[0], &r.hi[1], &r.lo[2], &r.lo[3], &r.hi[2], &r.hi[3]};
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t v = static_cast<uint32_t>(p[i]);
        *dst[i] = mode == 0 ? v : (mode == 1 ? *dst[i] + v : *dst[i] - v);
    }
    ps2_mmi_pack(r, hi, lo, hi1, lo1);
    return ps2_mmi_result(r);
}

inline __m128i ps2_pmulth(__m128i rs, __m128i rt, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    return ps2_pmac_h(rs, rt, 0, hi, lo, hi1, lo1);
}

inline __m128i ps2_pmaddh(__m128i rs, __m128i rt, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    return ps2_pmac_h(rs, rt, 1, hi, lo, hi1, lo1);
}

inline __m128i ps2_pmsubh(__m128i rs, __m128i rt, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    return ps2_pmac_h(rs, rt, 2, hi, lo, hi1, lo1);
}

inline __m128i ps2_phmadh(__m128i rs, __m128i rt, uint64_t &hi, uint64_t &lo, uint64_t &hi1, uint64_t &lo1)
{
    int32_t p[8];
    ps2_mmi_halfword_products(rs, rt, p);
    Ps2MmiHiLo r;
    r.lo[1] = static_cast<uint32_t>(p[1]);
    r.lo[0] = static_cast<uint32_t>(p[1] + p[0]);
    r.hi[1] = static_cast<uint32_t>(p[3]);
    r.hi[0] = static_cast<uint32_t>(p[3] + p[2]);
    r.lo[3] = static_cast<uint32_t>(p[5]);
    r.lo[2] = static_cast<uint32_t>(p[5] + p[4]);
    r.hi[3] = static_cast<uint32_t>(p[7]);
    r.hi[2] = static_cast<uint32_t>(p[7] + p[6]);
    ps2_mmi_pack(r, hi, lo, hi1, lo1);
    return ps2_mmi_result(r);
}
