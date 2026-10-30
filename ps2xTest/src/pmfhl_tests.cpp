// PMFHL (move from HI/LO) semantics. Regression: PMFHL.LW used to return the sign extended 64 bit LO in the low
// doubleword instead of {HI.w0, LO.w0}, which broke the GCC soft-float multiply idiom
//   multu rs, rt ; pmfhl.lw rd ; daddu t, rd, zero     (64 bit product of two 32 bit values)
// and every double precision multiplication that went through it.

#include "ps2_pmfhl.h"

#include <cstdio>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool condition, const char *what)
    {
        ++g_checks;
        if (!condition)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what);
        }
    }

    uint64_t lane64(__m128i v, int index)
    {
        alignas(16) uint64_t out[2];
        _mm_store_si128(reinterpret_cast<__m128i *>(out), v);
        return out[index];
    }

    // MULTU result as the recompiled code stores it: both words sign extended to 64 bits.
    void multu(uint32_t a, uint32_t b, uint64_t &hi, uint64_t &lo)
    {
        const uint64_t product = static_cast<uint64_t>(a) * b;
        lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(product)));
        hi = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(product >> 32)));
    }
}

int main()
{
    // 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001: the low doubleword of PMFHL.LW is the whole product.
    uint64_t hi = 0, lo = 0;
    multu(0xFFFFFFFFu, 0xFFFFFFFFu, hi, lo);
    check(lane64(ps2_pmfhl_lw(hi, lo, 0, 0), 0) == 0xFFFFFFFE00000001ull, "LW low doubleword is the 64 bit product");

    multu(0x12345678u, 0x9ABCDEF0u, hi, lo);
    check(lane64(ps2_pmfhl_lw(hi, lo, 0, 0), 0) == 0x12345678ull * 0x9ABCDEF0ull, "LW product of arbitrary operands");

    multu(3u, 5u, hi, lo);
    check(lane64(ps2_pmfhl_lw(hi, lo, 0, 0), 0) == 15ull, "LW small product");

    // Upper lanes come from the second HI/LO pair (MULT1): {HI.w2, LO.w2}.
    uint64_t hi1 = 0, lo1 = 0;
    multu(0xDEADBEEFu, 0xCAFEBABEu, hi1, lo1);
    check(lane64(ps2_pmfhl_lw(hi, lo, hi1, lo1), 1) == 0xDEADBEEFull * 0xCAFEBABEull, "LW upper doubleword from HI1/LO1");

    // UW takes words 1 and 3 (the sign extension of 32 bit results): negative low word -> 0xFFFFFFFF in both.
    const uint64_t negLo = 0xFFFFFFFF80000000ull;
    const uint64_t negHi = 0xFFFFFFFFFFFFFFFFull;
    check(lane64(ps2_pmfhl_uw(negHi, negLo, 0, 0), 0) == 0xFFFFFFFFFFFFFFFFull, "UW uses word 1 of HI and LO");

    // SLW saturates the 64 bit value {HI.w0, LO.w0} to a signed 32 bit result.
    multu(0x00010000u, 0x00010000u, hi, lo); // 0x1_0000_0000 > INT32_MAX
    check(lane64(ps2_pmfhl_slw(hi, lo, 0, 0), 0) == 0x7FFFFFFFull, "SLW saturates high");
    check(lane64(ps2_pmfhl_slw(0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0, 0), 0) == 0xFFFFFFFFFFFFFFFFull, "SLW keeps -1");
    check(lane64(ps2_pmfhl_slw(0xFFFFFFFFFFFFFFFEull, 0x0ull, 0, 0), 0) == 0xFFFFFFFF80000000ull, "SLW saturates low");

    // LH interleaves the even halfwords of LO and HI.
    const __m128i lh = ps2_pmfhl_lh(0x0000000000004444ull, 0x0000000000001111ull, 0, 0);
    alignas(16) uint16_t halves[8];
    _mm_store_si128(reinterpret_cast<__m128i *>(halves), lh);
    check(halves[0] == 0x1111 && halves[1] == 0x4444, "LH interleaves LO.h0 and HI.h0");

    // SH saturates each word to a signed halfword.
    const __m128i sh = ps2_pmfhl_sh(0, 0x0000800000000001ull, 0, 0); // LO.w0 = 1, LO.w1 = 0x8000
    _mm_store_si128(reinterpret_cast<__m128i *>(halves), sh);
    check(halves[0] == 1 && halves[1] == 0x7FFF, "SH saturates words to halfwords");

    std::printf("PMFHL tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
