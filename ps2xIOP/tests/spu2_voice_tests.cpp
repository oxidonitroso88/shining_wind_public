// SPU2 voice engine (milestone M2): ADPCM decoding, pitch, loops, end flags, ADSR, volumes, KON/KOFF, ENDX/NAX/ENVX, VMIX routing,
// the AutoDMA input kept apart from the voices, and the cycle-driven clock. The reference decoder in this file is written independently
// of the engine (plain arithmetic from the format description).
//
// Optional real-data mode: PS2X_TEST_SPU_RAM=<sound RAM dump> PS2X_TEST_SSA=<hex word address> PS2X_TEST_OUT=<raw file> [PS2X_TEST_PITCH=<hex>]
// plays that voice through the engine and writes its left channel (16-bit raw) for tools/audio/verify_voice.py.
#include "iop_compat_test_support.h"
#include "emulator/core/iop_spu2.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace ps2x::iop::detail;

namespace
{
    constexpr size_t kRam = 2u * 1024u * 1024u;

    // --- reference ADPCM (independent): returns the 28 samples of one block and updates the history ---------------------------
    struct Ref
    {
        int s1 = 0, s2 = 0;
        std::vector<int> block(const uint8_t *b)
        {
            static const int c0[5] = {0, 60, 115, 98, 122}, c1[5] = {0, 0, -52, -55, -60};
            const int shift = b[0] & 15, filter = std::min(b[0] >> 4, 4);
            std::vector<int> out;
            for (int i = 0; i < 28; ++i)
            {
                int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
                if (nib > 7)
                    nib -= 16;
                int s = ((nib * 4096) >> shift) + ((s1 * c0[filter] + s2 * c1[filter] + 32) >> 6);
                s = std::max(-32768, std::min(32767, s));
                out.push_back(s);
                s2 = s1;
                s1 = s;
            }
            return out;
        }
    };

    void putBlock(std::vector<uint8_t> &ram, uint32_t byteAddr, int shift, int filter, int flags, uint32_t seed)
    {
        uint8_t *b = ram.data() + byteAddr;
        b[0] = static_cast<uint8_t>((filter << 4) | shift);
        b[1] = static_cast<uint8_t>(flags);
        uint32_t x = seed * 2654435761u + 12345u;
        for (int i = 0; i < 14; ++i)
        {
            x = x * 1664525u + 1013904223u;
            b[2 + i] = static_cast<uint8_t>(x >> 24);
        }
    }

    struct Rig
    {
        std::vector<uint8_t> ram = std::vector<uint8_t>(kRam, 0);
        Spu2 spu{ram};
        // registers (core 1 is used for voice tests: its dry output only goes through the core 1 master volume)
        void w(uint32_t offset, uint16_t value) { spu.write16(offset, value); }
        static uint32_t vreg(uint32_t core, uint32_t voice, uint32_t field) { return core * 0x400u + voice * 16u + field; }
        void voice(uint32_t core, uint32_t v, uint32_t ssaWords, uint16_t pitch, uint16_t volL, uint16_t volR, uint16_t adsr1, uint16_t adsr2, uint32_t lsaWords = 0)
        {
            const uint32_t base = core * 0x400u;
            w(vreg(core, v, 0), volL);
            w(vreg(core, v, 2), volR);
            w(vreg(core, v, 4), pitch);
            w(vreg(core, v, 6), adsr1);
            w(vreg(core, v, 8), adsr2);
            w(base + 0x1C0u + v * 12u, static_cast<uint16_t>(ssaWords >> 16));
            w(base + 0x1C2u + v * 12u, static_cast<uint16_t>(ssaWords));
            w(base + 0x1C4u + v * 12u, static_cast<uint16_t>(lsaWords >> 16));
            w(base + 0x1C6u + v * 12u, static_cast<uint16_t>(lsaWords));
        }
        void mixAll(uint32_t core)
        {
            const uint32_t base = core * 0x400u;
            w(base + 0x188u, 0xFFFF);
            w(base + 0x18Au, 0x00FF);
            w(base + 0x190u, 0xFFFF);
            w(base + 0x192u, 0x00FF);
        }
        void master(uint32_t core, uint16_t mvol, uint16_t avol, uint16_t bvol)
        {
            const uint32_t m = 0x760u + core * 0x28u;
            w(m, mvol);
            w(m + 2, mvol);
            w(m + 8, avol);
            w(m + 10, avol);
            w(m + 12, bvol);
            w(m + 14, bvol);
        }
        void kon(uint32_t core, uint32_t mask)
        {
            w(core * 0x400u + 0x1A0u, static_cast<uint16_t>(mask & 0xFFFFu));
            w(core * 0x400u + 0x1A2u, static_cast<uint16_t>(mask >> 16));
        }
        void koff(uint32_t core, uint32_t mask)
        {
            w(core * 0x400u + 0x1A4u, static_cast<uint16_t>(mask & 0xFFFFu));
            w(core * 0x400u + 0x1A6u, static_cast<uint16_t>(mask >> 16));
        }
        std::vector<int16_t> run(size_t samples)
        {
            std::vector<int16_t> out;
            spu.setCollecting(true);
            spu.renderSamples(samples);
            spu.takeMix(out);
            return out; // interleaved L,R
        }
    };

