#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <smmintrin.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    __forceinline TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    bool isFourBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T4 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
    }

    bool isEightBitIndexedPsm(uint8_t psm)
    {
        return psm == GS_PSM_T8 || psm == GS_PSM_T8H;
    }

    __forceinline uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        // round half away from zero for x >= 0 equals trunc(x + 0.5); negatives are clamped to 0 either way
        return clampU8(static_cast<int>(top + (bottom - top) * fy + 0.5f));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode2)
    {
        return {(smode2 & 0x1ull) != 0ull, ((smode2 >> 1) & 0x1ull) != 0ull};
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
            return;
        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
                sourceY = height - 1u;
            std::memcpy(pixels.data() + y * kHostFrameWidth * 4u,
                        source.data() + sourceY * kHostFrameWidth * 4u,
                        width * 4u);
        }
    }

    // FIELD mode (SMODE2.INT=1, FFMD=0): the display area height (DH+1) covers the whole interlaced frame, while the
    // frame buffer holds one field of half that height (e.g. two 640x224 buffers at FBP 0 and 70 for a 640x448 area).
    // Only the field is read; its lines are doubled to fill the frame.
    uint32_t fieldReadHeight(bool fieldMode, uint32_t displayHeight)
    {
        return (fieldMode && displayHeight > 300u) ? displayHeight / 2u : displayHeight;
    }

    void expandFieldLines(std::vector<uint8_t> &pixels, uint32_t width, uint32_t readHeight, uint32_t displayHeight)
    {
        if (readHeight >= displayHeight || pixels.empty() || width == 0u)
            return;
        for (uint32_t y = displayHeight; y-- > 0u;)
        {
            const uint32_t sourceY = std::min(y / 2u, readHeight - 1u);
            if (sourceY == y)
                continue;
            std::memmove(pixels.data() + y * kHostFrameWidth * 4u, pixels.data() + sourceY * kHostFrameWidth * 4u, width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    m_ctxs.resize(1);
    m_ctxs[0].cache = &m_texturePageCache;
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    if (vram && vramSize < GSMem::MEMORY_SIZE)
        throw std::invalid_argument("GS CPU backend requires at least 4 MiB of VRAM");

    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    ++m_clutVersion;
    m_texLutKey.valid = false;
    if (!m_ctxs.empty()) m_ctxs[0].quadValid = false;
    m_clut.fill(0u);
    m_clutCbp.fill(0u);
    m_texturePageCache.Invalidate();
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
        return;
    DrawPrimitive(batch);
}

void GSCpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || (!isFourBitIndexedPsm(tex0.psm) && !isEightBitIndexedPsm(tex0.psm)))
        return;

    switch (tex0.cld)
    {
    case 0u:
    case 6u:
    case 7u:
        return;
    case 1u:
        break;
    case 2u:
        m_clutCbp[0] = tex0.cbp;
        break;
    case 3u:
        m_clutCbp[1] = tex0.cbp;
        break;
    case 4u:
        if (m_clutCbp[0] == tex0.cbp)
            return;
        m_clutCbp[0] = tex0.cbp;
        break;
    case 5u:
        if (m_clutCbp[1] == tex0.cbp)
            return;
        m_clutCbp[1] = tex0.cbp;
        break;
    default:
        return;
    }

    LoadClutUnlocked(tex0, texclut);
}

