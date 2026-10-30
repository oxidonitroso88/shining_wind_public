// Software rasteriser regression tests: deterministic synthetic scenes covering sprites, triangles, texture formats, CLUT, filters,
// wrap modes, alpha blending/testing, depth, masks, DATE, fog and large (band-parallel) primitives. Each scene's final VRAM is hashed
// and compared with a golden value recorded from the reference (pre-optimisation) rasteriser, both sequentially and with helper threads,
// so a rasteriser optimisation can never change a single pixel unnoticed.
//
//   ps2_gs_raster_tests            run the checks (exit code 0 = all scenes match)
//   GS_RASTER_PRINT=1 ps2_gs_raster_tests   print the computed hashes (to regenerate the goldens deliberately)
#include "runtime/gs/gs_cpu_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{
    struct Rng
    {
        uint64_t s = 0x9E3779B97F4A7C15ull;
        uint32_t next()
        {
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            return static_cast<uint32_t>(s >> 11);
        }
        float uni() { return static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f; }
        float range(float lo, float hi) { return lo + (hi - lo) * uni(); }
        int irange(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
    };

    uint64_t fnv(const uint8_t *d, size_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= d[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    constexpr uint32_t kVram = 4u * 1024u * 1024u;
    constexpr uint32_t kFbp = 0;      // frame base (pages)
    constexpr uint32_t kFbw = 10;     // 640 pixels
    constexpr uint32_t kZbp = 160;    // z base (pages)
    constexpr uint32_t kTbp = 0x2800; // texture base (blocks)
    constexpr uint32_t kCbp = 0x3E00; // CLUT base (blocks)

    struct Scene
    {
        std::vector<uint8_t> vram;
        GSCpuBackend be;
        Rng rng;
        GSDrawState st;

        explicit Scene(uint64_t seed) : vram(kVram)
        {
            rng.s ^= seed * 0xD6E8FEB86659FD93ull + 1;
            for (auto &b : vram)
                b = static_cast<uint8_t>(rng.next());
            be.Initialize(vram.data(), kVram);
            be.Reset();
            st = base();
        }

        static GSDrawState base()
        {
            GSDrawState s;
            s.context.frame.fbp = kFbp;
            s.context.frame.fbw = kFbw;
            s.context.frame.psm = GS_PSM_CT32;
            s.context.frame.fbmsk = 0;
            s.context.scissor = {0, 639, 0, 447};
            s.context.zbuf.zbp = kZbp;
            s.context.zbuf.psm = GS_PSM_Z32;
            s.context.zbuf.zmask = false;
            s.context.test = 0x30000;      // alpha test off, ZTEST ALWAYS (ZTE=1, ZTST=1 at bits 16..18)
            s.context.alpha = 0x44;        // (Cs - Cd) * As + Cd
            s.context.tex0.tbp0 = kTbp;
            s.context.tex0.tbw = 4;
            s.context.tex0.psm = GS_PSM_CT32;
            s.context.tex0.tw = 7;
            s.context.tex0.th = 7;
            s.context.tex0.tcc = 1;
            s.context.tex0.tfx = 0;
            s.context.tex0.cbp = kCbp;
            s.context.tex0.cpsm = GS_PSM_CT32;
            s.context.tex0.csm = 0;
            s.context.tex0.csa = 0;
            s.context.tex0.cld = 1;
            s.context.clamp = 0;
            s.textureWidth = 128;
            s.textureHeight = 128;
            s.texa.ta0 = 0x40;
            s.texa.ta1 = 0x80;
            s.texa.aem = false;
            return s;
        }

        void setTexture(uint8_t psm, uint8_t tw, uint8_t th, uint8_t cpsm = GS_PSM_CT32, uint8_t csa = 0, uint8_t tbw = 4)
        {
            st.context.tex0.psm = psm;
            st.context.tex0.tw = tw;
            st.context.tex0.th = th;
            st.context.tex0.cpsm = cpsm;
            st.context.tex0.csa = csa;
            st.context.tex0.tbw = tbw;
            st.textureWidth = static_cast<uint16_t>(1u << tw);
            st.textureHeight = static_cast<uint16_t>(1u << th);
            if (psm == GS_PSM_T4 || psm == GS_PSM_T8 || psm == GS_PSM_T4HL || psm == GS_PSM_T4HH || psm == GS_PSM_T8H)
                be.LoadClut(st.context.tex0, st.texclut);
        }

        GSVertex vtx(float x, float y, float z, bool textured)
        {
            GSVertex v;
            v.x = x;
            v.y = y;
            v.z = z;
            v.r = static_cast<uint8_t>(rng.next());
            v.g = static_cast<uint8_t>(rng.next());
            v.b = static_cast<uint8_t>(rng.next());
            v.a = static_cast<uint8_t>(rng.next());
            v.fog = static_cast<uint8_t>(rng.next());
            if (textured)
            {
                const float tw = static_cast<float>(st.textureWidth), th = static_cast<float>(st.textureHeight);
                v.u = static_cast<uint16_t>(rng.range(-0.3f * tw, 1.3f * tw) * 16.0f);
                v.v = static_cast<uint16_t>(rng.range(-0.3f * th, 1.3f * th) * 16.0f);
                v.q = rng.range(0.5f, 2.0f);
                v.s = rng.range(-0.3f, 1.3f) * v.q;
                v.t = rng.range(-0.3f, 1.3f) * v.q;
            }
            return v;
        }

        void sprite(float minSize, float maxSize, bool textured)
        {
            const float w = rng.range(minSize, maxSize), h = rng.range(minSize, maxSize);
            const float x = rng.range(-30.0f, 640.0f - w * 0.5f), y = rng.range(-30.0f, 448.0f - h * 0.5f);
            GSPrimitiveBatch b;
            b.state = st;
            b.state.prim.type = GS_PRIM_SPRITE;
            b.state.prim.tme = textured;
            b.vertices[0] = vtx(x, y, static_cast<float>(rng.next() & 0xFFFFFF), textured);
            b.vertices[1] = vtx(x + w, y + h, static_cast<float>(rng.next() & 0xFFFFFF), textured);
            b.vertexCount = 2;
            be.Submit(b);
        }

        void triangle(float minSize, float maxSize, bool textured, GSPrimType type = GS_PRIM_TRIANGLE)
        {
            const float cx = rng.range(0.0f, 640.0f), cy = rng.range(0.0f, 448.0f);
            GSPrimitiveBatch b;
            b.state = st;
            b.state.prim.type = type;
            b.state.prim.tme = textured;
            for (int i = 0; i < 3; ++i)
                b.vertices[i] = vtx(cx + rng.range(-maxSize, maxSize) + (i == 0 ? minSize : 0.0f), cy + rng.range(-maxSize, maxSize),
                                    static_cast<float>(rng.next() & 0xFFFFFF), textured);
            b.vertexCount = 3;
            be.Submit(b);
        }

        uint64_t hash()
        {
            std::vector<uint8_t> out;
            be.SnapshotVram(out);
            return fnv(out.data(), out.size());
        }
    };

    struct Case
    {
        const char *name;
        std::function<void(Scene &)> run;
        uint64_t golden;
    };

    // A fixed order matters: the PRNG is consumed deterministically.
    std::vector<Case> buildCases()
    {
        std::vector<Case> c;
        c.push_back({"sprite_flat_noblend_ct32", [](Scene &s) { for (int i = 0; i < 60; ++i) s.sprite(4, 200, false); }, 0});
        c.push_back({"sprite_flat_blend_variants", [](Scene &s)
        {
            const uint64_t alphas[] = {0x44, 0x48, 0x41, 0x4A, 0x0000002000000044ull | 0x20, 0x0000008000000064ull, 0x00000040000000A4ull};
            for (uint64_t a : alphas)
                for (int pabe = 0; pabe < 2; ++pabe)
                {
                    s.st.context.alpha = a;
                    s.st.pabe = pabe != 0;
                    s.st.prim.abe = true;
                    for (int i = 0; i < 12; ++i)
                        s.sprite(6, 150, false);
                }
        }, 0});
        c.push_back({"sprite_flat_fbmsk_ct16_ct24", [](Scene &s)
        {
            const uint8_t psms[] = {GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_CT24, GS_PSM_CT32};
            for (uint8_t psm : psms)
            {
                s.st.context.frame.psm = psm;
                s.st.context.frame.fbmsk = (psm == GS_PSM_CT32) ? 0x00FF00FFu : 0u;
                for (int abe = 0; abe < 2; ++abe)
                {
                    s.st.prim.abe = abe != 0;
                    for (int i = 0; i < 10; ++i)
                        s.sprite(6, 160, false);
                }
            }
        }, 0});
        c.push_back({"sprite_tex_formats_tfx", [](Scene &s)
        {
            const uint8_t psms[] = {GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_T8, GS_PSM_T4, GS_PSM_T8H, GS_PSM_T4HL, GS_PSM_T4HH};
            for (uint8_t psm : psms)
                for (uint8_t tfx = 0; tfx < 4; ++tfx)
                {
                    s.setTexture(psm, 6, 6, (tfx & 1) ? GS_PSM_CT16 : GS_PSM_CT32, static_cast<uint8_t>(tfx * 3));
                    s.st.context.tex0.tfx = tfx;
                    s.st.context.tex0.tcc = (tfx & 2) ? 1 : 0;
                    s.st.texa.aem = (tfx == 1);
                    s.st.prim.fst = true;
                    s.st.prim.abe = (tfx != 3);
                    s.st.linearFilter = (psm & 1) != 0;
                    for (int i = 0; i < 4; ++i)
                        s.sprite(10, 120, true);
                }
        }, 0});
        c.push_back({"sprite_tex_wrap_modes_stq", [](Scene &s)
        {
            for (uint64_t mode = 0; mode < 4; ++mode)
                for (uint64_t modeV = 0; modeV < 4; modeV += 3)
                    for (int fst = 0; fst < 2; ++fst)
                    {
                        s.setTexture(GS_PSM_T8, 5, 5, GS_PSM_CT32, 0, 2);
                        s.st.context.clamp = mode | (modeV << 2) | (4ull << 4) | (20ull << 14) | (3ull << 24) | (25ull << 34);
                        s.st.prim.fst = fst != 0;
                        s.st.prim.abe = true;
                        s.st.linearFilter = (mode & 1) != 0;
                        for (int i = 0; i < 4; ++i)
                            s.sprite(10, 140, true);
                    }
        }, 0});
        c.push_back({"tri_gouraud_ztest_zmask", [](Scene &s)
        {
            const uint8_t zpsms[] = {GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16, GS_PSM_Z16S};
            for (uint8_t zpsm : zpsms)
                for (uint64_t zt = 0; zt < 4; ++zt)
                    for (int zmask = 0; zmask < 2; ++zmask)
                    {
                        s.st.context.zbuf.psm = zpsm;
                        s.st.context.zbuf.zmask = zmask != 0;
                        s.st.context.test = 0x10000 | (zt << 17);
                        s.st.prim.iip = (zt & 1) != 0;
                        s.st.prim.abe = (zt >= 2);
                        for (int i = 0; i < 6; ++i)
                            s.triangle(0, 120, false);
                    }
        }, 0});
        c.push_back({"tri_alpha_test_afail", [](Scene &s)
        {
            for (uint64_t atst = 0; atst < 8; ++atst)
                for (uint64_t afail = 0; afail < 4; ++afail)
                {
                    const uint8_t psm = (afail & 1) ? GS_PSM_CT32 : GS_PSM_CT16;
                    s.st.context.frame.psm = psm;
                    s.st.context.test = 1ull | (atst << 1) | (0x70ull << 4) | (afail << 12) | (1ull << 16) | (3ull << 17);
                    s.st.prim.iip = true;
                    s.st.prim.abe = (atst & 1) != 0;
                    for (int i = 0; i < 5; ++i)
                        s.triangle(0, 110, false);
                }
        }, 0});
        c.push_back({"tri_textured_t4_linear_blend_game_like", [](Scene &s)
        {
            s.setTexture(GS_PSM_T4, 7, 7, GS_PSM_CT32, 0);
            s.st.context.test = 0x0003000B; // ATE, GEQUAL aref 0, ZTEST ALWAYS
            s.st.prim.iip = true;
            s.st.prim.fst = true;
            s.st.prim.abe = true;
            s.st.linearFilter = true;
            s.st.context.tex0.tfx = 0;
            s.st.context.tex0.tcc = 1;
            for (int i = 0; i < 60; ++i)
                s.triangle(0, 160, true, i % 2 ? GS_PRIM_TRISTRIP : GS_PRIM_TRIFAN);
        }, 0});
        c.push_back({"tri_fog_date", [](Scene &s)
        {
            for (int date = 0; date < 2; ++date)
                for (int fge = 0; fge < 2; ++fge)
                    for (int datm = 0; datm < 2; ++datm)
                    {
                        s.st.context.test = 0x30000 | (date ? (1u << 14) : 0u) | (datm ? (1u << 15) : 0u);
                        s.st.prim.fge = fge != 0;
                        s.st.prim.abe = datm != 0;
                        s.st.fogR = 0x30;
                        s.st.fogG = 0x90;
                        s.st.fogB = 0xD0;
                        s.st.context.frame.psm = date ? GS_PSM_CT16 : GS_PSM_CT32;
                        for (int i = 0; i < 6; ++i)
                            s.triangle(0, 130, false);
                    }
        }, 0});
        c.push_back({"large_primitives_band_parallel", [](Scene &s)
        {
            s.setTexture(GS_PSM_T4, 7, 7, GS_PSM_CT32, 0);
            s.st.context.test = 0x0003000B;
            s.st.prim.iip = true;
            s.st.prim.fst = true;
            s.st.prim.abe = true;
            s.st.linearFilter = true;
            for (int i = 0; i < 14; ++i)
                s.triangle(0, 420, true);
            s.setTexture(GS_PSM_CT16, 6, 6);
            s.st.linearFilter = false;
            s.st.context.tex0.tfx = 0;
            for (int i = 0; i < 10; ++i)
                s.sprite(300, 620, true);
            s.st.prim.abe = false;
            for (int i = 0; i < 4; ++i)
                s.sprite(400, 640, false);
        }, 0});
        c.push_back({"clut_reload_between_primitives", [](Scene &s)
        {
            for (int round = 0; round < 6; ++round)
            {
                s.setTexture(round % 2 ? GS_PSM_T4 : GS_PSM_T8, 6, 6, round % 3 ? GS_PSM_CT32 : GS_PSM_CT16, static_cast<uint8_t>(round));
                s.st.texa.ta0 = static_cast<uint8_t>(0x10 * round);
                s.st.prim.fst = true;
                s.st.prim.abe = true;
                for (int i = 0; i < 6; ++i)
                    s.sprite(20, 140, true);
                for (int i = 0; i < 3; ++i)
                    s.triangle(0, 100, true);
                // overwrite the CLUT memory and reload it
                for (uint32_t i = 0; i < 256; ++i)
                    s.be.WriteVram(GS_PSM_CT32, kCbp, 1, i & 15, i >> 4, s.rng.next());
            }
        }, 0});
        return c;
    }
}

int main()
{
    const bool print = std::getenv("GS_RASTER_PRINT") != nullptr;
    // Golden hashes recorded from the reference rasteriser (see gs_raster_goldens.inc); regenerated deliberately with GS_RASTER_PRINT=1.
    static const uint64_t goldens[] = {
#include "gs_raster_goldens.inc"
    };
    std::vector<Case> cases = buildCases();
    int failures = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        uint64_t hashes[2] = {0, 0};
        const char *modes[2] = {"0", "4"}; // sequential, then band-parallel with 4 helper threads
        for (int m = 0; m < 2; ++m)
        {
#ifdef _WIN32
            _putenv_s("PS2X_GS_THREADS", modes[m]);
#else
            setenv("PS2X_GS_THREADS", modes[m], 1);
#endif
            Scene scene(static_cast<uint64_t>(i) + 1);
            cases[i].run(scene);
            hashes[m] = scene.hash();
        }
        const uint64_t expected = i < sizeof(goldens) / sizeof(goldens[0]) ? goldens[i] : 0;
        if (print)
            std::printf("    0x%016llxull, // %s\n", static_cast<unsigned long long>(hashes[0]), cases[i].name);
        const bool same = hashes[0] == hashes[1];
        const bool golden = print || hashes[0] == expected;
        std::printf("[%s] %-42s seq=%016llx par=%016llx%s\n", (same && golden) ? "PASS" : "FAIL", cases[i].name,
                    static_cast<unsigned long long>(hashes[0]), static_cast<unsigned long long>(hashes[1]),
                    same ? (golden ? "" : "  (differs from golden)") : "  (parallel differs from sequential)");
        failures += (same && golden) ? 0 : 1;
    }
    std::printf("%s: %zu scenes, %d failures\n", failures ? "FAILED" : "OK", cases.size(), failures);
    return failures ? 1 : 0;
}