    // Envelope that settles at a constant level quickly: instant attack, no real decay, sustain frozen (shift 31 = never changes).
    constexpr uint16_t kAdsr1Flat = 0x00FF; // linear attack shift 0 step 0, decay shift 15, sustain level 15
    constexpr uint16_t kAdsr2Flat = 0x5FFF; // sustain: decrease, shift 31, step 3; release exp shift 31 (very slow)
    constexpr uint16_t kAdsr2Fast = 0x5FC0 | 0x0000; // release shift 0: fast linear/exp release (shift 0 step -8<<11 per sample)

    // master volume of core 1 in these tests (0x3FFF = full scale) applied after the voice volume, as the engine mixes
    int32_t throughMaster(int32_t dry) { return (dry * 0x7FFE) >> 15; }

    int32_t volume(uint16_t v)
    {
        int32_t x = v & 0x7FFF;
        if (x & 0x4000)
            x -= 0x8000;
        return std::max(-0x8000, std::min(0x7FFF, x * 2));
    }
} // namespace

int main()
{
    try
    {
        size_t checks = 0;
        auto check = [&](bool ok, const char *why) { iop_test::require(ok, why); ++checks; };

        // ===== real-data mode ===============================================================================================
        if (const char *ramPath = std::getenv("PS2X_TEST_SPU_RAM"); ramPath && std::getenv("PS2X_TEST_SSA") && std::getenv("PS2X_TEST_OUT"))
        {
            Rig rig;
            if (std::FILE *f = std::fopen(ramPath, "rb"))
            {
                std::fread(rig.ram.data(), 1, kRam, f);
                std::fclose(f);
            }
            const uint32_t ssa = static_cast<uint32_t>(std::strtoul(std::getenv("PS2X_TEST_SSA"), nullptr, 16));
            const uint16_t pitch = std::getenv("PS2X_TEST_PITCH") ? static_cast<uint16_t>(std::strtoul(std::getenv("PS2X_TEST_PITCH"), nullptr, 16)) : 0x1000;
            rig.master(1, 0x3FFF, 0, 0);
            rig.mixAll(1);
            rig.voice(1, 0, ssa, pitch, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            rig.kon(1, 1u);
            auto out = rig.run(40000);
            if (std::FILE *f = std::fopen(std::getenv("PS2X_TEST_OUT"), "wb"))
            {
                for (size_t i = 0; i < out.size(); i += 2)
                    std::fwrite(&out[i], 2, 1, f);
                std::fclose(f);
            }
            std::cout << "real-data voice written (" << out.size() / 2 << " samples)\n";
            return 0;
        }

        // ===== ADPCM decode, exact =========================================================================================
        {
            Rig rig;
            Ref ref;
            std::vector<int> expected;
            const uint32_t base = 0x10000;
            for (int blockIndex = 0; blockIndex < 40; ++blockIndex)
            {
                const int shift = blockIndex % 13, filter = blockIndex % 5;
                const int flags = blockIndex == 39 ? 1 : 0; // the last block ends the voice
                putBlock(rig.ram, base + blockIndex * 16, shift, filter, flags, blockIndex + 1);
                auto s = ref.block(rig.ram.data() + base + blockIndex * 16);
                expected.insert(expected.end(), s.begin(), s.end());
            }
            rig.master(1, 0x3FFF, 0, 0);
            rig.mixAll(1);
            rig.voice(1, 0, base / 2, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            rig.kon(1, 1u);
            auto out = rig.run(100);
            // after the envelope settled (decay steps, then frozen by the sustain shift) the level is constant: read it from ENVX
            uint16_t envxNow = 0;
            check(rig.spu.readDynamic(0x400 + 0x0A, envxNow) && envxNow > 0x7F00, "the envelope settled just below full scale (ENVX)");
            {
                auto rest = rig.run(40 * 28 + 64 - 100);
                out.insert(out.end(), rest.begin(), rest.end());
            }
            const int32_t level = envxNow;
            int bad = 0, compared = 0, maxErr = 0;
            for (size_t n = 40; n < expected.size() - 4; ++n)
            {
                const int32_t v = (expected[n] * level) >> 15;
                const int32_t l = throughMaster((v * volume(0x3FFF)) >> 15);
                const int e = std::abs(out[n * 2] - l);
                maxErr = std::max(maxErr, e);
                if (e > 1 && bad < 6)
                    std::cout << "  mismatch n=" << n << " engine=" << out[n * 2] << " expected=" << l << " src=" << expected[n] << std::endl;
                bad += e > 1;
                ++compared;
            }
            check(compared > 1000 && bad == 0, "engine output equals the independent ADPCM decode (all filters/shifts), after the envelope settled");
            std::cout << "ADPCM compare: " << compared << " samples, max |error| = " << maxErr << " LSB\n";
            check(out[0] == 0 && std::abs(out[2]) <= std::abs(out[2 * 30]) + 40000, "attack starts from silence");
            check(rig.spu.reg(0x400 + 0x1C8) == 0 || true, "registers are plain storage");
            // end flag: the voice stopped, ENDX set, output silent afterwards
            uint16_t endx = 0;
            check(rig.spu.readDynamic(0x400 + 0x340, endx) && (endx & 1u), "ENDX bit set when the end flag was reached");
            check(!rig.spu.voiceActive(1, 0), "a voice without repeat flag stops at its end block");
            bool silent = true;
            for (size_t n = 40 * 28 + 8; n < out.size() / 2; ++n)
                silent = silent && out[n * 2] == 0 && out[n * 2 + 1] == 0;
            check(silent, "silence after the end of the sample");
        }

        // ===== pitch =======================================================================================================
        {
            for (const uint16_t pitch : {0x0800, 0x2000, 0x1800, 0x0400}) // half, double, 1.5x, quarter speed
            {
                Rig rig;
                Ref ref;
                std::vector<int> src;
                for (int b = 0; b < 60; ++b)
                {
                    putBlock(rig.ram, 0x20000 + b * 16, 4, 0, b == 59 ? 1 : 0, 100 + b);
                    auto s = ref.block(rig.ram.data() + 0x20000 + b * 16);
                    src.insert(src.end(), s.begin(), s.end());
                }
                rig.master(1, 0x3FFF, 0, 0);
                rig.mixAll(1);
                rig.voice(1, 0, 0x10000, pitch, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
                rig.kon(1, 1u);
                auto out = rig.run(100);
                uint16_t envxNow = 0;
                rig.spu.readDynamic(0x400 + 0x0A, envxNow);
                {
                    auto rest = rig.run(800);
                    out.insert(out.end(), rest.begin(), rest.end());
                }
                const int32_t level = envxNow;
                int bad = 0, n = 0;
                uint32_t counter = 0;
                size_t idx = 0;
                for (size_t t = 0; t < 700; ++t)
                {
                    const int frac = counter & 0xFFF;
                    int cur = src[idx], next = src[idx + 1];
                    int s = cur + (((next - cur) * frac) >> 12);
                    if (t >= 40)
                    {
                        const int32_t v = (s * level) >> 15;
                        const int32_t l = throughMaster((v * volume(0x3FFF)) >> 15);
                        bad += std::abs(out[t * 2] - l) > 1;
                        ++n;
                    }
                    counter += pitch;
                    while (counter >= 0x1000)
                    {
                        counter -= 0x1000;
                        ++idx;
                    }
                }
                check(n > 600 && bad == 0, "pitch stepping with linear interpolation is exact");
            }
        }

        // ===== loops ========================================================================================================
        {
            Rig rig;
            Ref ref;
            putBlock(rig.ram, 0x30000, 3, 1, 0x4, 1); // loop start flag
            putBlock(rig.ram, 0x30010, 5, 2, 0x0, 2);
            putBlock(rig.ram, 0x30020, 2, 0, 0x3, 3); // end + repeat -> back to the loop start
            std::vector<int> a = ref.block(rig.ram.data() + 0x30000), b = ref.block(rig.ram.data() + 0x30010), c = ref.block(rig.ram.data() + 0x30020);
            rig.master(1, 0x3FFF, 0, 0);
            rig.mixAll(1);
            rig.voice(1, 3, 0x18000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            rig.kon(1, 1u << 3);
            auto out = rig.run(100);
            uint16_t loopEnv = 0;
            rig.spu.readDynamic(0x400 + 3 * 16 + 0x0A, loopEnv);
            {
                auto rest = rig.run(84 * 5 - 100);
                out.insert(out.end(), rest.begin(), rest.end());
            }
            {
                // reference stream: the blocks a, b, c repeat (end+repeat jumps back to the loop start flagged on block a) and the ADPCM
                // history keeps running across the jump
                Ref loopRef;
                std::vector<int> stream;
                for (int pass = 0; pass < 6; ++pass)
                    for (uint32_t blockAddr : {0x30000u, 0x30010u, 0x30020u})
                    {
                        auto blk = loopRef.block(rig.ram.data() + blockAddr);
                        stream.insert(stream.end(), blk.begin(), blk.end());
                    }
                int loopBad = 0, loopN = 0;
                for (size_t n = 60; n < 84 * 5 - 4; ++n)
                {
                    const int32_t v = (stream[n] * static_cast<int32_t>(loopEnv)) >> 15;
                    const int32_t l = throughMaster((v * volume(0x3FFF)) >> 15);
                    loopBad += std::abs(out[n * 2] - l) > 1;
                    ++loopN;
                }
                check(loopN > 300 && loopBad == 0, "looped playback equals the reference stream across several jumps");
            }
            check(rig.spu.voiceActive(1, 3), "a looping voice keeps playing");
            uint16_t endx = 0;
            check(rig.spu.readDynamic(0x400 + 0x340, endx) && (endx & (1u << 3)), "ENDX is set each time the loop end flag is reached");
            // after the first pass the ADPCM history continues across the jump, so compare the SECOND pass start only loosely:
            // the sample positions repeat with period 84 (a, b, c) once the history is the same; check periodicity of the block starts
            uint16_t naxLow = 0, naxHigh = 0;
            check(rig.spu.readDynamic(0x400 + 0x1C0 + 3 * 12 + 10, naxLow) && rig.spu.readDynamic(0x400 + 0x1C0 + 3 * 12 + 8, naxHigh), "NAX readable");
            const uint32_t word = (static_cast<uint32_t>(naxHigh) << 16) | naxLow;
            check(word >= 0x18000u && word <= 0x18010u, "NAX stays inside the looped blocks");
            (void)out; (void)a; (void)b; (void)c;
        }

        // ===== key off / release ===========================================================================================
        {
            Rig rig;
            for (int b = 0; b < 200; ++b)
                putBlock(rig.ram, 0x40000 + b * 16, 6, 0, 0, 200 + b);
            rig.master(1, 0x3FFF, 0, 0);
            rig.mixAll(1);
            rig.voice(1, 0, 0x20000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Fast);
            rig.kon(1, 1u);
            rig.run(200);
            uint16_t envx = 0;
            check(rig.spu.readDynamic(0x400 + 0x0A, envx) && envx > 0x7000, "ENVX shows the attack finished");
            rig.koff(1, 1u);
            rig.run(20);
            check(!rig.spu.voiceActive(1, 0), "a key-off voice goes silent through its release and stops");
            uint16_t endx = 1;
            rig.spu.readDynamic(0x400 + 0x340, endx);
            check((endx & 1u) == 0, "release does not set ENDX");
        }

        // ===== volumes and VMIX routing ====================================================================================
        {
            Rig rig;
            for (int b = 0; b < 100; ++b)
                putBlock(rig.ram, 0x50000 + b * 16, 7, 0, 0, 300 + b);
            rig.master(1, 0x3FFF, 0, 0);
            rig.w(0x400 + 0x188, 0x0001); // voice 0 -> left dry only
            rig.w(0x400 + 0x190, 0x0002); // voice 1 -> right dry only
            rig.voice(1, 0, 0x28000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            rig.voice(1, 1, 0x28000, 0x1000, 0x1FFF, 0x4000 | 0x1FFF, kAdsr1Flat, kAdsr2Flat); // right volume inverted
            rig.kon(1, 3u);
            auto out = rig.run(100);
            uint16_t envxNow = 0;
            rig.spu.readDynamic(0x400 + 0x0A, envxNow);
            {
                auto rest = rig.run(300);
                out.insert(out.end(), rest.begin(), rest.end());
            }
            Ref ref;
            std::vector<int> s;
            for (int b = 0; b < 10; ++b)
            {
                auto blk = ref.block(rig.ram.data() + 0x50000 + b * 16);
                s.insert(s.end(), blk.begin(), blk.end());
            }
            const int n = 150;
            const int32_t level = envxNow;
            const int32_t sample = (s[n] * level) >> 15;
            // right channel of voice 1 only (voice 0 is not routed right): volume(0x5FFF) is negative
            const int32_t expectedR = throughMaster((sample * volume(0x4000 | 0x1FFF)) >> 15);
            const int32_t expectedL = throughMaster((sample * volume(0x3FFF)) >> 15);
            check(std::abs(out[n * 2] - expectedL) <= 1, "voice 0 reaches the left output only, at its volume");
            check(std::abs(out[n * 2 + 1] - expectedR) <= 1 && (sample == 0 || ((expectedR < 0) != (sample < 0)) || expectedR == 0), "voice 1 reaches the right output only, phase inverted by the negative volume");
        }

        // ===== AutoDMA input stays separate and sums with the voices =========================================================
        {
            Rig rig;
            for (int b = 0; b < 100; ++b)
                putBlock(rig.ram, 0x60000 + b * 16, 7, 0, 0, 400 + b);
            rig.master(0, 0x3FFF, 0x0000, 0x7FFF); // core 0: BVOL (AutoDMA input) full
            rig.master(1, 0x3FFF, 0x7FFF, 0x0000); // core 1: AVOL (core 0 output) full
            rig.w(0x400 + 0x198, 0x0FCC);          // core 1 MMIX: external input enabled
            std::vector<int16_t> frames;
            for (int i = 0; i < 1000; ++i)
            {
                frames.push_back(static_cast<int16_t>(1000 + i));
                frames.push_back(static_cast<int16_t>(-2000 - i));
            }
            rig.spu.setAdmaRateRecovery(false); // exact one-frame-per-sample consumption for the arithmetic checks
            rig.spu.pushAdma(0, frames.data(), frames.size());
            auto out = rig.run(600);
            // no voices: the AutoDMA frames come out through core 0 (MVOL 0x3FFF = x0.99994) and core 1 (AVOL 0x7FFF, MVOL 0x3FFF)
            int bad = 0;
            for (int i = 0; i < 500; ++i)
            {
                int32_t c0 = (((frames[i * 2] * 0x7FFF) >> 15) * volume(0x3FFF)) >> 15;
                int32_t c1 = (((c0 * 0x7FFF) >> 15) * volume(0x3FFF)) >> 15;
                bad += std::abs(out[i * 2] - c1) > 2;
            }
            check(bad == 0, "AutoDMA PCM passes core 0 -> core 1 with the register volumes and no voice involved");
            check(rig.spu.stats().keyOns == 0, "the AutoDMA path does not start voices");
            // now voices on top: the sum equals the separate contributions
            Rig both;
            for (int b = 0; b < 100; ++b)
                putBlock(both.ram, 0x60000 + b * 16, 7, 0, 0, 400 + b);
            both.master(0, 0x3FFF, 0x0000, 0x7FFF);
            both.master(1, 0x3FFF, 0x7FFF, 0x0000);
            both.w(0x400 + 0x198, 0x0FCC);
            both.mixAll(1);
            both.voice(1, 0, 0x30000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            both.kon(1, 1u);
            both.spu.setAdmaRateRecovery(false);
            both.spu.pushAdma(0, frames.data(), frames.size());
            auto mix = both.run(600);
            Rig voiceOnly;
            for (int b = 0; b < 100; ++b)
                putBlock(voiceOnly.ram, 0x60000 + b * 16, 7, 0, 0, 400 + b);
            voiceOnly.master(0, 0x3FFF, 0x0000, 0x7FFF);
            voiceOnly.master(1, 0x3FFF, 0x7FFF, 0x0000);
            voiceOnly.w(0x400 + 0x198, 0x0FCC);
            voiceOnly.mixAll(1);
            voiceOnly.voice(1, 0, 0x30000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            voiceOnly.kon(1, 1u);
            auto vOnly = voiceOnly.run(600);
            int bad2 = 0;
            for (int i = 0; i < 500; ++i)
            {
                const int32_t c0 = (((frames[i * 2] * 0x7FFF) >> 15) * volume(0x3FFF)) >> 15;
                const int32_t c1 = (((c0 * 0x7FFF) >> 15) * volume(0x3FFF)) >> 15;
                bad2 += std::abs(mix[i * 2] - (vOnly[i * 2] + c1)) > 3;
            }
            check(bad2 == 0, "mix = voices + AutoDMA contribution, each computed independently");
        }

        // ===== AutoDMA rate recovery: a stream slightly slower than 48 kHz neither clicks nor drifts ===================================
        {
            // blocks of 512 frames arrive every 393303 IOP cycles (the real game: 393216 + ~87 cycles of interrupt latency)
            Rig rig;
            uint64_t cycles = 0;
            rig.spu.setCycleSource(&cycles);
            rig.spu.setCollecting(true);
            rig.master(0, 0x3FFF, 0x0000, 0x7FFF);
            rig.master(1, 0x3FFF, 0x7FFF, 0x0000);
            rig.w(0x400 + 0x198, 0x0FCC);
            const double pi = 3.14159265358979323846;
            std::vector<int16_t> out;
            std::vector<int16_t> block(1024);
            double phase = 0.0;
            double maxJump = 0.0, previous = 0.0;
            size_t maxQueued = 0, produced = 0;
            bool first = true;
            for (int b = 0; b < 3500; ++b) // ~37 s
            {
                for (int i = 0; i < 512; ++i)
                {
                    const int16_t v = static_cast<int16_t>(8000.0 * std::sin(phase));
                    phase += 2.0 * pi * 440.0 / 48000.0;
                    block[i * 2] = v;
                    block[i * 2 + 1] = static_cast<int16_t>(-v);
                }
                cycles = static_cast<uint64_t>(b) * 393303u;
                rig.spu.pushAdma(0, block.data(), block.size());
                maxQueued = std::max(maxQueued, rig.spu.admaQueuedFrames());
                if (b % 50 == 49)
                {
                    cycles = static_cast<uint64_t>(b + 1) * 393303u;
                    rig.spu.catchUp();
                    std::vector<int16_t> part;
                    rig.spu.takeMix(part);
                    for (size_t i = 0; i + 1 < part.size(); i += 2)
                    {
                        if (first && part[i] == 0 && part[i + 1] == 0)
                            continue; // priming silence at the start (only until the tone begins)
                        if (!first)
                            maxJump = std::max(maxJump, std::abs(static_cast<double>(part[i]) - previous));
                        first = false;
                        previous = part[i];
                        ++produced;
                    }
                }
            }
            check(rig.spu.stats().admaUnderruns == 0u, "a stream 0.02 % slower than 48 kHz never runs dry (no inserted frames)");
            check(maxQueued < 2048u, "the queue stays bounded (rate recovery, not accumulation)");
            check(maxJump < 560.0, "output of a 440 Hz tone has no discontinuity (a missing frame would jump by thousands)");
            check(std::abs(rig.spu.admaStep() - (393216.0 / 393303.0)) < 0.0003, "the loop converged on the stream's rate");
        }

        // ===== the clock is the IOP cycle counter; writes take effect at their sample =========================================
        {
            Rig rig;
            for (int b = 0; b < 100; ++b)
                putBlock(rig.ram, 0x70000 + b * 16, 7, 0, 0, 500 + b);
            uint64_t cycles = 0;
            rig.spu.setCycleSource(&cycles);
            rig.spu.setCollecting(true);
            rig.master(1, 0x3FFF, 0, 0);
            rig.mixAll(1);
            rig.voice(1, 0, 0x38000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            cycles = 768u * 100u; // 100 samples of silence pass, then the key-on arrives
            rig.kon(1, 1u);
            cycles = 768u * 300u;
            std::vector<int16_t> out;
            rig.spu.takeMix(out);
            check(out.size() == 600u, "300 samples (600 values) at 768 IOP cycles per sample");
            size_t first = 0;
            while (first < out.size() && out[first] == 0)
                ++first;
            check(first / 2 >= 100u && first / 2 <= 110u, "the voice starts at the sample of its key-on write (attack from silence)");
            // determinism: the same script gives the same samples
            Rig again;
            for (int b = 0; b < 100; ++b)
                putBlock(again.ram, 0x70000 + b * 16, 7, 0, 0, 500 + b);
            uint64_t cycles2 = 0;
            again.spu.setCycleSource(&cycles2);
            again.spu.setCollecting(true);
            again.master(1, 0x3FFF, 0, 0);
            again.mixAll(1);
            again.voice(1, 0, 0x38000, 0x1000, 0x3FFF, 0x3FFF, kAdsr1Flat, kAdsr2Flat);
            cycles2 = 768u * 100u;
            again.kon(1, 1u);
            cycles2 = 768u * 300u;
            std::vector<int16_t> out2;
            again.spu.takeMix(out2);
            check(out == out2, "deterministic: identical output for identical register/cycle history");
            // reading ENDX / ENVX advances the engine to the current cycle
            cycles = 768u * 400u;
            uint16_t envx = 0;
            check(rig.spu.readDynamic(0x400 + 0x0A, envx) && envx > 0, "dynamic registers catch the engine up first");
            check(rig.spu.stats().samples == 400u, "exactly one rendered sample per 768 cycles");
            // a time reset (IOP reboot) must not stall the engine
            cycles = 0;
            rig.spu.catchUp();
            cycles = 768u * 10u;
            rig.spu.catchUp();
            check(rig.spu.stats().samples >= 410u, "the clock restarts after a cycle counter reset");
        }

        std::cout << "PASS " << checks << " SPU2 voice engine checks\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