void GSCpuBackend::LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    ++m_clutVersion;
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return;

    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;

    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;

    for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
    {
        uint32_t sourceX = 0u;
        uint32_t sourceY = 0u;
        uint32_t sourceWidth = 1u;

        if (tex0.csm == 0u)
        {
            const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
            sourceX = sourceIndex & 0x0Fu;
            sourceY = sourceIndex >> 4u;
        }
        else
        {
            sourceWidth = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
            sourceX = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
            sourceY = static_cast<uint32_t>(texclut.cov);
        }

        const uint32_t raw = ReadTextureVramUnlocked(tex0.cpsm, tex0.cbp, sourceWidth, sourceX, sourceY); 
        const uint32_t destination = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
        if (sixteenBit)
        {
            m_clut[destination] = static_cast<uint16_t>(raw);
        }
        else
        {
            m_clut[destination] = static_cast<uint16_t>(raw & 0xFFFFu);
            m_clut[destination + 256u] = static_cast<uint16_t>(raw >> 16u);
        }
    }
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_ctxs.empty()) m_ctxs[0].quadValid = false;
    m_texturePageCache.Invalidate();
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadTextureVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y)
{
    if (!m_vram)
        return 0u;

    return GSMem::ReadTexture(m_texturePageCache, m_vram, psm, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

std::atomic<unsigned long long> g_ps2xGsVblank{0}; // guest VBlank count, set by EeScheduler (diagnostics: PS2X_GS_VLOG)

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    {
        // PS2X_GS_VLOG="<firstVBlank>-<lastVBlank>@<file>" ('@' because play.ps1 -ExtraEnv splits on ';'): append one line per primitive (full texture/sampling state and vertices) while the guest VBlank
        // counter is in that range. Use with PS2X_GS_SYNC=1 so the primitives are drawn inside the VBlank they were submitted in (diagnostic only).
        static FILE *vlog = []() -> FILE * {
            const char *s = std::getenv("PS2X_GS_VLOG");
            const char *semi = s ? std::strchr(s, '@') : nullptr;
            return semi ? std::fopen(semi + 1, "w") : nullptr;
        }();
        if (vlog)
        {
            static const unsigned long long lo = std::strtoull(std::getenv("PS2X_GS_VLOG"), nullptr, 10);
            static const unsigned long long hi = []() { const char *d = std::strchr(std::getenv("PS2X_GS_VLOG"), '-'); return d ? std::strtoull(d + 1, nullptr, 10) : 0ull; }();
            const unsigned long long vb = g_ps2xGsVblank.load(std::memory_order_relaxed);
            if (vb >= lo && vb <= hi)
            {
                std::fprintf(vlog, "vb=%llu type=%u iip=%d tme=%d fge=%d abe=%d fst=%d ctxt=%d fbp=%u fbw=%u fpsm=0x%x fbmsk=0x%x tbp0=0x%x tbw=%u tpsm=0x%x tw=%u th=%u tcc=%u tfx=%u cbp=0x%x cpsm=0x%x csm=%u csa=%u tex1=0x%llx clamp=0x%llx miptbp1=0x%llx alpha=0x%llx test=0x%llx texW=%u texH=%u lin=%d n=%u", vb,\
                             static_cast<unsigned>(state.prim.type), state.prim.iip, state.prim.tme, state.prim.fge, state.prim.abe, state.prim.fst, state.prim.ctxt, ctx.frame.fbp, ctx.frame.fbw,\
                             static_cast<unsigned>(ctx.frame.psm), ctx.frame.fbmsk, ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), static_cast<unsigned>(ctx.tex0.psm), static_cast<unsigned>(ctx.tex0.tw),\
                             static_cast<unsigned>(ctx.tex0.th), static_cast<unsigned>(ctx.tex0.tcc), static_cast<unsigned>(ctx.tex0.tfx), ctx.tex0.cbp, static_cast<unsigned>(ctx.tex0.cpsm),\
                             static_cast<unsigned>(ctx.tex0.csm), static_cast<unsigned>(ctx.tex0.csa), static_cast<unsigned long long>(ctx.tex1), static_cast<unsigned long long>(ctx.clamp),\
                             static_cast<unsigned long long>(ctx.miptbp1), static_cast<unsigned long long>(ctx.alpha), static_cast<unsigned long long>(ctx.test), state.textureWidth, state.textureHeight,\
                             state.linearFilter, static_cast<unsigned>(batch.vertexCount));
                for (unsigned i = 0; i < batch.vertexCount && i < 3u; ++i)
                {
                    const GSVertex &v = batch.vertices[i];
                    std::fprintf(vlog, " | x=%.2f y=%.2f z=%.0f rgba=%u,%u,%u,%u u=%.3f v=%.3f s=%g t=%g q=%g", v.x, v.y, v.z, v.r, v.g, v.b, v.a, v.u / 16.0, v.v / 16.0, v.s, v.t, v.q);
                }
                std::fputc('\n', vlog);
                std::fflush(vlog);
            }
        }
    }
    {
        // PS2X_GS_PRIM_LOG="<seconds>;<count>": after <seconds> of wall time, log the next <count> primitives (diagnostic only).
        static const char *spec = std::getenv("PS2X_GS_PRIM_LOG");
        if (spec)
        {
            static const auto started = std::chrono::steady_clock::now();
            static const double after = std::atof(spec);
            static const char *semi = std::strchr(spec, ';');
            static const long limit = semi ? std::atol(semi + 1) : 200;
            static std::atomic<long> logged{0};
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= after &&
                logged.load(std::memory_order_relaxed) < limit)
            {
                const long n = logged.fetch_add(1, std::memory_order_relaxed);
                std::cout << "[gs:primlog] t=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " n=" << n << " type=" << static_cast<uint32_t>(state.prim.type)
                          << " tme=" << static_cast<uint32_t>(state.prim.tme) << " abe=" << static_cast<uint32_t>(state.prim.abe)
                          << " fst=" << static_cast<uint32_t>(state.prim.fst) << " fbp=" << ctx.frame.fbp
                          << " fpsm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm)
                          << " tbp0=0x" << ctx.tex0.tbp0 << " tpsm=0x" << static_cast<uint32_t>(ctx.tex0.psm)
                          << " cbp=0x" << ctx.tex0.cbp << " cpsm=0x" << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                          << " tw=" << static_cast<uint32_t>(ctx.tex0.tw) << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                          << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc) << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                          << " alpha=0x" << std::hex << ctx.alpha << " test=0x" << ctx.test << std::dec
                          << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                          << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                          << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                          << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                          << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << "," << static_cast<uint32_t>(batch.vertices[0].g)
                          << "," << static_cast<uint32_t>(batch.vertices[0].b) << "," << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                          << std::endl;
            }
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    SetupPixelPipeline(state);
    SetupTexture(state);
    {
        // PS2X_GS_TEXDUMP="<firstVBlank>-<lastVBlank>@<prefix>": write every distinct texture (tbp0/psm/size/CLUT) used by a textured primitive in that VBlank
        // range as <prefix>_<tbp0>_<psm>.ppm, decoded exactly as the rasteriser samples it (point sampling). Diagnostic only.
        static const char *spec = std::getenv("PS2X_GS_TEXDUMP");
        if (spec && state.prim.tme)
        {
            static const unsigned long long lo = std::strtoull(spec, nullptr, 10);
            static const unsigned long long hi = []() { const char *d = std::strchr(std::getenv("PS2X_GS_TEXDUMP"), '-'); return d ? std::strtoull(d + 1, nullptr, 10) : 0ull; }();
            const unsigned long long vb = g_ps2xGsVblank.load(std::memory_order_relaxed);
            const auto &t0 = state.context.tex0;
            static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> seen;
            const auto key = std::make_tuple(t0.tbp0, static_cast<uint32_t>(t0.psm), static_cast<uint32_t>(t0.tw), static_cast<uint32_t>(t0.th), t0.cbp);
            if (vb >= lo && vb <= hi && seen.insert(key).second)
            {
                const char *at = std::strchr(spec, '@');
                char path[512];
                std::snprintf(path, sizeof(path), "%s_%x_%x_c%x.ppm", at ? at + 1 : "texdump", t0.tbp0, static_cast<unsigned>(t0.psm), t0.cbp);
                const int w = 1 << t0.tw, h = 1 << t0.th;
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fprintf(f, "P6 %d %d 255 ", w, h);
                    RasterCtx rc;
                    rc.cache = &m_texturePageCache;
                    GSDrawState st2 = state;
                    st2.prim.fst = true;
                    st2.linearFilter = false;
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < w; ++x)
                        {
                            rc.quadValid = false;
                            const uint32_t c = SampleTexture(rc, st2, 0.0f, 0.0f, 1.0f, static_cast<uint16_t>(x * 16), static_cast<uint16_t>(y * 16));
                            const unsigned char px[3] = {static_cast<unsigned char>(c & 0xFF), static_cast<unsigned char>((c >> 8) & 0xFF), static_cast<unsigned char>((c >> 16) & 0xFF)};
                            std::fwrite(px, 1, 3, f);
                        }
                    std::fclose(f);
                    // alpha channel as a PGM next to it
                    char apath[540];
                    std::snprintf(apath, sizeof(apath), "%s.alpha.pgm", path);
                    if (FILE *fa = std::fopen(apath, "wb"))
                    {
                        std::fprintf(fa, "P5 %d %d 255 ", w, h);
                        for (int y = 0; y < h; ++y)
                            for (int x = 0; x < w; ++x)
                            {
                                rc.quadValid = false;
                                const uint32_t c = SampleTexture(rc, st2, 0.0f, 0.0f, 1.0f, static_cast<uint16_t>(x * 16), static_cast<uint16_t>(y * 16));
                                const unsigned char px = static_cast<unsigned char>((c >> 24) & 0xFF);
                                std::fwrite(&px, 1, 1, fa);
                            }
                        std::fclose(fa);
                    }
                }
            }
        }
    }
    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

