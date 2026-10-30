#include "runtime/ps2_ipu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace ps2
{
    static std::atomic<uint64_t> g_commandsIssued{0};
    static uint32_t g_cscMbs = 0;
    static uint32_t g_cscZeroMbs = 0;
    static uint32_t g_vdecMbai[40] = {};
    static uint32_t g_vdecMbt[32] = {};
    static uint32_t g_vdecMotion = 0, g_vdecDmv = 0, g_bdecIntra = 0, g_bdecInter = 0;
    uint64_t ipuCommandsIssued() { return g_commandsIssued.load(std::memory_order_relaxed); }

    namespace
    {
        // ---------------------------------------------------------------------------------------------
        // Tables (ISO/IEC 13818-2). Codes are (code value, bit length).
        // ---------------------------------------------------------------------------------------------
        struct VlcDef
        {
            uint16_t code;
            uint8_t len;
            int32_t value;
        };

        struct VlcLut
        {
            struct Entry
            {
                int32_t value = 0;
                uint8_t len = 0;
            };
            uint32_t bits = 0;
            std::vector<Entry> entries;

            void build(const std::vector<VlcDef> &defs, uint32_t maxBits)
            {
                bits = maxBits;
                entries.assign(size_t(1) << maxBits, Entry{});
                for (const VlcDef &d : defs)
                {
                    const uint32_t first = static_cast<uint32_t>(d.code) << (maxBits - d.len);
                    const uint32_t count = 1u << (maxBits - d.len);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        entries[first + i].value = d.value;
                        entries[first + i].len = d.len;
                    }
                }
            }
        };

        constexpr uint32_t kDctEscape = 111;
        constexpr uint32_t kDctEob = 112;

        // Table B.14 (code, len) for the 111 (run, level) pairs, then escape and EOB.
        const uint16_t kDctB14[113][2] = {
            {0x3, 2}, {0x4, 4}, {0x5, 5}, {0x6, 7}, {0x26, 8}, {0x21, 8}, {0xa, 10}, {0x1d, 12}, {0x18, 12}, {0x13, 12}, {0x10, 12}, {0x1a, 13}, {0x19, 13}, {0x18, 13}, {0x17, 13}, {0x1f, 14},
            {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14}, {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
            {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15}, {0x3, 3}, {0x6, 6}, {0x25, 8}, {0xc, 10}, {0x1b, 12}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
            {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16}, {0x11, 16}, {0x10, 16}, {0x5, 4}, {0x4, 7}, {0xb, 10}, {0x14, 12}, {0x14, 13}, {0x7, 5},
            {0x24, 8}, {0x1c, 12}, {0x13, 13}, {0x6, 5}, {0xf, 10}, {0x12, 12}, {0x7, 6}, {0x9, 10}, {0x12, 13}, {0x5, 6}, {0x1e, 12}, {0x14, 16}, {0x4, 6}, {0x15, 12}, {0x7, 7}, {0x11, 12},
            {0x5, 7}, {0x11, 13}, {0x27, 8}, {0x10, 13}, {0x23, 8}, {0x1a, 16}, {0x22, 8}, {0x19, 16}, {0x20, 8}, {0x18, 16}, {0xe, 10}, {0x17, 16}, {0xd, 10}, {0x16, 16}, {0x8, 10}, {0x15, 16},
            {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}, {0x1, 6},
            {0x2, 2}};

        // Table B.15 (intra VLC format).
        const uint16_t kDctB15[113][2] = {
            {0x2, 2}, {0x6, 3}, {0x7, 4}, {0x1c, 5}, {0x1d, 5}, {0x5, 6}, {0x4, 6}, {0x7b, 7}, {0x7c, 7}, {0x23, 8}, {0x22, 8}, {0xfa, 8}, {0xfb, 8}, {0xfe, 8}, {0xff, 8}, {0x1f, 14},
            {0x1e, 14}, {0x1d, 14}, {0x1c, 14}, {0x1b, 14}, {0x1a, 14}, {0x19, 14}, {0x18, 14}, {0x17, 14}, {0x16, 14}, {0x15, 14}, {0x14, 14}, {0x13, 14}, {0x12, 14}, {0x11, 14}, {0x10, 14}, {0x18, 15},
            {0x17, 15}, {0x16, 15}, {0x15, 15}, {0x14, 15}, {0x13, 15}, {0x12, 15}, {0x11, 15}, {0x10, 15}, {0x2, 3}, {0x6, 5}, {0x79, 7}, {0x27, 8}, {0x20, 8}, {0x16, 13}, {0x15, 13}, {0x1f, 15},
            {0x1e, 15}, {0x1d, 15}, {0x1c, 15}, {0x1b, 15}, {0x1a, 15}, {0x19, 15}, {0x13, 16}, {0x12, 16}, {0x11, 16}, {0x10, 16}, {0x5, 5}, {0x7, 7}, {0xfc, 8}, {0xc, 10}, {0x14, 13}, {0x7, 5},
            {0x26, 8}, {0x1c, 12}, {0x13, 13}, {0x6, 6}, {0xfd, 8}, {0x12, 12}, {0x7, 6}, {0x4, 9}, {0x12, 13}, {0x6, 7}, {0x1e, 12}, {0x14, 16}, {0x4, 7}, {0x15, 12}, {0x5, 7}, {0x11, 12},
            {0x78, 7}, {0x11, 13}, {0x7a, 7}, {0x10, 13}, {0x21, 8}, {0x1a, 16}, {0x25, 8}, {0x19, 16}, {0x24, 8}, {0x18, 16}, {0x5, 9}, {0x17, 16}, {0x7, 9}, {0x16, 16}, {0xd, 10}, {0x15, 16},
            {0x1f, 12}, {0x1a, 12}, {0x19, 12}, {0x17, 12}, {0x16, 12}, {0x1f, 13}, {0x1e, 13}, {0x1d, 13}, {0x1c, 13}, {0x1b, 13}, {0x1f, 16}, {0x1e, 16}, {0x1d, 16}, {0x1c, 16}, {0x1b, 16}, {0x1, 6},
            {0x6, 4}};

        const uint8_t kDctRun[111] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 8, 8,
            9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};

        const uint8_t kDctLevel[111] = {
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 1, 2, 3, 4, 5, 1, 2, 3, 4, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 1, 2,
            1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

        // Table B.9, coded_block_pattern, indexed by the pattern value.
        const uint16_t kCbp[64][2] = {
            {1, 9}, {11, 5}, {9, 5}, {13, 6}, {13, 4}, {23, 7}, {19, 7}, {31, 8}, {12, 4}, {22, 7}, {18, 7}, {30, 8}, {19, 5}, {27, 8}, {23, 8}, {19, 8},
            {11, 4}, {21, 7}, {17, 7}, {29, 8}, {17, 5}, {25, 8}, {21, 8}, {17, 8}, {15, 6}, {15, 8}, {13, 8}, {3, 9}, {15, 5}, {11, 8}, {7, 8}, {7, 9},
            {10, 4}, {20, 7}, {16, 7}, {28, 8}, {14, 6}, {14, 8}, {12, 8}, {2, 9}, {16, 5}, {24, 8}, {20, 8}, {16, 8}, {14, 5}, {10, 8}, {6, 8}, {6, 9},
            {18, 5}, {26, 8}, {22, 8}, {18, 8}, {13, 5}, {9, 8}, {5, 8}, {5, 9}, {12, 5}, {8, 8}, {4, 8}, {4, 9}, {7, 3}, {10, 5}, {8, 5}, {12, 6}};

        // Table B.1, macroblock_address_increment: values 1..33, then stuffing (34) and escape (35).
        const uint16_t kMbai[35][2] = {
            {1, 1}, {3, 3}, {2, 3}, {3, 4}, {2, 4}, {3, 5}, {2, 5}, {7, 7}, {6, 7}, {11, 8}, {10, 8}, {9, 8}, {8, 8}, {7, 8}, {6, 8}, {23, 10}, {22, 10}, {21, 10}, {20, 10}, {19, 10}, {18, 10},
            {35, 11}, {34, 11}, {33, 11}, {32, 11}, {31, 11}, {30, 11}, {29, 11}, {28, 11}, {27, 11}, {26, 11}, {25, 11}, {24, 11}, {15, 11}, {8, 11}};

        // Table B.10, motion_code magnitude 0..16 (code without the sign bit).
        const uint16_t kMotion[17][2] = {{1, 1}, {1, 2}, {1, 3}, {1, 4}, {3, 6}, {5, 7}, {4, 7}, {3, 7}, {11, 9}, {10, 9}, {9, 9}, {17, 10}, {16, 10}, {15, 10}, {14, 10}, {13, 10}, {12, 10}};

        // Tables B.12 / B.13, dct_dc_size 0..11.
        const uint16_t kDcLum[12][2] = {{0x4, 3}, {0x0, 2}, {0x1, 2}, {0x5, 3}, {0x6, 3}, {0xe, 4}, {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x1ff, 9}};
        const uint16_t kDcChroma[12][2] = {{0x0, 2}, {0x1, 2}, {0x2, 2}, {0x6, 3}, {0xe, 4}, {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x3fe, 10}, {0x3ff, 10}};

        // Macroblock type flags (libmpeg2 layout, which is also what the IPU returns from VDEC).
        constexpr int32_t kMbIntra = 0x01;
        constexpr int32_t kMbPattern = 0x02;
        constexpr int32_t kMbBackward = 0x04;
        constexpr int32_t kMbForward = 0x08;
        constexpr int32_t kMbQuant = 0x10;

        const VlcDef kMbTypeI[] = {{1, 1, kMbIntra}, {1, 2, kMbIntra | kMbQuant}};
        const VlcDef kMbTypeP[] = {{1, 1, kMbForward | kMbPattern}, {1, 2, kMbPattern}, {1, 3, kMbForward},
                                   {3, 5, kMbIntra}, {2, 5, kMbQuant | kMbForward | kMbPattern}, {1, 5, kMbQuant | kMbPattern},
                                   {1, 6, kMbQuant | kMbIntra}};
        const VlcDef kMbTypeB[] = {{2, 2, kMbForward | kMbBackward}, {3, 2, kMbForward | kMbBackward | kMbPattern},
                                   {2, 3, kMbBackward}, {3, 3, kMbBackward | kMbPattern}, {2, 4, kMbForward},
                                   {3, 4, kMbForward | kMbPattern}, {3, 5, kMbIntra},
                                   {2, 5, kMbQuant | kMbForward | kMbBackward | kMbPattern},
                                   {3, 6, kMbQuant | kMbForward | kMbPattern}, {2, 6, kMbQuant | kMbBackward | kMbPattern},
                                   {1, 6, kMbQuant | kMbIntra}};
        const VlcDef kDmv[] = {{0, 1, 0}, {2, 2, 1}, {3, 2, -1}};

        const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                     35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        const uint8_t kAlternate[64] = {0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
                                        51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45, 53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63};
        const uint8_t kNonLinearQscale[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

        const uint8_t kDefaultIntra[64] = {8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
                                           22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

        struct Tables
        {
            VlcLut dct0, dct1, cbp, mbai, motion, dcLum, dcChroma, mbtI, mbtP, mbtB, dmv;
            Tables()
            {
                std::vector<VlcDef> d;
                for (uint32_t i = 0; i < 113; ++i)
                    d.push_back({kDctB14[i][0], static_cast<uint8_t>(kDctB14[i][1]), static_cast<int32_t>(i)});
                dct0.build(d, 16);
                d.clear();
                for (uint32_t i = 0; i < 113; ++i)
                    d.push_back({kDctB15[i][0], static_cast<uint8_t>(kDctB15[i][1]), static_cast<int32_t>(i)});
                dct1.build(d, 16);
                d.clear();
                for (int32_t i = 0; i < 64; ++i)
                    d.push_back({kCbp[i][0], static_cast<uint8_t>(kCbp[i][1]), i});
                cbp.build(d, 9);
                d.clear();
                for (int32_t i = 0; i < 35; ++i)
                    d.push_back({kMbai[i][0], static_cast<uint8_t>(kMbai[i][1]), i + 1});
                mbai.build(d, 11);
                d.clear();
                for (int32_t i = 0; i < 17; ++i)
                    d.push_back({kMotion[i][0], static_cast<uint8_t>(kMotion[i][1]), i});
                motion.build(d, 10);
                d.clear();
                for (int32_t i = 0; i < 12; ++i)
                    d.push_back({kDcLum[i][0], static_cast<uint8_t>(kDcLum[i][1]), i});
                dcLum.build(d, 9);
                d.clear();
                for (int32_t i = 0; i < 12; ++i)
                    d.push_back({kDcChroma[i][0], static_cast<uint8_t>(kDcChroma[i][1]), i});
                dcChroma.build(d, 10);
                mbtI.build(std::vector<VlcDef>(std::begin(kMbTypeI), std::end(kMbTypeI)), 2);
                mbtP.build(std::vector<VlcDef>(std::begin(kMbTypeP), std::end(kMbTypeP)), 6);
                mbtB.build(std::vector<VlcDef>(std::begin(kMbTypeB), std::end(kMbTypeB)), 6);
                dmv.build(std::vector<VlcDef>(std::begin(kDmv), std::end(kDmv)), 2);
            }
        };

        const Tables &tables()
        {
            static const Tables t;
            return t;
        }

        // 8-point IDCT basis, a(u) * cos((2n+1) u pi / 16).
        struct IdctBasis
        {
            float c[8][8];
            IdctBasis()
            {
                const double pi = 3.14159265358979323846;
                for (int u = 0; u < 8; ++u)
                    for (int n = 0; n < 8; ++n)
                        c[u][n] = static_cast<float>((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * n + 1) * u * pi / 16.0));
            }
        };

        const IdctBasis &idctBasis()
        {
            static const IdctBasis b;
            return b;
        }

        inline int32_t clampInt(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

        bool ipuLogEnabled()
        {
            static const bool enabled = std::getenv("PS2X_IPU_LOG") != nullptr;
            return enabled;
        }
    } // namespace

    // -------------------------------------------------------------------------------------------------
    // Kernels
    // -------------------------------------------------------------------------------------------------
    void Ipu::inverseDct8x8(const int16_t *coefficients, int16_t *samples)
    {
        const IdctBasis &b = idctBasis();
        float tmp[64];
        // Rows.
        for (int y = 0; y < 8; ++y)
        {
            for (int x = 0; x < 8; ++x)
            {
                float s = 0.0f;
                for (int u = 0; u < 8; ++u)
                    s += b.c[u][x] * static_cast<float>(coefficients[y * 8 + u]);
                tmp[y * 8 + x] = s;
            }
        }
        // Columns.
        for (int x = 0; x < 8; ++x)
        {
            for (int y = 0; y < 8; ++y)
            {
                float s = 0.0f;
                for (int v = 0; v < 8; ++v)
                    s += b.c[v][y] * tmp[v * 8 + x];
                samples[y * 8 + x] = static_cast<int16_t>(clampInt(static_cast<int32_t>(std::floor(s + 0.5f)), -32768, 32767));
            }
        }
    }

    void Ipu::yuv420ToRgb32(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, uint32_t th0, uint32_t th1, uint8_t *rgba)
    {
        for (int row = 0; row < 16; ++row)
        {
            for (int col = 0; col < 16; ++col)
            {
                const int32_t yy = 149 * (static_cast<int32_t>(y[row * 16 + col]) - 16);
                const int32_t u = static_cast<int32_t>(cb[(row >> 1) * 8 + (col >> 1)]) - 128;
                const int32_t v = static_cast<int32_t>(cr[(row >> 1) * 8 + (col >> 1)]) - 128;
                const int32_t r = clampInt((yy + 204 * v + 64) >> 7, 0, 255);
                const int32_t g = clampInt((yy - 104 * v - 50 * u + 64) >> 7, 0, 255);
                const int32_t bl = clampInt((yy + 258 * u + 64) >> 7, 0, 255);
                uint8_t *p = rgba + (row * 16 + col) * 4;
                p[0] = static_cast<uint8_t>(r);
                p[1] = static_cast<uint8_t>(g);
                p[2] = static_cast<uint8_t>(bl);
                p[3] = 0x80;
                if (th0 > 0 && static_cast<uint32_t>(r) < th0 && static_cast<uint32_t>(g) < th0 && static_cast<uint32_t>(bl) < th0)
                {
                    p[0] = p[1] = p[2] = p[3] = 0;
                }
                else if (static_cast<uint32_t>(r) < th1 && static_cast<uint32_t>(g) < th1 && static_cast<uint32_t>(bl) < th1)
                {
                    p[3] = 0x40;
                }
            }
        }
    }

    // -------------------------------------------------------------------------------------------------
    // Construction, reset and registers
    // -------------------------------------------------------------------------------------------------
    Ipu::Ipu()
    {
        reset();
    }

    void Ipu::reset()
    {
        clearInput();
        m_out.clear();
        m_ctrl = 0;
        m_cbp = 0;
        m_top = 0;
        m_cmdData = 0;
        m_lastCommand = 0xFFFFFFFFu;
        m_busy = false;
        m_running = false;
        m_attempting = false;
        m_cmdPhase = 0;
        m_th0 = m_th1 = 0;
        m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = 128;
        // Neutral defaults: the MPEG default matrices (software normally loads its own with SETIQ).
        for (int i = 0; i < 64; ++i)
        {
            m_intraMatrix[i] = kDefaultIntra[i];
            m_nonIntraMatrix[i] = 16;
        }
        m_vqClut.fill(0);
        m_commandCount = 0;
        m_macroblockCount = 0;
    }

    uint32_t Ipu::outFifoQwords() const
    {
        return static_cast<uint32_t>(std::min<size_t>(kFifoQwords, m_out.size() / 16u));
    }

    uint32_t Ipu::ctrl() const
    {
        uint32_t v = m_ctrl & ~(kCtrlRst);
        v |= (inFifoQwords() & 0xFu) << kCtrlIfcShift;
        v |= (outFifoQwords() & 0xFu) << kCtrlOfcShift;
        v |= (m_cbp & 0x3Fu) << kCtrlCbpShift;
        if (m_busy)
            v |= kCtrlBusy;
        return v;
    }

    uint32_t Ipu::readRegister(uint32_t offset)
    {
        run();
        switch (offset & 0x3Fu)
        {
        case 0x00:
        {
            if (m_lastCommand == kVdec || m_lastCommand == kFdec)
                return m_cmdData;
            uint32_t v = 0;
            const uint32_t avail = std::min<uint32_t>(32u, availableBits() + (static_cast<uint32_t>(m_fifo.size()) * 128u));
            (void)avail;
            if (loadWindow(32))
            {
                peekBits(32, v);
            }
            return v;
        }
        case 0x04:
            return m_busy ? 0x80000000u : 0u;
        case 0x10:
            return ctrl();
        case 0x20:
        {
            return (m_fp << 16) | (inFifoQwords() << 8) | (m_bp & 0x7Fu);
        }
        case 0x30:
        {
            if (m_busy)
                return 0;
            uint32_t v = 0;
            if (loadWindow(32))
                peekBits(32, v);
            return v;
        }
        case 0x34:
        {
            const bool full = availableBits() >= 32u || m_fifo.size() > 0;
            return (m_busy || !full) ? 0x80000000u : 0u;
        }
        default:
            return 0;
        }
    }

    void Ipu::writeRegister(uint32_t offset, uint32_t value)
    {
        switch (offset & 0x3Fu)
        {
        case 0x00:
            startCommand(value);
            run();
            break;
        case 0x10:
        {
            const uint32_t writable = (3u << kCtrlIdpShift) | kCtrlAs | kCtrlIvf | kCtrlQst | kCtrlMp1 | (7u << kCtrlPctShift);
            if (value & kCtrlRst)
            {
                // Soft reset: FIFOs, bit pointer, thresholds, flags and the running command.
                clearInput();
                m_out.clear();
                m_cbp = 0;
                m_top = 0;
                m_cmdData = 0;
                m_th0 = m_th1 = 0;
                m_busy = false;
                m_cmdPhase = 0;
                m_lastCommand = 0xFFFFFFFFu;
                m_ctrl = 0;
                m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = 128;
                if (m_interrupt)
                    m_interrupt();
                run();
                break;
            }
            m_ctrl = (m_ctrl & (kCtrlEcd | kCtrlScd)) | (value & writable);
            if (((m_ctrl >> kCtrlIdpShift) & 3u) == 3u)
                m_ctrl = (m_ctrl & ~(3u << kCtrlIdpShift)) | (1u << kCtrlIdpShift);
            run();
            break;
        }
        default:
            break;
        }
    }

    bool Ipu::writeInFifo(const uint8_t *qword)
    {
        if (m_fifo.size() >= kFifoQwords)
            return false;
        std::array<uint8_t, 16> q;
        std::memcpy(q.data(), qword, 16);
        m_fifo.push_back(q);
        run();
        return true;
    }

    bool Ipu::readOutFifo(uint8_t *qword)
    {
        if (m_out.size() < 16u)
            return false;
        for (int i = 0; i < 16; ++i)
        {
            qword[i] = m_out.front();
            m_out.pop_front();
        }
        run();
        return true;
    }

    // -------------------------------------------------------------------------------------------------
    // Bit stream
    // -------------------------------------------------------------------------------------------------
    void Ipu::clearInput()
    {
        m_fifo.clear();
        m_window.fill(0);
        m_fp = 0;
        m_bp = 0;
        m_attemptPulled.clear();
    }

    void Ipu::pullInput()
    {
        if (!m_source)
            return;
        while (m_fifo.size() < kFifoQwords)
        {
            std::array<uint8_t, 16> q;
            if (m_source(q.data(), 1) == 0)
                break;
            m_fifo.push_back(q);
            if (ipuLogEnabled())
            {
                static uint32_t pulled = 0;
                if (pulled < 16u || (pulled % 4096u) == 0u)
                {
                    std::fprintf(stderr, "[IPU:dma4] qw#%u:", pulled);
                    for (int b = 0; b < 16; ++b)
                        std::fprintf(stderr, " %02x", q[b]);
                    std::fprintf(stderr, "\n");
                }
                ++pulled;
            }
            if (m_attempting)
                m_attemptPulled.push_back(q);
        }
    }

    void Ipu::normalizeWindow()
    {
        while (m_bp >= 128u && m_fp > 0)
        {
            std::memmove(m_window.data(), m_window.data() + 16, 16);
            --m_fp;
            m_bp -= 128u;
        }
    }

    bool Ipu::loadWindow(uint32_t bitsNeeded)
    {
        while (availableBits() < bitsNeeded)
        {
            if (m_fp >= 2)
                return false;
            if (m_fifo.empty())
                pullInput();
            if (m_fifo.empty())
                return false;
            std::memcpy(m_window.data() + m_fp * 16u, m_fifo.front().data(), 16);
            m_fifo.pop_front();
            ++m_fp;
            // A source that was blocked by a full FIFO can refill now.
            pullInput();
        }
        return true;
    }

    namespace
    {
        uint32_t readRaw(const uint8_t *window, uint32_t bp, uint32_t count)
        {
            if (count == 0)
                return 0;
            uint64_t acc = 0;
            const uint32_t byte = bp >> 3;
            for (uint32_t k = 0; k < 8; ++k)
                acc = (acc << 8) | window[byte + k];
            acc <<= (bp & 7u);
            return static_cast<uint32_t>(acc >> (64u - count));
        }
    } // namespace

    bool Ipu::peekBits(uint32_t count, uint32_t &value)
    {
        if (count == 0)
        {
            value = 0;
            return true;
        }
        if (!loadWindow(count))
            return false;
        value = readRaw(m_window.data(), m_bp, count);
        return true;
    }

    bool Ipu::skipBits(uint32_t count)
    {
        if (count == 0)
            return true;
        if (!loadWindow(count))
            return false;
        m_bp += count;
        normalizeWindow();
        return true;
    }

    bool Ipu::getBits(uint32_t count, uint32_t &value)
    {
        if (!peekBits(count, value))
            return false;
        m_bp += count;
        normalizeWindow();
        return true;
    }

    void Ipu::beginAttempt()
    {
        m_snapshot.window = m_window;
        m_snapshot.fp = m_fp;
        m_snapshot.bp = m_bp;
        m_snapshot.fifo = m_fifo;
        m_snapshot.dc[0] = m_dcPredictor[0];
        m_snapshot.dc[1] = m_dcPredictor[1];
        m_snapshot.dc[2] = m_dcPredictor[2];
        m_attemptPulled.clear();
        m_attempting = true;
    }

    void Ipu::commitAttempt()
    {
        m_attempting = false;
        m_attemptPulled.clear();
    }

    void Ipu::rollbackAttempt()
    {
        m_window = m_snapshot.window;
        m_fp = m_snapshot.fp;
        m_bp = m_snapshot.bp;
        m_fifo = m_snapshot.fifo;
        for (const auto &q : m_attemptPulled)
            m_fifo.push_back(q);
        m_dcPredictor[0] = m_snapshot.dc[0];
        m_dcPredictor[1] = m_snapshot.dc[1];
        m_dcPredictor[2] = m_snapshot.dc[2];
        m_attemptPulled.clear();
        m_attempting = false;
    }

    // Decodes a VLC without consuming it. Returns 0 = ok, 1 = starved, 2 = invalid code.
    int Ipu::peekVlc(const void *lutPtr, int32_t &value, uint32_t &length)
    {
        const VlcLut &lut = *static_cast<const VlcLut *>(lutPtr);
        loadWindow(lut.bits);
        const uint32_t avail = std::min(availableBits(), lut.bits);
        if (avail == 0)
            return 1;
        const uint32_t v = readRaw(m_window.data(), m_bp, avail) << (lut.bits - avail);
        const VlcLut::Entry &e = lut.entries[v];
        if (e.len == 0)
            return avail < lut.bits ? 1 : 2;
        if (e.len > avail)
            return 1;
        value = e.value;
        length = e.len;
        return 0;
    }

    // -------------------------------------------------------------------------------------------------
    // Output
    // -------------------------------------------------------------------------------------------------
    void Ipu::appendOutput(const uint8_t *data, size_t bytes)
    {
        m_out.insert(m_out.end(), data, data + bytes);
    }

    bool Ipu::pushOutput()
    {
        if (!m_sink || m_out.size() < 16u)
            return false;
        const size_t qwords = m_out.size() / 16u;
        std::vector<uint8_t> tmp(qwords * 16u);
        std::copy(m_out.begin(), m_out.begin() + static_cast<std::ptrdiff_t>(tmp.size()), tmp.begin());
        const size_t accepted = m_sink(tmp.data(), qwords);
        if (accepted == 0)
            return false;
        m_out.erase(m_out.begin(), m_out.begin() + static_cast<std::ptrdiff_t>(accepted * 16u));
        return true;
    }

    // -------------------------------------------------------------------------------------------------
    // Execution
    // -------------------------------------------------------------------------------------------------
    void Ipu::run()
    {
        if (m_running)
            return;
        // PS2X_THREAD_PROFILE=1: host time spent inside the IPU emulation, printed every ~5 s.
        static const bool profile = std::getenv("PS2X_THREAD_PROFILE") != nullptr;
        if (profile)
        {
            static uint64_t nanos = 0, calls = 0;
            static auto lastPrint = std::chrono::steady_clock::now();
            const auto t0 = std::chrono::steady_clock::now();
            runLoop();
            const auto t1 = std::chrono::steady_clock::now();
            nanos += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            ++calls;
            if (t1 - lastPrint > std::chrono::seconds(5))
            {
                lastPrint = t1;
                std::fprintf(stderr, "[ipuprof] host=%llums runs=%llu commands=%llu macroblocks=%llu\n", static_cast<unsigned long long>(nanos / 1000000u),
                             static_cast<unsigned long long>(calls), static_cast<unsigned long long>(m_commandCount), static_cast<unsigned long long>(m_macroblockCount));
            }
            return;
        }
        runLoop();
    }

    void Ipu::runLoop()
    {
        if (m_running)
            return;
        m_running = true;
        uint32_t guard = 0;
        bool progress;
        do
        {
            progress = false;
            pullInput();
            if (pushOutput())
                progress = true;
            if (m_busy && stepCommand())
                progress = true;
        } while (progress && ++guard < 1000000u);
        m_running = false;
    }

    void Ipu::startCommand(uint32_t value)
    {
        m_ctrl &= ~(kCtrlEcd | kCtrlScd);
        m_cmdId = value >> 28;
        m_cmdOption = value & 0x0FFFFFFFu;
        m_cmdPhase = 0;
        m_lastCommand = m_cmdId;
        m_busy = true;
        ++m_commandCount;
        g_commandsIssued.fetch_add(1, std::memory_order_relaxed);
        if (ipuLogEnabled())
        {
            // PS2X_IPU_LOG=1: command trace; consecutive identical command words are folded into a counter.
            static uint32_t logged = 0;
            static uint32_t lastValue = 0xFFFFFFFFu;
            static uint32_t repeats = 0;
            if (value == lastValue)
            {
                ++repeats;
            }
            else
            {
                if (repeats != 0 && logged < 60000u)
                    std::fprintf(stderr, "[IPU:cmd]   (previous command repeated %u more times)\n", repeats);
                repeats = 0;
                lastValue = value;
                if (logged++ < 60000u)
                {
                    static const auto t0 = std::chrono::steady_clock::now();
                    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                    std::fprintf(stderr, "[IPU:cmd] @%lldms #%llu 0x%08X id=%u ctrl=0x%08X ifc=%u fp=%u bp=%u\n", ms, static_cast<unsigned long long>(m_commandCount), value, m_cmdId, ctrl(), inFifoQwords(), m_fp, m_bp);
                }
            }
        }
    }

    void Ipu::finishCommand(bool raiseInterrupt)
    {
        m_busy = false;
        m_attempting = false;
        if (raiseInterrupt && m_interrupt)
            m_interrupt();
    }

    bool Ipu::stepCommand()
    {
        switch (m_cmdId)
        {
        case kBclr:
            clearInput();
            m_bp = m_cmdOption & 0x7Fu;
            finishCommand();
            return true;
        case kSetth:
            m_th0 = m_cmdOption & 0x1FFu;
            m_th1 = (m_cmdOption >> 16) & 0x1FFu;
            finishCommand();
            return true;
        case kIdec:
            return stepIdec();
        case kBdec:
            return stepBdec();
        case kVdec:
            return stepVdec();
        case kFdec:
            return stepFdec();
        case kSetiq:
            return stepSetiq();
        case kSetvq:
            return stepSetvq();
        case kCsc:
            return stepCsc();
        case kPack:
            return stepPack();
        default:
            finishCommand();
            return true;
        }
    }

    // Skips the FB bits at the beginning of a command (phase 0 -> 1).
    bool Ipu::skipFirstBits()
    {
        beginAttempt();
        if (!skipBits(m_cmdOption & 0x3Fu))
        {
            rollbackAttempt();
            return false;
        }
        commitAttempt();
        m_cmdPhase = 1;
        return true;
    }

    void Ipu::raiseEnd(bool startCode)
    {
        m_ctrl |= startCode ? kCtrlScd : kCtrlEcd;
    }

    bool Ipu::stepVdec()
    {
        if (m_cmdPhase == 0 && !skipFirstBits())
            return false;
        const Tables &t = tables();
        const uint32_t table = (m_cmdOption >> 26) & 3u;
        beginAttempt();
        int32_t value = 0;
        uint32_t length = 0;
        int r = 0;
        switch (table)
        {
        case 0:
            r = peekVlc(&t.mbai, value, length);
            break;
        case 1:
        {
            const uint32_t pct = (m_ctrl >> kCtrlPctShift) & 7u;
            r = peekVlc(pct == 2 ? static_cast<const void *>(&t.mbtP) : (pct == 3 ? static_cast<const void *>(&t.mbtB) : static_cast<const void *>(&t.mbtI)), value, length);
            break;
        }
        case 2:
        {
            r = peekVlc(&t.motion, value, length);
            if (r == 0 && value != 0)
            {
                // Sign bit follows the magnitude code.
                if (!loadWindow(length + 1))
                {
                    r = 1;
                }
                else
                {
                    const uint32_t bits = readRaw(m_window.data(), m_bp, length + 1);
                    if (bits & 1u)
                        value = -value;
                    ++length;
                }
            }
            break;
        }
        default:
            r = peekVlc(&t.dmv, value, length);
            break;
        }
        if (r == 1)
        {
            rollbackAttempt();
            return false;
        }
        if (r == 2)
        {
            commitAttempt();
            m_cmdData = 0;
            raiseEnd(false);
            finishCommand();
            return true;
        }
        skipBits(length);
        commitAttempt();
        if (ipuLogEnabled())
        {
            if (table == 0 && value >= 0 && value < 40)
                ++g_vdecMbai[value];
            else if (table == 1 && value >= 0 && value < 32)
                ++g_vdecMbt[value];
            else if (table == 2)
                ++g_vdecMotion;
            else if (table == 3)
                ++g_vdecDmv;
        }
        m_cmdData = (length << 16) | (static_cast<uint32_t>(value) & 0xFFFFu);
        finishCommand();
        return true;
    }

    bool Ipu::stepFdec()
    {
        if (m_cmdPhase == 0 && !skipFirstBits())
            return false;
        uint32_t v = 0;
        beginAttempt();
        if (!peekBits(32, v))
        {
            rollbackAttempt();
            return false;
        }
        commitAttempt();
        m_cmdData = v;
        m_top = v;
        if (ipuLogEnabled())
        {
            static uint32_t shown = 0;
            if (shown++ < 24u || (v >> 8) == 1u)
                std::fprintf(stderr, "[IPU:fdec] result=0x%08X (bp=%u fp=%u)\n", v, m_bp, m_fp);
        }
        finishCommand();
        return true;
    }

    bool Ipu::stepSetiq()
    {
        if (m_cmdPhase == 0 && !skipFirstBits())
            return false;
        beginAttempt();
        uint8_t raw[64];
        for (int i = 0; i < 64; ++i)
        {
            uint32_t v;
            if (!getBits(8, v))
            {
                rollbackAttempt();
                return false;
            }
            raw[i] = static_cast<uint8_t>(v);
        }
        commitAttempt();
        // Matrices travel in zigzag order and are kept in raster order.
        auto &dst = (m_cmdOption & (1u << 27)) ? m_nonIntraMatrix : m_intraMatrix;
        for (int i = 0; i < 64; ++i)
            dst[kZigzag[i]] = raw[i];
        finishCommand();
        return true;
    }

    bool Ipu::stepSetvq()
    {
        beginAttempt();
        uint16_t clut[16];
        for (int i = 0; i < 16; ++i)
        {
            uint32_t lo, hi;
            if (!getBits(8, lo) || !getBits(8, hi))
            {
                rollbackAttempt();
                return false;
            }
            clut[i] = static_cast<uint16_t>(lo | (hi << 8));
        }
        commitAttempt();
        std::copy(clut, clut + 16, m_vqClut.begin());
        finishCommand();
        return true;
    }

    // Reads `count` whole bytes into dst (bit aligned stream).
    bool Ipu::readBytes(uint8_t *dst, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t v;
            if (!getBits(8, v))
                return false;
            dst[i] = static_cast<uint8_t>(v);
        }
        return true;
    }

    namespace
    {
        const int8_t kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

        void rgb32ToRgb16(const uint8_t *rgba, bool dither, uint8_t *out)
        {
            for (int y = 0; y < 16; ++y)
            {
                for (int x = 0; x < 16; ++x)
                {
                    const uint8_t *p = rgba + (y * 16 + x) * 4;
                    int32_t r = p[0], g = p[1], b = p[2];
                    if (dither)
                    {
                        const int d = kDither[y & 3][x & 3];
                        r = clampInt(r + d, 0, 255);
                        g = clampInt(g + d, 0, 255);
                        b = clampInt(b + d, 0, 255);
                    }
                    const uint16_t v = static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((p[3] & 0x80u) ? 0x8000u : 0u));
                    out[(y * 16 + x) * 2] = static_cast<uint8_t>(v & 0xFF);
                    out[(y * 16 + x) * 2 + 1] = static_cast<uint8_t>(v >> 8);
                }
            }
        }
    } // namespace

    bool Ipu::stepCsc()
    {
        if (m_cmdPhase == 0)
        {
            m_mbRemaining = m_cmdOption & 0x7FFu;
            m_cmdPhase = 1;
        }
        bool progress = false;
        while (m_mbRemaining > 0)
        {
            if (!outputHasRoom())
                return progress;
            beginAttempt();
            uint8_t yuv[384];
            if (!readBytes(yuv, 384))
            {
                rollbackAttempt();
                return progress;
            }
            commitAttempt();
            if (ipuLogEnabled())
            {
                bool allZero = true;
                for (int k = 0; k < 384 && allZero; ++k)
                    allZero = yuv[k] == 0;
                g_cscZeroMbs += allZero ? 1u : 0u;
                ++g_cscMbs;
            }
            uint8_t rgba[1024];
            yuv420ToRgb32(yuv, yuv + 256, yuv + 320, m_th0, m_th1, rgba);
            if (m_cmdOption & (1u << 27))
            {
                uint8_t out16[512];
                rgb32ToRgb16(rgba, (m_cmdOption & (1u << 26)) != 0, out16);
                appendOutput(out16, sizeof(out16));
            }
            else
            {
                appendOutput(rgba, sizeof(rgba));
            }
            --m_mbRemaining;
            ++m_macroblockCount;
            progress = true;
        }
        if (!outputHasRoom())
            return progress;
        if (ipuLogEnabled())
        {
            std::fprintf(stderr, "[IPU:csc] done: %u macroblocks, %u with all-zero YCbCr input\n", g_cscMbs, g_cscZeroMbs);
            std::fprintf(stderr, "[IPU:stats] mbai:");
            for (int k = 0; k < 40; ++k)
                if (g_vdecMbai[k])
                    std::fprintf(stderr, " %d=%u", k, g_vdecMbai[k]);
            std::fprintf(stderr, " | mbt:");
            for (int k = 0; k < 32; ++k)
                if (g_vdecMbt[k])
                    std::fprintf(stderr, " 0x%02x=%u", k, g_vdecMbt[k]);
            std::fprintf(stderr, " | motion=%u dmv=%u bdecIntra=%u bdecInter=%u pct=%u\n", g_vdecMotion, g_vdecDmv, g_bdecIntra, g_bdecInter, (m_ctrl >> kCtrlPctShift) & 7u);
            std::memset(g_vdecMbai, 0, sizeof(g_vdecMbai));
            std::memset(g_vdecMbt, 0, sizeof(g_vdecMbt));
            g_vdecMotion = g_vdecDmv = g_bdecIntra = g_bdecInter = 0;
            g_cscMbs = g_cscZeroMbs = 0;
        }
        finishCommand();
        return true;
    }

    bool Ipu::stepPack()
    {
        if (m_cmdPhase == 0)
        {
            m_mbRemaining = std::max<uint32_t>(1u, m_cmdOption & 0x7FFu);
            m_cmdPhase = 1;
        }
        bool progress = false;
        while (m_mbRemaining > 0)
        {
            if (!outputHasRoom())
                return progress;
            beginAttempt();
            uint8_t rgba[1024];
            if (!readBytes(rgba, 1024))
            {
                rollbackAttempt();
                return progress;
            }
            commitAttempt();
            uint8_t out16[512];
            rgb32ToRgb16(rgba, (m_cmdOption & (1u << 26)) != 0, out16);
            if (m_cmdOption & (1u << 27))
            {
                appendOutput(out16, sizeof(out16));
            }
            else
            {
                // 4-bit palette indices: nearest CLUT entry by squared RGB distance.
                uint8_t idx[256];
                for (int i = 0; i < 256; ++i)
                {
                    const uint16_t px = static_cast<uint16_t>(out16[i * 2] | (out16[i * 2 + 1] << 8));
                    const int32_t pr = px & 31, pg = (px >> 5) & 31, pb = (px >> 10) & 31;
                    int32_t best = 0x7FFFFFFF;
                    uint8_t bi = 0;
                    for (int c = 0; c < 16; ++c)
                    {
                        const int32_t cr = m_vqClut[c] & 31, cg = (m_vqClut[c] >> 5) & 31, cb = (m_vqClut[c] >> 10) & 31;
                        const int32_t d = (pr - cr) * (pr - cr) + (pg - cg) * (pg - cg) + (pb - cb) * (pb - cb);
                        if (d < best)
                        {
                            best = d;
                            bi = static_cast<uint8_t>(c);
                        }
                    }
                    idx[i] = bi;
                }
                uint8_t packed[128];
                for (int i = 0; i < 128; ++i)
                    packed[i] = static_cast<uint8_t>(idx[i * 2] | (idx[i * 2 + 1] << 4));
                appendOutput(packed, sizeof(packed));
            }
            --m_mbRemaining;
            progress = true;
        }
        if (!outputHasRoom())
            return progress;
        finishCommand();
        return true;
    }

    // -------------------------------------------------------------------------------------------------
    // Macroblock decoding
    // -------------------------------------------------------------------------------------------------
    // Decodes one 8x8 block. `block` receives the dequantised coefficients in raster order.
    // Returns 0 = ok, 1 = starved, 2 = invalid.
    int Ipu::decodeBlock(uint32_t blockIndex, bool intra, uint32_t quantiserScale, int32_t *block)
    {
        const Tables &t = tables();
        std::memset(block, 0, 64 * sizeof(int32_t));
        const bool mpeg1 = (m_ctrl & kCtrlMp1) != 0;
        const bool alternate = (m_ctrl & kCtrlAs) != 0;
        const uint8_t *scan = alternate ? kAlternate : kZigzag;
        const auto &matrix = intra ? m_intraMatrix : m_nonIntraMatrix;
        uint32_t i = 0;

        if (intra)
        {
            const uint32_t channel = blockIndex < 4 ? 0u : blockIndex - 3u;
            int32_t size = 0;
            uint32_t length = 0;
            int r = peekVlc(channel == 0 ? static_cast<const void *>(&t.dcLum) : static_cast<const void *>(&t.dcChroma), size, length);
            if (r != 0)
                return r;
            skipBits(length);
            int32_t diff = 0;
            if (size > 0)
            {
                uint32_t bits;
                if (!getBits(static_cast<uint32_t>(size), bits))
                    return 1;
                diff = static_cast<int32_t>(bits);
                if (!(bits >> (size - 1)))
                    diff -= (1 << size) - 1;
            }
            m_dcPredictor[channel] += diff;
            const uint32_t precision = (m_ctrl >> kCtrlIdpShift) & 3u;
            const int32_t mult = mpeg1 ? 8 : (8 >> precision);
            block[0] = clampInt(m_dcPredictor[channel] * mult, -2048, 2047);
            i = 1;
        }

        const VlcLut &lut = (intra && (m_ctrl & kCtrlIvf) && !mpeg1) ? t.dct1 : t.dct0;
        bool first = !intra;
        for (;;)
        {
            int32_t index = 0;
            uint32_t length = 0;
            int32_t run, level;
            bool negative;
            if (first)
            {
                // The first coefficient of a non-intra block uses "1s" for (run 0, level 1).
                uint32_t two;
                if (!peekBits(2, two))
                    return 1;
                if (two & 2u)
                {
                    skipBits(2);
                    run = 0;
                    level = 1;
                    negative = (two & 1u) != 0;
                    goto have_coefficient;
                }
            }
            {
                int r = peekVlc(&lut, index, length);
                if (r != 0)
                    return r;
                skipBits(length);
                if (static_cast<uint32_t>(index) == kDctEob)
                    break;
                if (static_cast<uint32_t>(index) == kDctEscape)
                {
                    uint32_t v;
                    if (!getBits(6, v))
                        return 1;
                    run = static_cast<int32_t>(v);
                    if (mpeg1)
                    {
                        if (!getBits(8, v))
                            return 1;
                        int32_t l = static_cast<int32_t>(v);
                        if (l == 0)
                        {
                            if (!getBits(8, v))
                                return 1;
                            l = static_cast<int32_t>(v);
                        }
                        else if (l == 0x80)
                        {
                            if (!getBits(8, v))
                                return 1;
                            l = static_cast<int32_t>(v) - 256;
                        }
                        else if (l > 0x80)
                        {
                            l -= 256;
                        }
                        negative = l < 0;
                        level = l < 0 ? -l : l;
                    }
                    else
                    {
                        if (!getBits(12, v))
                            return 1;
                        int32_t l = static_cast<int32_t>(v);
                        if (l >= 2048)
                            l -= 4096;
                        if (l == 0 || l == -2048)
                            return 2;
                        negative = l < 0;
                        level = l < 0 ? -l : l;
                    }
                }
                else
                {
                    run = kDctRun[index];
                    level = kDctLevel[index];
                    uint32_t s;
                    if (!getBits(1, s))
                        return 1;
                    negative = s != 0;
                }
            }
        have_coefficient:
            first = false;
            i += static_cast<uint32_t>(run);
            if (i > 63)
                return 2;
            const uint32_t pos = scan[i];
            int32_t value;
            if (intra)
            {
                value = (level * matrix[pos] * static_cast<int32_t>(quantiserScale)) >> 4;
                if (mpeg1 && value != 0)
                    value = (value - 1) | 1;
            }
            else
            {
                value = ((2 * level + 1) * matrix[pos] * static_cast<int32_t>(quantiserScale)) >> 5;
                if (mpeg1 && value != 0)
                    value = (value - 1) | 1;
            }
            if (negative)
                value = -value;
            block[pos] = clampInt(value, -2048, 2047);
            ++i;
            if (i > 63)
            {
                // The last coefficient was placed; an EOB must follow.
                first = false;
            }
        }

        if (!mpeg1)
        {
            int32_t sum = 0;
            for (int k = 0; k < 64; ++k)
                sum += block[k];
            if (!(sum & 1))
                block[63] ^= 1;
        }
        return 0;
    }

    // Decodes the blocks of one macroblock and lays the samples out as a 16x16 luma plane followed by the
    // 8x8 Cb and Cr planes. Returns 0 ok, 1 starved, 2 invalid.
    int Ipu::decodeMacroblock(bool intra, uint32_t cbp, bool fieldDct, uint32_t quantiserScale, int16_t *out)
    {
        std::memset(out, 0, 384 * sizeof(int16_t));
        for (uint32_t b = 0; b < 6; ++b)
        {
            if (!intra && !(cbp & (0x20u >> b)))
                continue;
            int32_t coef[64];
            int r = decodeBlock(b, intra, quantiserScale, coef);
            if (r != 0)
                return r;
            int16_t in[64], px[64];
            for (int k = 0; k < 64; ++k)
                in[k] = static_cast<int16_t>(coef[k]);
            inverseDct8x8(in, px);
            for (int k = 0; k < 64; ++k)
                px[k] = static_cast<int16_t>(intra ? clampInt(px[k], 0, 255) : clampInt(px[k], -256, 255));
            if (b < 4)
            {
                for (int row = 0; row < 8; ++row)
                {
                    const int line = fieldDct ? (row * 2 + static_cast<int>(b >> 1)) : ((static_cast<int>(b >> 1) * 8) + row);
                    std::memcpy(out + line * 16 + (b & 1u) * 8, px + row * 8, 8 * sizeof(int16_t));
                }
            }
            else
            {
                std::memcpy(out + 256 + (b - 4) * 64, px, 64 * sizeof(int16_t));
            }
        }
        return 0;
    }

    bool Ipu::stepBdec()
    {
        if (m_cmdPhase == 0 && !skipFirstBits())
            return false;
        if (!outputHasRoom())
            return false;
        if (m_cmdPhase == 2)
        {
            // Everything was produced; the command ends once the data fits in the OUT FIFO.
            finishCommand();
            return true;
        }
        const Tables &t = tables();
        const bool intra = (m_cmdOption & (1u << 27)) != 0;
        const bool fieldDct = (m_cmdOption & (1u << 25)) != 0;
        const bool resetDc = (m_cmdOption & (1u << 26)) != 0;
        const uint32_t qsc = (m_cmdOption >> 16) & 0x1Fu;
        const uint32_t qs = (m_ctrl & kCtrlQst) ? kNonLinearQscale[qsc] : (qsc << 1);

        beginAttempt();
        if (resetDc)
        {
            const int32_t v = 128 << ((m_ctrl >> kCtrlIdpShift) & 3u);
            m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = v;
        }
        uint32_t cbp = 0x3Fu;
        int r = 0;
        if (!intra)
        {
            int32_t v = 0;
            uint32_t len = 0;
            r = peekVlc(&t.cbp, v, len);
            if (r == 0)
            {
                skipBits(len);
                cbp = static_cast<uint32_t>(v);
            }
        }
        int16_t mb[384];
        if (r == 0)
            r = decodeMacroblock(intra, cbp, fieldDct, qs, mb);
        if (r == 1)
        {
            rollbackAttempt();
            return false;
        }
        commitAttempt();
        if (r == 2)
        {
            raiseEnd(false);
            finishCommand();
            return true;
        }
        m_cbp = cbp;
        if (ipuLogEnabled())
            (intra ? g_bdecIntra : g_bdecInter)++;
        uint8_t bytes[768];
        for (int i = 0; i < 384; ++i)
        {
            bytes[i * 2] = static_cast<uint8_t>(static_cast<uint16_t>(mb[i]) & 0xFF);
            bytes[i * 2 + 1] = static_cast<uint8_t>(static_cast<uint16_t>(mb[i]) >> 8);
        }
        appendOutput(bytes, sizeof(bytes));
        ++m_macroblockCount;
        m_cmdPhase = 2;
        if (!outputHasRoom())
            return true;
        finishCommand();
        return true;
    }

    bool Ipu::stepIdec()
    {
        if (m_cmdPhase == 0)
        {
            if (!skipFirstBits())
                return false;
            const int32_t v = 128 << ((m_ctrl >> kCtrlIdpShift) & 3u);
            m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = v;
            m_quantiserScaleCode = (m_cmdOption >> 16) & 0x1Fu;
            m_cmdPhase = 1;
        }
        const Tables &t = tables();
        bool progress = false;
        for (;;)
        {
            if (m_cmdPhase == 1)
            {
                // Decode one macroblock and convert it.
                if (!outputHasRoom())
                    return progress;
                beginAttempt();
                int32_t flags = 0;
                uint32_t len = 0;
                int r = peekVlc(&t.mbtI, flags, len);
                uint32_t qsc = m_quantiserScaleCode;
                bool fieldDct = false;
                int16_t mb[384];
                if (r == 0)
                {
                    skipBits(len);
                    if (!(flags & kMbIntra))
                    {
                        r = 2;
                    }
                }
                if (r == 0 && (m_cmdOption & (1u << 24)))
                {
                    uint32_t bit;
                    if (!getBits(1, bit))
                        r = 1;
                    else
                        fieldDct = bit != 0;
                }
                if (r == 0 && (flags & kMbQuant))
                {
                    uint32_t code;
                    if (!getBits(5, code))
                        r = 1;
                    else
                        qsc = code;
                }
                if (r == 0)
                {
                    const uint32_t qs = (m_ctrl & kCtrlQst) ? kNonLinearQscale[qsc] : (qsc << 1);
                    r = decodeMacroblock(true, 0x3Fu, fieldDct, qs, mb);
                }
                if (r == 1)
                {
                    rollbackAttempt();
                    return progress;
                }
                commitAttempt();
                if (r == 2)
                {
                    raiseEnd(false);
                    finishCommand();
                    return true;
                }
                m_quantiserScaleCode = qsc;
                m_cbp = 0x3Fu;
                uint8_t yuv[384];
                for (int i = 0; i < 384; ++i)
                    yuv[i] = static_cast<uint8_t>(clampInt(mb[i], 0, 255));
                uint8_t rgba[1024];
                yuv420ToRgb32(yuv, yuv + 256, yuv + 320, m_th0, m_th1, rgba);
                if (m_cmdOption & (1u << 25))
                {
                    for (int i = 0; i < 256; ++i)
                    {
                        rgba[i * 4] ^= 0x80;
                        rgba[i * 4 + 1] ^= 0x80;
                        rgba[i * 4 + 2] ^= 0x80;
                    }
                }
                if (m_cmdOption & (1u << 27))
                {
                    uint8_t out16[512];
                    rgb32ToRgb16(rgba, (m_cmdOption & (1u << 26)) != 0, out16);
                    appendOutput(out16, sizeof(out16));
                }
                else
                {
                    appendOutput(rgba, sizeof(rgba));
                }
                ++m_macroblockCount;
                m_cmdPhase = 2;
                progress = true;
            }

            // Phase 2: what follows the macroblock (next address increment or the end of the slice).
            if (!outputHasRoom())
                return progress;
            beginAttempt();
            uint32_t next;
            if (!peekBits(23, next))
            {
                rollbackAttempt();
                return progress;
            }
            if (next == 0)
            {
                // Slice end: align to a byte and look for the start code prefix.
                if (!skipBits((8u - (m_bp & 7u)) & 7u))
                {
                    rollbackAttempt();
                    return progress;
                }
                for (;;)
                {
                    uint32_t code;
                    if (!peekBits(24, code))
                    {
                        rollbackAttempt();
                        return progress;
                    }
                    if (code == 1)
                    {
                        raiseEnd(true);
                        break;
                    }
                    if (code == 0)
                    {
                        skipBits(8);
                        continue;
                    }
                    raiseEnd(false);
                    break;
                }
                commitAttempt();
                finishCommand();
                return true;
            }
            uint32_t increment = 0;
            bool bad = false;
            for (;;)
            {
                int32_t v = 0;
                uint32_t l = 0;
                int r = peekVlc(&t.mbai, v, l);
                if (r == 1)
                {
                    rollbackAttempt();
                    return progress;
                }
                if (r == 2)
                {
                    bad = true;
                    break;
                }
                skipBits(l);
                if (v == 35)
                {
                    increment += 33;
                    continue;
                }
                if (v == 34)
                    continue;
                increment += static_cast<uint32_t>(v);
                break;
            }
            commitAttempt();
            if (bad)
            {
                raiseEnd(false);
                finishCommand();
                return true;
            }
            if (increment != 1)
            {
                const int32_t v = 128 << ((m_ctrl >> kCtrlIdpShift) & 3u);
                m_dcPredictor[0] = m_dcPredictor[1] = m_dcPredictor[2] = v;
            }
            m_cmdPhase = 1;
        }
    }
} // namespace ps2
