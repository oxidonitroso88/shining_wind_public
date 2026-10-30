#pragma once

// Emotion Engine FPU (COP1, single precision) arithmetic semantics.
//
// The R5900 FPU is NOT IEEE 754: it has no NaN and no infinities. Operands whose exponent field is 255 are ordinary (huge) numbers, results that
// overflow saturate to +-Fmax (0x7F7FFFFF), denormal operands and results are flushed to +-0, division by zero returns +-Fmax, SQRT/RSQRT take the
// absolute value of a negative operand (setting the invalid flag) and CVT.W.S truncates and saturates. Plain C++ float arithmetic differs in all of
// these cases, which makes collision and geometry code that relies on them take different branches (e.g. sqrt of a negative -> NaN -> comparisons
// that are always false). References: the R5900 manual and the PCSX2 FPU implementation (fpuDouble / checkOverflow / DIV_S / SQRT_S / RSQRT_S / CVT_W).
// Rounding of arithmetic results is round-to-nearest here (the real EE truncates); this matches what the reference emulator does by default.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ps2fpu
{
    constexpr uint32_t kFmaxBits = 0x7F7FFFFFu;
    constexpr uint32_t kSignBit = 0x80000000u;
    // FCR31 flag bits (EE layout)
    constexpr uint32_t kFlagC = 0x00800000u;
    constexpr uint32_t kFlagI = 0x00020000u;
    constexpr uint32_t kFlagD = 0x00010000u;
    constexpr uint32_t kFlagO = 0x00008000u;
    constexpr uint32_t kFlagU = 0x00004000u;
    constexpr uint32_t kFlagSI = 0x00000040u;
    constexpr uint32_t kFlagSD = 0x00000020u;
    constexpr uint32_t kFlagSO = 0x00000010u;
    constexpr uint32_t kFlagSU = 0x00000008u;

    inline uint32_t bitsOf(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    inline float fromBits(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

    // Operand conversion: denormal -> +-0, exponent 255 (IEEE Inf/NaN pattern) -> +-Fmax.
    inline float in(float f)
    {
        uint32_t u = bitsOf(f);
        const uint32_t e = u & 0x7F800000u;
        if (e == 0u)
            u &= kSignBit;
        else if (e == 0x7F800000u)
            u = (u & kSignBit) | kFmaxBits;
        return fromBits(u);
    }

    // Result conversion from a wider intermediate: overflow -> +-Fmax, denormal/underflow -> +-0.
    inline float out(double d)
    {
        uint32_t u = bitsOf(static_cast<float>(d));
        const uint32_t e = u & 0x7F800000u;
        if (e == 0x7F800000u)
            u = (u & kSignBit) | kFmaxBits;
        else if (e == 0u)
            u &= kSignBit;
        return fromBits(u);
    }

    inline float add(float a, float b) { return out(static_cast<double>(in(a)) + static_cast<double>(in(b))); }
    inline float sub(float a, float b) { return out(static_cast<double>(in(a)) - static_cast<double>(in(b))); }
    inline float mul(float a, float b) { return out(static_cast<double>(in(a)) * static_cast<double>(in(b))); }
    // MADD/MSUB: acc +- fs*ft with the product kept exact (double) before the single result conversion.
    inline float madd(float acc, float a, float b) { return out(static_cast<double>(in(acc)) + static_cast<double>(in(a)) * static_cast<double>(in(b))); }
    inline float msub(float acc, float a, float b) { return out(static_cast<double>(in(acc)) - static_cast<double>(in(a)) * static_cast<double>(in(b))); }

    inline bool isZero(float f) { return (bitsOf(f) & 0x7FFFFFFFu) == 0u; }

    // DIV.S fd, fs, ft
    inline float div(float fs, float ft, uint32_t &fcr31)
    {
        const float a = in(fs), b = in(ft);
        if (isZero(b))
        {
            fcr31 |= isZero(a) ? (kFlagI | kFlagSI) : (kFlagD | kFlagSD);
            return fromBits(((bitsOf(a) ^ bitsOf(b)) & kSignBit) | kFmaxBits);
        }
        return out(static_cast<double>(a) / static_cast<double>(b));
    }

    // SQRT.S fd, ft  (the radicand is ft; a negative operand uses its absolute value and sets the invalid flag)
    inline float sqrt(float ft, uint32_t &fcr31)
    {
        const float b = in(ft);
        if (bitsOf(b) & kSignBit)
            fcr31 |= kFlagI | kFlagSI;
        return out(std::sqrt(std::fabs(static_cast<double>(b))));
    }

    // RSQRT.S fd, fs, ft : fd = fs / sqrt(ft)
    inline float rsqrt(float fs, float ft, uint32_t &fcr31)
    {
        const float a = in(fs), b = in(ft);
        if (isZero(b))
        {
            fcr31 |= isZero(a) ? (kFlagI | kFlagSI) : (kFlagD | kFlagSD);
            return fromBits(((bitsOf(a) ^ bitsOf(b)) & kSignBit) | kFmaxBits);
        }
        if (bitsOf(b) & kSignBit)
            fcr31 |= kFlagI | kFlagSI;
        return out(static_cast<double>(a) / std::sqrt(std::fabs(static_cast<double>(b))));
    }

    // CVT.W.S: truncation toward zero; values that do not fit saturate (positive -> 0x7FFFFFFF, negative -> 0x80000000).
    inline int32_t cvtW(float f)
    {
        const uint32_t u = bitsOf(f);
        if ((u & 0x7F800000u) <= 0x4E800000u)
            return static_cast<int32_t>(f);
        return (u & kSignBit) ? INT32_MIN : INT32_MAX;
    }

    inline float maxS(float a, float b)
    {
        const float x = in(a), y = in(b);
        return x > y ? x : y;
    }
    inline float minS(float a, float b)
    {
        const float x = in(a), y = in(b);
        return x < y ? x : y;
    }
    inline float absS(float a) { return fromBits(bitsOf(a) & 0x7FFFFFFFu); }
    inline float negS(float a) { return fromBits(bitsOf(a) ^ kSignBit); }

    inline bool lt(float a, float b) { return in(a) < in(b); }
    inline bool le(float a, float b) { return in(a) <= in(b); }
    inline bool eq(float a, float b) { return in(a) == in(b); }
}