bool GSCpuBackend::EnsureRasterPool()
{
    if (m_poolChecked)
        return m_pool != nullptr;
    m_poolChecked = true;
    unsigned helpers = 0;
    if (const char *env = std::getenv("PS2X_GS_THREADS"))
    {
        helpers = static_cast<unsigned>(std::max(0, std::atoi(env)));
    }
    else
    {
        const unsigned hw = std::thread::hardware_concurrency();
        helpers = hw >= 8 ? std::min(5u, hw / 2u - 1u) : (hw >= 4 ? 2u : 0u);
    }
    if (helpers == 0)
        return false;
    m_pool = std::make_unique<GSRasterPool>(helpers);
    for (unsigned i = 0; i < helpers; ++i)
    {
        m_helperCaches.push_back(std::make_unique<GSMem::TexturePageCache>());
        RasterCtx c;
        c.cache = m_helperCaches.back().get();
        m_ctxs.push_back(c);
    }
    return true;
}

// Draws rows [yBegin, yEndInclusive] through fn(RasterCtx&, rowBegin, rowEnd). Large primitives are split into bands that are
// drawn concurrently; every pixel still belongs to exactly one band, so the output equals the sequential one.
template <typename Fn>
void GSCpuBackend::RunRows(int yBegin, int yEndInclusive, uint64_t pixels, Fn &&fn)
{
    if (yEndInclusive < yBegin)
        return;
    constexpr uint64_t kParallelPixelThreshold = 16384u;
    if (pixels < kParallelPixelThreshold || (yEndInclusive - yBegin + 1) < 8 || !EnsureRasterPool())
    {
        fn(m_ctxs[0], yBegin, yEndInclusive + 1);
        return;
    }
    for (size_t i = 1; i < m_ctxs.size(); ++i)
    {
        m_ctxs[i].cache->Invalidate();
        m_ctxs[i].quadValid = false;
    }
    struct Job
    {
        GSCpuBackend *self;
        Fn *fn;
    } job{this, &fn};
    const int workers = static_cast<int>(m_pool->helpers()) + 1;
    const int rows = yEndInclusive - yBegin + 1;
    const int bandRows = std::max(4, rows / (workers * 4));
    m_pool->Run(yBegin, yEndInclusive + 1, bandRows,
                [](void *user, unsigned context, int y0, int y1)
                {
                    Job *j = static_cast<Job *>(user);
                    (*j->fn)(j->self->m_ctxs[context], y0, y1);
                },
                &job);
}

