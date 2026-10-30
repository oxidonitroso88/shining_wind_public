// Emotion Engine FPU (COP1) semantics: the R5900 FPU has no NaN/Inf, saturates, flushes denormals, DIV by zero = +-Fmax, SQRT.S/RSQRT.S use |x|,
// CVT.W.S truncates and saturates. Regression for the Shining Wind collision teleport (SQRT.S radicand field + NaN from sqrt of a negative).
#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_fpu.h"

#include <cstdint>
#include <cstring>

namespace
{
    using namespace ps2fpu;

    constexpr float kFmax = 3.4028234663852886e38f; // 0x7F7FFFFF
    constexpr uint32_t kPosInf = 0x7F800000u, kNegInf = 0xFF800000u, kQNaN = 0x7FC00000u, kNegQNaN = 0xFFC00000u;
    constexpr uint32_t kDenormal = 0x00000001u, kNegDenormal = 0x80000001u;

    bool isFinite(float f) { return (bitsOf(f) & 0x7F800000u) != 0x7F800000u; }
    bool same(float a, uint32_t expectedBits) { return bitsOf(a) == expectedBits; }
}

void register_ps2_fpu_tests()
{
    MiniTest::Case("EE FPU semantics", [](TestCase &tc)
                   {
        tc.Run("DIV.S by zero returns +-Fmax with the xor of the signs and never Inf/NaN", [](TestCase &t) {
            uint32_t fcr = 0;
            t.IsTrue(same(div(1.0f, 0.0f, fcr), 0x7F7FFFFFu), "+1 / +0 = +Fmax");
            t.IsTrue((fcr & kFlagD) && (fcr & kFlagSD), "x/0 sets the divide-by-zero flags");
            fcr = 0;
            t.IsTrue(same(div(-1.0f, 0.0f, fcr), 0xFF7FFFFFu), "-1 / +0 = -Fmax");
            t.IsTrue(same(div(1.0f, fromBits(kSignBit), fcr), 0xFF7FFFFFu), "+1 / -0 = -Fmax");
            t.IsTrue(same(div(-1.0f, fromBits(kSignBit), fcr), 0x7F7FFFFFu), "-1 / -0 = +Fmax");
            fcr = 0;
            t.IsTrue(same(div(0.0f, 0.0f, fcr), 0x7F7FFFFFu), "0/0 = +Fmax (never NaN)");
            t.IsTrue((fcr & kFlagI) && (fcr & kFlagSI) && !(fcr & kFlagD), "0/0 sets the invalid flags, not divide-by-zero");
            t.IsTrue(same(div(fromBits(kSignBit), 0.0f, fcr), 0xFF7FFFFFu), "-0/+0 = -Fmax");
        });

        tc.Run("DIV.S normal, overflow, underflow and exponent-255 operands", [](TestCase &t) {
            uint32_t fcr = 0;
            t.Equals(div(6.0f, 3.0f, fcr), 2.0f, "6/3");
            t.Equals(div(-9.0f, 4.0f, fcr), -2.25f, "-9/4");
            t.IsTrue(same(div(kFmax, 0.5f, fcr), 0x7F7FFFFFu), "overflow saturates to +Fmax");
            t.IsTrue(same(div(-kFmax, 0.5f, fcr), 0xFF7FFFFFu), "negative overflow saturates to -Fmax");
            t.IsTrue(same(div(1.0e-30f, 1.0e30f, fcr), 0u), "underflow flushes to +0");
            t.IsTrue(same(div(-1.0e-30f, 1.0e30f, fcr), kSignBit), "negative underflow flushes to -0");
            // An IEEE Inf/NaN bit pattern is an ordinary huge operand on the EE (here +Fmax / 2 = 1.7e38)
            const float r = div(fromBits(kPosInf), 2.0f, fcr);
            t.IsTrue(isFinite(r) && r > 1.0e38f, "Inf operand behaves as Fmax");
            t.IsTrue(isFinite(div(fromBits(kQNaN), 1.0f, fcr)), "NaN pattern operand never produces a NaN");
            t.IsTrue(div(1.0f, fromBits(kNegDenormal), fcr) == fromBits(0xFF7FFFFFu), "a denormal divisor is zero");
        });

        tc.Run("SQRT.S uses the absolute value of a negative operand and flags it", [](TestCase &t) {
            uint32_t fcr = 0;
            t.Equals(sqrt(484.0f, fcr), 22.0f, "sqrt(484) = 22");
            t.Equals(fcr, 0u, "no flag for a valid operand");
            t.Equals(sqrt(-484.0f, fcr), 22.0f, "sqrt(-484) = sqrt(|-484|) = 22, not NaN");
            t.IsTrue((fcr & kFlagI) && (fcr & kFlagSI), "negative operand sets the invalid flags");
            fcr = 0;
            t.IsTrue(same(sqrt(0.0f, fcr), 0u), "sqrt(+0) = +0");
            t.IsTrue(same(sqrt(fromBits(kSignBit), fcr), 0u), "sqrt(-0) = +0");
            t.IsTrue(isFinite(sqrt(fromBits(kNegQNaN), fcr)), "sqrt of a NaN pattern is finite");
            t.IsTrue(isFinite(sqrt(kFmax, fcr)) && sqrt(kFmax, fcr) > 1.8e19f, "sqrt(Fmax) is finite");
            t.IsTrue(same(sqrt(fromBits(kDenormal), fcr), 0u), "a denormal operand is zero");
        });

        tc.Run("RSQRT.S is fs / sqrt(ft) and saturates like DIV.S", [](TestCase &t) {
            uint32_t fcr = 0;
            t.Equals(rsqrt(1.0f, 4.0f, fcr), 0.5f, "1/sqrt(4)");
            t.Equals(rsqrt(8.0f, 16.0f, fcr), 2.0f, "8/sqrt(16): the numerator is fs, not 1");
            t.Equals(fcr, 0u, "no flags");
            t.Equals(rsqrt(2.0f, -4.0f, fcr), 1.0f, "negative ft uses |ft|");
            t.IsTrue((fcr & kFlagI) && (fcr & kFlagSI), "negative ft sets the invalid flags");
            fcr = 0;
            t.IsTrue(same(rsqrt(1.0f, 0.0f, fcr), 0x7F7FFFFFu), "ft = 0 -> +Fmax");
            t.IsTrue((fcr & kFlagD) != 0u, "ft = 0 sets the divide-by-zero flag");
            t.IsTrue(same(rsqrt(-1.0f, 0.0f, fcr), 0xFF7FFFFFu), "fs < 0, ft = 0 -> -Fmax");
            fcr = 0;
            t.IsTrue(same(rsqrt(0.0f, 0.0f, fcr), 0x7F7FFFFFu) && (fcr & kFlagI), "0/sqrt(0) -> Fmax with the invalid flag");
        });

        tc.Run("ADD/SUB/MUL saturate, flush denormals and treat Inf/NaN patterns as huge numbers", [](TestCase &t) {
            t.IsTrue(same(add(kFmax, kFmax), 0x7F7FFFFFu), "Fmax + Fmax = Fmax");
            t.IsTrue(same(add(-kFmax, -kFmax), 0xFF7FFFFFu), "-Fmax + -Fmax = -Fmax");
            t.IsTrue(same(sub(-kFmax, kFmax), 0xFF7FFFFFu), "-Fmax - Fmax = -Fmax");
            t.IsTrue(same(mul(kFmax, 2.0f), 0x7F7FFFFFu), "Fmax * 2 = Fmax");
            t.IsTrue(same(mul(kFmax, -2.0f), 0xFF7FFFFFu), "sign is kept on overflow");
            t.IsTrue(same(mul(1.0e-30f, 1.0e-30f), 0u), "product underflow flushes to +0");
            t.IsTrue(same(mul(-1.0e-30f, 1.0e-30f), kSignBit), "negative product underflow flushes to -0");
            t.IsTrue(same(add(fromBits(kDenormal), 0.0f), 0u), "a denormal operand is +0");
            t.IsTrue(same(add(fromBits(kNegDenormal), fromBits(kNegDenormal)), kSignBit), "-denormal + -denormal = -0");
            t.IsTrue(isFinite(add(fromBits(kPosInf), 1.0f)), "Inf + 1 is finite");
            t.IsTrue(same(mul(fromBits(kPosInf), 0.0f), 0u), "Inf * 0 = 0 (no NaN)");
            t.IsTrue(same(add(fromBits(kPosInf), fromBits(kNegInf)), 0u), "Inf + -Inf = 0 (both are +-Fmax and cancel, no NaN)");
            t.Equals(add(1.5f, 2.25f), 3.75f, "ordinary addition");
            t.Equals(mul(-3.0f, 4.0f), -12.0f, "ordinary multiplication");
        });

        tc.Run("MADD/MSUB keep the product exact and saturate once", [](TestCase &t) {
            t.Equals(madd(1.0f, 2.0f, 3.0f), 7.0f, "1 + 2*3");
            t.Equals(msub(10.0f, 2.0f, 3.0f), 4.0f, "10 - 2*3");
            t.IsTrue(same(madd(kFmax, kFmax, 2.0f), 0x7F7FFFFFu), "acc + overflow stays Fmax");
            t.IsTrue(same(msub(0.0f, kFmax, 4.0f), 0xFF7FFFFFu), "0 - Fmax*4 = -Fmax");
            // fs*ft = 1e30 * 1e30 would overflow a float intermediate; with an exact product the final acc - product is still saturated, not NaN
            t.IsTrue(isFinite(madd(-1.0e30f, 1.0e30f, 1.0e30f)), "never non-finite");
        });

        tc.Run("CVT.W.S truncates toward zero and saturates", [](TestCase &t) {
            t.Equals(cvtW(1.9f), 1, "1.9 -> 1");
            t.Equals(cvtW(-1.9f), -1, "-1.9 -> -1 (not -2)");
            t.Equals(cvtW(0.99f), 0, "0.99 -> 0");
            t.Equals(cvtW(2.5f), 2, "2.5 -> 2 (not round-to-even 2 / nearest 3)");
            t.Equals(cvtW(3.5f), 3, "3.5 -> 3 (not nearest 4)");
            t.Equals(cvtW(3.0e9f), INT32_MAX, "positive overflow saturates to 0x7FFFFFFF");
            t.Equals(cvtW(-3.0e9f), INT32_MIN, "negative overflow saturates to 0x80000000");
            t.Equals(cvtW(fromBits(kPosInf)), INT32_MAX, "+Inf pattern -> 0x7FFFFFFF");
            t.Equals(cvtW(fromBits(kNegInf)), INT32_MIN, "-Inf pattern -> 0x80000000");
            t.Equals(cvtW(fromBits(kQNaN)), INT32_MAX, "NaN pattern with sign 0 -> 0x7FFFFFFF");
            t.Equals(cvtW(fromBits(kNegQNaN)), INT32_MIN, "NaN pattern with sign 1 -> 0x80000000");
            t.Equals(cvtW(2147483520.0f), 2147483520, "largest float below 2^31 converts exactly");
            t.Equals(cvtW(fromBits(kNegDenormal)), 0, "denormal -> 0");
        });

        tc.Run("comparisons use the converted operands; there is no unordered result", [](TestCase &t) {
            t.IsTrue(lt(1.0f, 2.0f) && !lt(2.0f, 1.0f) && !lt(1.0f, 1.0f), "c.lt");
            t.IsTrue(le(1.0f, 1.0f) && le(1.0f, 2.0f) && !le(2.0f, 1.0f), "c.le");
            t.IsTrue(eq(0.0f, fromBits(kSignBit)), "+0 == -0");
            t.IsTrue(eq(fromBits(kDenormal), 0.0f), "denormal == 0");
            t.IsTrue(lt(kFmax, fromBits(kPosInf)) == false && eq(kFmax, fromBits(kPosInf)), "+Inf pattern compares equal to Fmax");
            t.IsTrue(lt(fromBits(kNegInf), -1.0f), "-Inf pattern is a very negative number");
            t.IsTrue(lt(fromBits(kNegQNaN), 0.0f), "a negative NaN pattern is a negative number: c.lt is true, not false");
            t.IsTrue(!eq(fromBits(kQNaN), fromBits(kQNaN)) == false, "a NaN pattern equals itself");
        });

        tc.Run("MAX.S / MIN.S / ABS.S / NEG.S", [](TestCase &t) {
            t.Equals(maxS(1.0f, 2.0f), 2.0f, "max");
            t.Equals(minS(1.0f, 2.0f), 1.0f, "min");
            t.IsTrue(isFinite(maxS(fromBits(kQNaN), 1.0f)), "max with a NaN pattern is finite");
            t.IsTrue(same(absS(fromBits(kNegQNaN)), kQNaN), "abs clears only the sign bit");
            t.IsTrue(same(negS(0.0f), kSignBit), "neg(+0) = -0");
            t.Equals(negS(2.5f), -2.5f, "neg");
        });

        tc.Run("macros used by generated code behave the same and update FCR31", [](TestCase &t) {
            R5900Context ctxObject{};
            R5900Context *ctx = &ctxObject; // the macros refer to `ctx->`, as in generated functions
            ctx->f[0] = -22.0f;
            ctx->f[1] = 484.0f;
            ctx->f[2] = 0.0f;
            // sqrt.s $f1,$f1 (ft = f1): the radicand is f1, not f0
            ctx->f[3] = FPU_SQRT_S(ctx->f[1]);
            t.Equals(ctx->f[3], 22.0f, "FPU_SQRT_S(ft)");
            t.Equals(ctx->fcr31 & kFlagI, 0u, "a valid operand sets no flag");
            ctx->f[4] = FPU_SQRT_S(ctx->f[0]);
            t.Equals(ctx->f[4], static_cast<float>(std::sqrt(22.0)), "FPU_SQRT_S of a negative operand uses |x|");
            t.IsTrue((ctx->fcr31 & kFlagI) != 0u, "...and sets the invalid flag in ctx->fcr31");
            ctx->fcr31 = 0;
            ctx->f[5] = FPU_DIV_S(ctx->f[1], ctx->f[2]);
            t.IsTrue(same(ctx->f[5], 0x7F7FFFFFu) && (ctx->fcr31 & kFlagD), "FPU_DIV_S by zero");
            ctx->f[6] = FPU_RSQRT_S(ctx->f[1], ctx->f[1]);
            t.Equals(ctx->f[6], 22.0f, "FPU_RSQRT_S(fs, ft) = 484 / sqrt(484)");
            FPU_SET_ACC(ctx, 1.0f);
            ctx->f[7] = FPU_MADD_S(ctx->f_acc, 2.0f, 3.0f);
            t.Equals(ctx->f[7], 7.0f, "FPU_MADD_S");
            t.Equals(FPU_CVT_W_S(-7.9f), -7, "FPU_CVT_W_S truncates");
            t.IsTrue(FPU_C_LT_S(-22.0f, 0.0f) && !FPU_C_UN_S(1.0f, 2.0f), "compare macros");
        });
    });
}
