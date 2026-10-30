// EE MMI parallel halfword multiply family (PMULTH / PMADDH / PMSUBH / PHMADH). Regression for the Shining Wind battle-voice static:
// the recompiler emitted PHMADH as rs * rs (never rt) with 16-bit truncated products summed into one lane, so the CRI AHX (MPEG audio)
// synthesis in function 0x13b5d0 produced full-scale noise for every battle voice line (docs/ahx-phmadh-battle-static-2026-10-15.md).
#include "MiniTest.h"
#include "ps2_mmi_mac.h"

#include <cstdint>

namespace
{
    __m128i halves(const int16_t (&h)[8])
    {
        return _mm_setr_epi16(h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    }

    void words(__m128i v, int32_t (&w)[4])
    {
        alignas(16) int32_t out[4];
        _mm_store_si128(reinterpret_cast<__m128i *>(out), v);
        for (int i = 0; i < 4; ++i)
            w[i] = out[i];
    }

    const int16_t rs[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const int16_t rt[8] = {10, 20, 30, 40, 50, 60, 70, 80};

    struct HiLo
    {
        uint64_t hi = 0, lo = 0, hi1 = 0, lo1 = 0;
        int32_t LO(int i) const { return static_cast<int32_t>(i < 2 ? (lo >> (32 * i)) : (lo1 >> (32 * (i - 2)))); }
        int32_t HI(int i) const { return static_cast<int32_t>(i < 2 ? (hi >> (32 * i)) : (hi1 >> (32 * (i - 2)))); }
    };
}

void register_ps2_mmi_mac_tests()
{
    MiniTest::Case("EE MMI halfword multiply-accumulate", [](TestCase &tc)
                   {
        // products: 10 40 90 160 250 360 490 640

        tc.Run("PHMADH multiplies rs by rt (not rs by rs) and sums adjacent pairs", [&](TestCase &t) {
            HiLo r;
            int32_t w[4];
            words(ps2_phmadh(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1), w);
            t.IsTrue(w[0] == 50 && w[1] == 250 && w[2] == 610 && w[3] == 1130, "rd = {p0+p1, p2+p3, p4+p5, p6+p7}");
            t.IsTrue(r.LO(0) == 50 && r.LO(1) == 40, "LO.w0 = p0+p1, LO.w1 = p1");
            t.IsTrue(r.HI(0) == 250 && r.HI(1) == 160, "HI.w0 = p2+p3, HI.w1 = p3");
            t.IsTrue(r.LO(2) == 610 && r.LO(3) == 360, "LO.w2 = p4+p5, LO.w3 = p5");
            t.IsTrue(r.HI(2) == 1130 && r.HI(3) == 640, "HI.w2 = p6+p7, HI.w3 = p7");
        });

        tc.Run("PHMADH keeps the full 32-bit products (no 16-bit truncation) and the sign", [&](TestCase &t) {
            const int16_t a[8] = {-32768, 32767, 1000, -1000, 0, 0, 0, 0};
            const int16_t b[8] = {32767, 32767, 300, 300, 0, 0, 0, 0};
            HiLo r;
            int32_t w[4];
            words(ps2_phmadh(halves(a), halves(b), r.hi, r.lo, r.hi1, r.lo1), w);
            t.IsTrue(w[0] == -32768 * 32767 + 32767 * 32767, "p0+p1 with large products");
            t.IsTrue(w[1] == 1000 * 300 - 1000 * 300, "p2+p3 cancel");
        });

        tc.Run("PMULTH writes the eight products to HI/LO and {LO0,HI0,LO2,HI2} to rd", [&](TestCase &t) {
            HiLo r;
            int32_t w[4];
            words(ps2_pmulth(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1), w);
            t.IsTrue(w[0] == 10 && w[1] == 90 && w[2] == 250 && w[3] == 490, "rd");
            t.IsTrue(r.LO(0) == 10 && r.LO(1) == 40 && r.HI(0) == 90 && r.HI(1) == 160, "lanes 0..3");
            t.IsTrue(r.LO(2) == 250 && r.LO(3) == 360 && r.HI(2) == 490 && r.HI(3) == 640, "lanes 4..7");
        });

        tc.Run("PMADDH and PMSUBH accumulate on the previous HI/LO", [&](TestCase &t) {
            HiLo r;
            int32_t w[4];
            (void)ps2_pmulth(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1);
            words(ps2_pmaddh(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1), w);
            t.IsTrue(w[0] == 20 && w[1] == 180 && w[2] == 500 && w[3] == 980, "madd doubles the products");
            words(ps2_pmsubh(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1), w);
            t.IsTrue(w[0] == 10 && w[1] == 90 && w[2] == 250 && w[3] == 490, "msub brings it back");
            t.IsTrue(r.LO(3) == 360 && r.HI(3) == 640, "odd lanes too");
        });

        tc.Run("the results agree with what PMFHL.LW reads back", [&](TestCase &t) {
            HiLo r;
            int32_t w[4], m[4];
            words(ps2_phmadh(halves(rs), halves(rt), r.hi, r.lo, r.hi1, r.lo1), w);
            words(ps2_pmfhl_lw(r.hi, r.lo, r.hi1, r.lo1), m);
            t.IsTrue(w[0] == m[0] && w[1] == m[1] && w[2] == m[2] && w[3] == m[3], "PMFHL.LW == rd");
        }); });
}