void GSCpuBackend::SetupPixelPipeline(const GSDrawState &state)
{
    const auto &ctx = state.context;
    PixelSetup &p = m_px;
    p.scX0 = static_cast<int>(ctx.scissor.x0);
    p.scY0 = static_cast<int>(ctx.scissor.y0);
    p.scX1 = static_cast<int>(ctx.scissor.x1);
    p.scY1 = static_cast<int>(ctx.scissor.y1);
    p.fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    p.fbw = std::max<u32>(ctx.frame.fbw, 1u);
    p.fpsm = ctx.frame.psm;
    p.zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    p.zpsm = ctx.zbuf.psm;
    p.fb16 = bitsPerPixel(static_cast<uint8_t>(p.fpsm)) == 16;
    p.fb24 = p.fpsm == GS_PSM_CT24;
    p.ztest = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    p.zwrite = !ctx.zbuf.zmask;
    p.abe = state.prim.abe;
    p.pabe = state.pabe;
    p.fba = (ctx.fba & 0x1ull) != 0ull;
    const uint64_t alphaReg = ctx.alpha;
    p.asel = static_cast<uint8_t>(alphaReg & 3);
    p.bsel = static_cast<uint8_t>((alphaReg >> 2) & 3);
    p.csel = static_cast<uint8_t>((alphaReg >> 4) & 3);
    p.dsel = static_cast<uint8_t>((alphaReg >> 6) & 3);
    p.fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);
    p.stdBlend = p.asel == 0 && p.bsel == 1 && p.csel == 0 && p.dsel == 1;
    p.readFb = m_readVramFuncs[p.fpsm & 0x3Fu];
    p.writeFb = m_writeVramFuncs[p.fpsm & 0x3Fu];
    p.readZ = m_readVramFuncs[p.zpsm & 0x3Fu];
    p.writeZ = m_writeVramFuncs[p.zpsm & 0x3Fu];

    // The alpha test never rejects a pixel when it is disabled, ALWAYS, or an inequality every 8-bit alpha satisfies.
    p.atst = static_cast<uint8_t>((ctx.test >> 1) & 0x7u);
    p.aref = static_cast<uint8_t>((ctx.test >> 4) & 0xFFu);
    p.afail = static_cast<uint8_t>((ctx.test >> 12) & 0x3u);
    p.ate = (ctx.test & 0x1u) != 0u &&
            !(p.atst == 1 || (p.atst == 5 && p.aref == 0) || (p.atst == 3 && p.aref == 255));
    const bool date = ((ctx.test >> 14) & 0x1u) != 0u;
    p.fast = !state.prim.fge && ctx.frame.fbmsk == 0 && !date && m_vram != nullptr;
}

