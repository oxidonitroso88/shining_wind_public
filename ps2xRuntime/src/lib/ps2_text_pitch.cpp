// See include/ps2_text_pitch.h.
#include "ps2_text_pitch.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

namespace
{
    constexpr uint32_t kGlyphQuad = 0x30BDF0u;
    constexpr uint32_t kCharWidget = 0x1CAC30u;
    constexpr uint32_t kCharAdvance = 0x30AF90u;
    PS2Runtime::RecompiledFunction g_originalGlyphQuad = nullptr;
    PS2Runtime::RecompiledFunction g_originalCharWidget = nullptr;
    PS2Runtime::RecompiledFunction g_originalCharAdvance = nullptr;

    // a0 = output quad, a1 = ?, a2 = FNT glyph record {u16 code, u16 offset, u16 0, u8 crop_left, u8 crop_width, u16 0}, a3 = font,
    // t0 = proportional flag (the byte 0x17 of the text style).
    const bool g_diag = std::getenv("PS2X_TEXT_DIAG") != nullptr;
    const bool g_trace = std::getenv("PS2X_TEXT_TRACE") != nullptr; // every glyph (limited by PS2X_TEXT_TRACE=<count>, default 20000)
    std::atomic<long> g_traceBudget{[]() { const char *v = std::getenv("PS2X_TEXT_TRACE"); const long n = v ? std::atol(v) : 0; return n > 1 ? n : 20000L; }()};

    // PS2X_TEXT_DIAG=1: print once every distinct (caller, font, style flag, glyph class) seen by the glyph quad builder.
    void diagnose(uint8_t *rdram, R5900Context *ctx)
    {
        static std::mutex mutex;
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, int>> seen;
        const uint32_t record = getRegU32(ctx, 6);
        if (record < 0x100000u || record + 8u > PS2_RAM_SIZE)
            return;
        uint16_t code = 0;
        std::memcpy(&code, rdram + record, sizeof(code));
        const int cls = code >= 0x20u && code <= 0x7Eu ? 0 : 1;
        const auto key = std::make_tuple(getRegU32(ctx, 31), getRegU32(ctx, 7), getRegU32(ctx, 8), cls);
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.insert(key).second)
            std::cerr << "[text] glyph ra=0x" << std::hex << std::get<0>(key) << " font=0x" << std::get<1>(key) << " flag=" << std::dec << std::get<2>(key)
                      << " class=" << (cls == 0 ? "ascii" : "other") << " code=0x" << std::hex << code << " crop_left=" << std::dec << int(rdram[record + 6])
                      << " crop_width=" << int(rdram[record + 7]) << " cell_w=" << (getRegU32(ctx, 7) < PS2_RAM_SIZE - 0x20 ? *reinterpret_cast<uint32_t *>(rdram + getRegU32(ctx, 7) + 0x10) : 0u)
                      << std::endl;
    }

    // Glyphs that always use their own width: ASCII, and the 17 full-width small latin codes 0x8281-0x8291 that the Spanish font variants (tools/fonts) reuse for
    // the accented letters, n-tilde, u-diaeresis and the inverted marks. With the original font those cells are 22 wide, so the rule changes nothing for them.
    bool proportionalCode(uint32_t code)
    {
        return (code >= 0x20u && code <= 0x7Eu) || (code >= 0x8281u && code <= 0x8291u);
    }

    // 0x1CAC30 builds one text character as its own widget (stat lines of the shops, button bars...): a0 = text object, a1 = character code,
    // a2 = next character (kerning), a3 = proportional flag. With a3 == 0 the pen advances by the font's fixed pitch (field 0xC of the font
    // slot: 18/22) regardless of the glyph, with a3 != 0 by the glyph's crop width. Same rule as the glyph quad: ASCII is always proportional.
    void charWidgetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t code = getRegU32(ctx, 5);
        if (getRegU32(ctx, 7) == 0u && proportionalCode(code))
            SET_GPR_U32(ctx, 7, 1u);
        ctx->pc = kCharWidget;
        g_originalCharWidget(rdram, ctx, runtime);
    }

    // 0x30AF90 = advance of one character, used to measure strings (centering, wrapping): a0 = text object, a1 = font slot, a2 = character,
    // a3 = next character (kerning), t0 = proportional flag. With t0 == 0 it returns the font's fixed cell width instead of the glyph's crop
    // width: centred or right-aligned ASCII text was placed with the wide measure and drawn narrow. Same rule as the other hooks.
    void charAdvanceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t code = getRegU32(ctx, 6);
        if (getRegU32(ctx, 8) == 0u && proportionalCode(code))
            SET_GPR_U32(ctx, 8, 1u);
        ctx->pc = kCharAdvance;
        g_originalCharAdvance(rdram, ctx, runtime);
    }

    void glyphQuadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_diag)
            diagnose(rdram, ctx);
        const uint32_t record = getRegU32(ctx, 6);
        if (getRegU32(ctx, 8) == 0u && record >= 0x100000u && record + 8u <= PS2_RAM_SIZE)
        {
            uint16_t code = 0;
            std::memcpy(&code, rdram + record, sizeof(code));
            if (proportionalCode(code))
                SET_GPR_U32(ctx, 8, 1u);
        }
        const uint32_t flagBefore = getRegU32(ctx, 8), fontPtr = getRegU32(ctx, 7), callerRa = getRegU32(ctx, 31);
        const float penX = ctx->f[12], scale = ctx->f[16];
        ctx->pc = kGlyphQuad;
        g_originalGlyphQuad(rdram, ctx, runtime);
        if (g_trace && record >= 0x100000u && record + 8u <= PS2_RAM_SIZE && g_traceBudget.fetch_sub(1) > 0)
        {
            uint16_t code = 0;
            std::memcpy(&code, rdram + record, sizeof(code));
            std::cerr << "[textg] ra=0x" << std::hex << callerRa << " font=0x" << fontPtr << " code=0x" << code << std::dec << " flag=" << flagBefore
                      << " crop=" << int(rdram[record + 6]) << "/" << int(rdram[record + 7]) << " x=" << penX << " scale=" << scale << " adv=" << ctx->f[0] << std::endl;
        }
    }
}

namespace ps2_text_pitch
{
    void installFromEnvironment(PS2Runtime &runtime)
    {
        if (const char *mode = std::getenv("PS2X_ASCII_PITCH"); mode != nullptr && std::string(mode) == "fixed")
            return;
        g_originalGlyphQuad = runtime.hasFunction(kGlyphQuad) ? runtime.lookupFunction(kGlyphQuad) : nullptr;
        if (g_originalGlyphQuad == nullptr)
        {
            std::cerr << "[text] 0x30BDF0 is not registered: ASCII pitch hook not installed" << std::endl;
            return;
        }
        if (runtime.replaceFunction(kGlyphQuad, &glyphQuadHook))
            std::cout << "[text] ASCII glyphs use their own width in every window (PS2X_ASCII_PITCH=fixed disables)" << std::endl;
        g_originalCharWidget = runtime.hasFunction(kCharWidget) ? runtime.lookupFunction(kCharWidget) : nullptr;
        if (g_originalCharWidget != nullptr && runtime.replaceFunction(kCharWidget, &charWidgetHook))
            std::cout << "[text] per-character widgets (0x1CAC30) too" << std::endl;
        g_originalCharAdvance = runtime.hasFunction(kCharAdvance) ? runtime.lookupFunction(kCharAdvance) : nullptr;
        if (g_originalCharAdvance != nullptr && runtime.replaceFunction(kCharAdvance, &charAdvanceHook))
            std::cout << "[text] string measure (0x30AF90) too" << std::endl;
    }
}