void GSCpuBackend::WritePixelFast(int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const PixelSetup &p = m_px;
    if (x < p.scX0 || x > p.scX1 || y < p.scY0 || y > p.scY1)
        return;

    // TEST.AFAIL decides what a pixel that fails the alpha test still updates (same rules as classifyAlphaTest).
    bool writeRgb = true, writeAlpha = true, writeDepth = true;
    if (p.ate)
    {
        bool pass;
        switch (p.atst)
        {
        case 0: pass = false; break;
        case 2: pass = a < p.aref; break;
        case 3: pass = a <= p.aref; break;
        case 4: pass = a == p.aref; break;
        case 5: pass = a >= p.aref; break;
        case 6: pass = a > p.aref; break;
        case 7: pass = a != p.aref; break;
        default: pass = true; break;
        }
        if (!pass)
        {
            switch (p.afail)
            {
            case 1: // FB_ONLY
                writeDepth = false;
                break;
            case 2: // ZB_ONLY
                writeRgb = false;
                writeAlpha = false;
                break;
            case 3: // RGB_ONLY (distinct only for RGBA32)
                if (p.fpsm == GS_PSM_CT32)
                {
                    writeAlpha = false;
                    writeDepth = false;
                }
                else
                {
                    writeDepth = false;
                }
                break;
            default: // KEEP
                return;
            }
        }
    }

    switch (p.ztest)
    {
    case 0:
        return;
    case 1:
        break;
    case 2:
        if (static_cast<uint32_t>(z) < p.readZ(m_vram, p.zbp, p.fbw, x, y))
            return;
        break;
    default:
        if (static_cast<uint32_t>(z) <= p.readZ(m_vram, p.zbp, p.fbw, x, y))
            return;
        break;
    }

    if (writeRgb || writeAlpha)
    {
        const bool preserveDestinationAlpha = writeRgb && !writeAlpha && p.fpsm == GS_PSM_CT32;
        uint32_t fbrgba = 0;
        if (p.abe || preserveDestinationAlpha)
        {
            fbrgba = p.readFb(m_vram, p.fbp, p.fbw, x, y);
            if (p.fb16)
                fbrgba = Rgba5551ToRgba8888(static_cast<u16>(fbrgba));
            else if (p.fb24)
                fbrgba |= 0x80000000u; // the GS supplies 0x80 as destination alpha for RGB24 blending
        }

        if (p.abe && p.stdBlend && !p.pabe)
        {
            // ((Cs - Cd) * As >> 7) + Cd, the usual alpha blend
            const int sa = a;
            r = clampU8((((static_cast<int>(r) - static_cast<int>(fbrgba & 0xFF)) * sa) >> 7) + static_cast<int>(fbrgba & 0xFF));
            g = clampU8((((static_cast<int>(g) - static_cast<int>((fbrgba >> 8) & 0xFF)) * sa) >> 7) + static_cast<int>((fbrgba >> 8) & 0xFF));
            b = clampU8((((static_cast<int>(b) - static_cast<int>((fbrgba >> 16) & 0xFF)) * sa) >> 7) + static_cast<int>((fbrgba >> 16) & 0xFF));
        }
        else if (p.abe && !(p.pabe && (a & 0x80u) == 0u)) // PABE disables blending when the source alpha MSB is clear
        {
            const int dr = fbrgba & 0xFF;
            const int dg = (fbrgba >> 8) & 0xFF;
            const int db = (fbrgba >> 16) & 0xFF;
            const int da = (fbrgba >> 24) & 0xFF;
            const int cAlpha = (p.csel == 0) ? a : (p.csel == 1) ? da : p.fix;
            auto pick = [](uint8_t sel, int cs, int cd) -> int
            { return sel == 0 ? cs : (sel == 1 ? cd : 0); };
            const int sr = r, sg = g, sb = b;
            r = clampU8(((pick(p.asel, sr, dr) - pick(p.bsel, sr, dr)) * cAlpha >> 7) + pick(p.dsel, sr, dr));
            g = clampU8(((pick(p.asel, sg, dg) - pick(p.bsel, sg, dg)) * cAlpha >> 7) + pick(p.dsel, sg, dg));
            b = clampU8(((pick(p.asel, sb, db) - pick(p.bsel, sb, db)) * cAlpha >> 7) + pick(p.dsel, sb, db));
        }

        if (writeAlpha && p.fba && !p.fb24)
            a = static_cast<uint8_t>(a | 0x80u);

        u32 pixel = pack32(r, g, b, a);
        if (preserveDestinationAlpha)
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        if (p.fb16)
            pixel = Rgba8888ToRgba5551(pixel);
        p.writeFb(m_vram, p.fbp, p.fbw, x, y, pixel);
    }

    if (writeDepth && p.zwrite)
        p.writeZ(m_vram, p.zbp, p.fbw, x, y, static_cast<uint32_t>(z));
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    if (m_px.fast)
        WritePixelFast(x, y, z, r, g, b, a);
    else
        WritePixelSlow(state, x, y, z, r, g, b, a, fog);
}

void GSCpuBackend::WritePixelSlow(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        return;
    }

    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        return;
    }

    bool zpass = false;
    uint32_t storedZ = 0u;
    switch (ztestMethod)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) >= storedZ;
        break;
    case 3:
        storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
        zpass = static_cast<uint32_t>(z) > storedZ;
        break;
    }

    if (!zpass)
    {
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;

                r = clampU8(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = clampU8(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = clampU8(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const bool sixteenBit = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;
    const uint32_t sourceIndex = isFourBitIndexedPsm(sourcePsm)
                                     ? (static_cast<uint32_t>(index) & 0x0Fu)
                                     : static_cast<uint32_t>(index);

    uint32_t clutIndex = (clutBase + sourceIndex) & (sixteenBit ? 0x1FFu : 0x0FFu);
    if (!sixteenBit && csm == 0u && isEightBitIndexedPsm(sourcePsm))
    {
        const uint32_t block = std::min((sourceIndex & 0xF0u) + clutBase, 240u);
        clutIndex = block + (sourceIndex & 0x0Fu);
    }

    switch (cpsm)
    {
    case GS_PSM_CT32:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw);
    }
    case GS_PSM_CT24:
    {
        const uint32_t raw = static_cast<uint32_t>(m_clut[clutIndex]) | (static_cast<uint32_t>(m_clut[clutIndex + 256u]) << 16u);
        return applyTexa(state.texa, cpsm, raw & 0x00FFFFFFu);
    }
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(m_clut[clutIndex]));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

void GSCpuBackend::SetupTexture(const GSDrawState &state)
{
    TexSetup &tx = m_tx;
    tx.valid = false;
    if (!m_ctxs.empty()) m_ctxs[0].quadValid = false;
    if (!state.prim.tme)
        return;

    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;
    tx.psm = tex.psm;
    tx.tbp0 = tex.tbp0;
    tx.tbw = tex.tbw;
    tx.texW = state.textureWidth;
    tx.texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    tx.wrapU = static_cast<uint8_t>(clamp & 0x3u);
    tx.wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    tx.minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    tx.maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    tx.minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    tx.maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
    tx.texa = state.texa;

    const bool indexed = isFourBitIndexedPsm(tex.psm) || isEightBitIndexedPsm(tex.psm);
    tx.kind = indexed ? TexKind::Indexed : ((tex.psm == GS_PSM_CT32 || tex.psm == GS_PSM_Z32) ? TexKind::Direct32 : TexKind::Other);
    if (indexed)
    {
        // CLUT + TEXA are constant while the CLUT/TEXA state is: expand them once into a 256-entry RGBA table.
        TexLutKey key{};
        key.valid = true;
        key.version = m_clutVersion;
        key.cpsm = tex.cpsm;
        key.csm = tex.csm;
        key.csa = tex.csa;
        key.psm = tex.psm;
        key.aem = state.texa.aem;
        key.ta0 = state.texa.ta0;
        key.ta1 = state.texa.ta1;
        if (!m_texLutKey.valid || std::memcmp(&m_texLutKey, &key, sizeof(key)) != 0)
        {
            for (uint32_t i = 0; i < 256u; ++i)
                m_texLut[i] = LookupCLUT(state, static_cast<uint8_t>(i), tex.cpsm, tex.csm, tex.csa, tex.psm);
            m_texLutKey = key;
        }
    }
    tx.valid = true;
}

uint32_t GSCpuBackend::SampleTexture(RasterCtx &rc, const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const TexSetup &tx = m_tx;
    const int texW = tx.texW;
    const int texH = tx.texH;

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, tx.wrapU, tx.minU, tx.maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, tx.wrapV, tx.minV, tx.maxV);

        const u32 out = GSMem::ReadTexture(*rc.cache, m_vram, tx.psm, tx.tbp0, tx.tbw, sampleU, sampleV);

        switch (tx.kind)
        {
        case TexKind::Indexed:
            return m_texLut[out & 0xFFu];
        case TexKind::Direct32:
            return out; // applyTexa leaves CT32/Z32 texels untouched
        default:
            break;
        }

        switch (tx.psm)
        {
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(tx.texa, static_cast<uint8_t>(tx.psm), out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(tx.texa, static_cast<uint8_t>(tx.psm), Rgba5551ToRgba8888(static_cast<u16>(out)));
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    int u0 = static_cast<int>(sampleU);
    if (sampleU < static_cast<float>(u0))
        --u0; // floor
    int v0 = static_cast<int>(sampleV);
    if (sampleV < static_cast<float>(v0))
        --v0;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    // Neighbouring pixels of a magnified texture hit the same 2x2 texel quad: reuse the fetched texels.
    if (!rc.quadValid || rc.quadU != u0 || rc.quadV != v0)
    {
        rc.quad[0] = samplePoint(u0, v0);
        rc.quad[1] = samplePoint(u0 + 1, v0);
        rc.quad[2] = samplePoint(u0, v0 + 1);
        rc.quad[3] = samplePoint(u0 + 1, v0 + 1);
        rc.quadU = u0;
        rc.quadV = v0;
        rc.quadValid = true;
    }
    // The four channels are interpolated with exactly the float operations of lerpChannel():
    //   top = c00 + (c10 - c00) * fx; bottom = c01 + (c11 - c01) * fx; result = trunc(top + (bottom - top) * fy + 0.5), clamped to 0..255.
    const __m128 vc00 = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(rc.quad[0]))));
    const __m128 vc10 = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(rc.quad[1]))));
    const __m128 vc01 = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(rc.quad[2]))));
    const __m128 vc11 = _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(rc.quad[3]))));
    const __m128 vfx = _mm_set1_ps(fx);
    const __m128 vfy = _mm_set1_ps(fy);
    const __m128 top = _mm_add_ps(vc00, _mm_mul_ps(_mm_sub_ps(vc10, vc00), vfx));
    const __m128 bottom = _mm_add_ps(vc01, _mm_mul_ps(_mm_sub_ps(vc11, vc01), vfx));
    const __m128 res = _mm_add_ps(_mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bottom, top), vfy)), _mm_set1_ps(0.5f));
    const __m128i ints = _mm_cvttps_epi32(res);
    const __m128i bytes = _mm_packus_epi16(_mm_packs_epi32(ints, ints), _mm_setzero_si128());
    return static_cast<uint32_t>(_mm_cvtsi128_si32(bytes));
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t pixels = static_cast<uint64_t>(drawX1 - drawX0 + 1) * static_cast<uint64_t>(drawY1 - drawY0 + 1);
    m_stats.bboxPixels += pixels;
    m_stats.coveredPixels += pixels;

    const uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        RunRows(drawY0, drawY1, pixels, [&](RasterCtx &rc, int rowBegin, int rowEnd)
        {
            for (int y = rowBegin; y < rowEnd; ++y)
            {
                float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
                float texVf = v0f + (v1f - v0f) * ty;

                for (int x = drawX0; x <= drawX1; ++x)
                {
                    float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                    float texUf = u0f + (u1f - u0f) * tx;
                    uint32_t texel = 0xFFFF00FFu;
                    if (state.prim.fst)
                    {
                        const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                        const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                        const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                        const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                        texel = SampleTexture(rc, state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                    }
                    else
                    {
                        texel = SampleTexture(rc, state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                    }

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                    WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
                }
            }
        });
    }
    else
    {
        RunRows(drawY0, drawY1, pixels, [&](RasterCtx &, int rowBegin, int rowEnd)
        {
            for (int y = rowBegin; y < rowEnd; ++y)
                for (int x = drawX0; x <= drawX1; ++x)
                    WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
        });
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    {
        // PS2X_GS_MAXTRI=<pixels> (experiment): drop triangles whose raw (unclipped) bounding box is that large on either axis.
        static const int maxTri = []() { const char *v = std::getenv("PS2X_GS_MAXTRI"); return v ? std::atoi(v) : 0; }();
        if (maxTri > 0 && (maxX - minX >= maxTri || maxY - minY >= maxTri))
            return;
    }

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    // Per-edge linear coefficients (double precision) used only to bound the pixels worth testing on each row:
    //   w0(px) = a0*px + b0(py), w1(px) = a1*px + b1(py), w2 = 1 - w0 - w1.
    // The exact (float) coverage test below is unchanged; the interval is widened by kSpanMargin pixels on both sides, far above any
    // float rounding difference, so the set of pixels drawn is identical to testing the whole bounding box.
    constexpr int kSpanMargin = 2;
    const double wnd = static_cast<double>(winding) * static_cast<double>(invAbsDenom);
    const double a0 = static_cast<double>(fy1 - fy2) * wnd;
    const double a1 = static_cast<double>(fy2 - fy0) * wnd;
    const double a2 = -(a0 + a1);
    const double eps = static_cast<double>(kEdgeEpsilon);

    const uint64_t pixels = static_cast<uint64_t>(std::max(0, maxX - minX + 1)) * static_cast<uint64_t>(std::max(0, maxY - minY + 1));
    m_stats.bboxPixels += pixels;

    RunRows(minY, maxY, pixels, [&](RasterCtx &rc, int rowBegin, int rowEnd)
    {
        for (int y = rowBegin; y < rowEnd; ++y)
        {
            float py = static_cast<float>(y) + 0.5f;

            int xs = minX;
            int xe = maxX;
            {
                const double dpy = static_cast<double>(py);
                const double b0 = static_cast<double>(fx2 - fx1) * (dpy - static_cast<double>(fy2)) * wnd - a0 * static_cast<double>(fx2);
                const double b1 = static_cast<double>(fx0 - fx2) * (dpy - static_cast<double>(fy2)) * wnd - a1 * static_cast<double>(fx2);
                const double b2 = 1.0 - b0 - b1;
                const double a[3] = {a0, a1, a2};
                const double b[3] = {b0, b1, b2};
                double lo = -1.0e30, hi = 1.0e30;
                bool empty = false;
                for (int e = 0; e < 3; ++e)
                {
                    // need a*px + b >= -eps
                    if (std::fabs(a[e]) < 1.0e-9)
                    {
                        if (b[e] < -eps - 1.0e-3)
                            empty = true; // the whole row fails this edge (kept a wide tolerance)
                    }
                    else if (a[e] > 0.0)
                        lo = std::max(lo, (-eps - b[e]) / a[e]);
                    else
                        hi = std::min(hi, (-eps - b[e]) / a[e]);
                }
                if (empty || lo > hi + 4.0)
                    continue;
                if (lo > -1.0e29)
                    xs = std::max(xs, static_cast<int>(std::ceil(lo - 0.5)) - kSpanMargin);
                if (hi < 1.0e29)
                    xe = std::min(xe, static_cast<int>(std::floor(hi - 0.5)) + kSpanMargin);
            }

            for (int x = xs; x <= xe; ++x)
            {
                float px = static_cast<float>(x) + 0.5f;

                float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
                float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
                float w2 = 1.0f - w0 - w1;

                if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                    continue;

                double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

                uint8_t r, g, b, a;
                if (state.prim.iip)
                {
                    r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                    g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                    b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                    a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
                }
                else
                {
                    r = v2.r;
                    g = v2.g;
                    b = v2.b;
                    a = v2.a;
                }

                if (state.prim.tme)
                {
                    float is, it, iq;
                    uint16_t iu, iv;
                    if (state.prim.fst)
                    {
                        iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                        iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                        is = 0.0f;
                        it = 0.0f;
                        iq = 1.0f;
                    }
                    else
                    {
                        // The GS DDA interpolates the homogeneous S, T and Q
                        // values. Texel coordinates are calculated from S/Q
                        // and T/Q only after interpolation.
                        is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                        it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                        iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                        iu = 0;
                        iv = 0;
                    }

                    uint32_t texel = SampleTexture(rc, state, is, it, iq, iu, iv);

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const auto &tex = ctx.tex0;
                    const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);

                    r = color.r;
                    g = color.g;
                    b = color.b;
                    a = color.a;
                }

                const uint8_t fog = state.prim.fge ? clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2)) : static_cast<uint8_t>(0);
                WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
            }
        }
    });
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSSmode2State smode2 = decodeSMode2(request.smode2);
    // Interlaced output: the frame buffer holds one field (half the display height), whichever FFMD says -- games such
    // as Shining Wind set FFMD=1 and still alternate DISPFB between two 640x224 field buffers. PS2X_GS_FIELD_READ=0
    // restores the plain full-height read.
    static const bool fieldReadDisabled = []() { const char *v = std::getenv("PS2X_GS_FIELD_READ"); return v != nullptr && v[0] == '0'; }();
    const bool fieldMode = smode2.interlaced && !fieldReadDisabled;
    (void)request.vsyncTick;
    if (std::getenv("PS2X_GS_PRESENT_LOG"))
    {
        static uint64_t lastKey[5] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
        const uint64_t key[5] = {request.pmode, request.smode2, request.dispfb1, request.display1, request.dispfb2};
        bool changed = false;
        for (int i = 0; i < 5; ++i)
            changed |= key[i] != lastKey[i];
        static uint32_t logged = 0;
        if (changed && logged++ < 200u)
        {
            std::memcpy(lastKey, key, sizeof(key));
            std::fprintf(stderr, "[present] pmode=0x%llx smode2=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx fieldMode=%d\n",
                         static_cast<unsigned long long>(request.pmode), static_cast<unsigned long long>(request.smode2),
                         static_cast<unsigned long long>(request.dispfb1), static_cast<unsigned long long>(request.display1),
                         static_cast<unsigned long long>(request.dispfb2), static_cast<unsigned long long>(request.display2), fieldMode ? 1 : 0);
        }
    }
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
        }
        if (pixels.empty() && !CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y))
            return false;

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(pixels, width, height) == 0u)
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                break;
            }
        }
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        const uint32_t read1 = fieldReadHeight(fieldMode, height1);
        const uint32_t read2 = fieldReadHeight(fieldMode, height2);
        if (copySource(displayFrame1, origin1, width1, read1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, read2, false, true, selected2, crt2, preferred2))
        {
            expandFieldLines(crt1, width1, read1, height1);
            expandFieldLines(crt2, width2, read2, height2);
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    const uint32_t readHeight = fieldReadHeight(fieldMode, result.height);
    if (!copySource(displayFrame, origin, result.width, readHeight, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    expandFieldLines(result.pixels, result.width, readHeight, result.height);
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    return result;
}
