#include "iop_spu2.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kMasterBase = 0x760u; // master volumes: core 0 at 0x760, core 1 at 0x788
        constexpr uint32_t kMasterStride = 0x28u;
        constexpr size_t kMaxOutputSamples = 48000u * 2u * 10u;
        constexpr size_t kMaxAdmaSamples = 48000u * 2u;

        constexpr double kAdmaTargetFrames = 256.0; // queue level (just before a block arrives) the rate loop settles on
        constexpr double kAdmaPrimeFrames = 768.0;  // playback starts with one block plus the target queued
        constexpr double kAdmaGain = 4.0e-6;        // step change per frame of level error
        constexpr double kAdmaMaxDeviation = 0.003; // never follow the stream by more than 0.3 %

        bool admaLog()
        {
            static const bool on = std::getenv("PS2X_SPU2_ADMA_LOG") != nullptr;
            return on;
        }

        // PS2X_SPU2_KON_LOG=<file>: one CSV line per key-on / key-off with the emulated time (output sample index) and what the voice
        // is about to play: the ADPCM blocks from the start address up to the first end flag (length, invalid headers, loop flags,
        // whether the RAM there was ever written). Diagnostics only.
        FILE *konLog()
        {
            static FILE *file = []() -> FILE * {
                const char *path = std::getenv("PS2X_SPU2_KON_LOG");
                if (!path || !*path)
                    return nullptr;
                FILE *f = std::fopen(path, "w");
                if (f)
                    std::fputs("sample,seconds,event,core,voice,ssa,lsa,pitch,adsr1,adsr2,volL,volR,blocks,badHeaders,endFlag,repeat,firstBlockZero,maxShift,noise,pmon,reverb,mixL,mixR\n", f);
                return f;
            }();
            return file;
        }

        constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

        int32_t clamp16(int32_t v) { return std::clamp(v, -32768, 32767); }
    } // namespace

    void Spu2::reset()
    {
        if (const char *env = std::getenv("PS2X_SPU2_ADMA_RECOVER"); env && *env == '0')
            m_admaRecover = false; // diagnostics: one frame per output sample, no rate recovery
        m_regs.fill(0);
        for (auto &core : m_voices)
            core.fill(Voice{});
        m_endx[0] = m_endx[1] = 0u;
        m_adma[0].clear();
        m_adma[1].clear();
        m_admaPos = 0.0;
        m_admaStep = 1.0;
        m_admaLevel = 0.0;
        m_admaPrimed = false;
        m_underrun = false;
        m_out.clear();
        m_voicesOut.clear();
        m_admaOut.clear();
        m_rendered = 0u;
        m_stats = Stats{};
    }

    // Volume register in fixed mode: 15 bit signed value, doubled (0x3FFF is full scale). Sweep mode (bit 15) is not emulated: the
    // programmed value is used as the level and the use is counted.
    int32_t Spu2::volume(uint16_t value)
    {
        int32_t v = value & 0x7FFF;
        if (v & 0x4000)
            v -= 0x8000;
        return std::clamp(v * 2, -0x8000, 0x7FFF);
    }

    void Spu2::write16(uint32_t offset, uint16_t value)
    {
        if (offset >= kRegisterBytes || (offset & 1u))
            return;
        catchUp();
        m_regs[offset / 2u] = value;
        if (offset >= kMasterBase)
            return;
        const uint32_t core = offset >= kCoreStride ? 1u : 0u;
        const uint32_t local = offset & (kCoreStride - 1u);
        if (local == 0x1A0u || local == 0x1A2u)
        {
            for (uint32_t bit = 0; bit < 16u; ++bit)
            {
                const uint32_t voice = bit + (local == 0x1A2u ? 16u : 0u);
                if (((value >> bit) & 1u) && voice < kVoices)
                    keyOn(core, voice);
            }
        }
        else if (local == 0x1A4u || local == 0x1A6u)
        {
            for (uint32_t bit = 0; bit < 16u; ++bit)
            {
                const uint32_t voice = bit + (local == 0x1A4u ? 0u : 16u);
                if (((value >> bit) & 1u) && voice < kVoices)
                    keyOff(core, voice);
            }
        }
        else if ((local == 0x184u || local == 0x186u) && value != 0u)
            ++m_stats.noiseVoices;
        else if ((local == 0x180u || local == 0x182u) && value != 0u)
            ++m_stats.pitchModVoices;
        else if ((local == 0x18Cu || local == 0x18Eu || local == 0x194u || local == 0x196u) && value != 0u)
            ++m_stats.reverbSends;
    }

    bool Spu2::readDynamic(uint32_t offset, uint16_t &value)
    {
        if (offset >= kMasterBase || (offset & 1u))
            return false;
        const uint32_t core = offset >= kCoreStride ? 1u : 0u;
        const uint32_t local = offset & (kCoreStride - 1u);
        const uint32_t voiceBlock = local / 16u;
        if (local < kVoices * 16u && (local & 0xFu) >= 0xAu)
        {
            catchUp();
            const Voice &v = m_voices[core][voiceBlock];
            const uint32_t field = local & 0xFu;
            if (field == 0xAu)
                value = static_cast<uint16_t>(v.active ? v.level : 0);
            else // VOLXL / VOLXR: the effective volume (sweeps are not emulated, so it equals the programmed level)
                value = static_cast<uint16_t>(volume(r(core * kCoreStride + voiceBlock * 16u + (field == 0xCu ? 0u : 2u))) & 0xFFFF);
            return true;
        }
        if (local >= 0x1C0u && local < 0x1C0u + kVoices * 12u && ((local - 0x1C0u) % 12u) >= 8u)
        {
            catchUp();
            const uint32_t voice = (local - 0x1C0u) / 12u;
            const uint32_t word = m_voices[core][voice].active ? m_voices[core][voice].addr / 2u : 0u;
            value = (((local - 0x1C0u) % 12u) == 8u) ? static_cast<uint16_t>((word >> 16u) & 0xFu) : static_cast<uint16_t>(word & 0xFFFFu);
            return true;
        }
        if (local == 0x340u || local == 0x342u)
        {
            catchUp();
            value = static_cast<uint16_t>(local == 0x340u ? (m_endx[core] & 0xFFFFu) : (m_endx[core] >> 16u) & 0xFFu);
            return true;
        }
        return false;
    }

    void Spu2::pushAdma(uint32_t core, const int16_t *interleaved, size_t samples)
    {
        catchUp();
        auto &fifo = m_adma[core & 1u];
        if (core == 0u)
        {
            // queue level seen by this arrival (frames not yet played), smoothed over ~32 blocks
            const double level = static_cast<double>(fifo.size() / 2u) - m_admaPos - (m_admaPrimed ? 1.0 : 0.0);
            if (m_admaPrimed)
            {
                m_admaLevel += ((level - kAdmaTargetFrames) - m_admaLevel) / 32.0;
                m_admaStep = 1.0 + std::clamp(kAdmaGain * m_admaLevel, -kAdmaMaxDeviation, kAdmaMaxDeviation);
            }
            if (admaLog())
std::fprintf(stderr, "[adma] push t=%llu cyc=%llu level=%.1f step=%.6f add=%zu wall=%lld\n", static_cast<unsigned long long>(m_rendered),
                             static_cast<unsigned long long>(m_cycles ? *m_cycles : 0u), level, m_admaStep, samples / 2u,
                             static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() % 100000000000ll));
        }
        fifo.insert(fifo.end(), interleaved, interleaved + samples);
        m_stats.admaFrames += samples / 2u;
        if (fifo.size() > kMaxAdmaSamples)
            fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(fifo.size() - kMaxAdmaSamples));
    }

    void Spu2::logDma(const char *kind, uint32_t core, uint32_t iopAddress, uint64_t bytes, bool toSpu, uint32_t tsaWords, uint32_t admas)
    {
        FILE *log = konLog();
        if (!log)
            return;
        const uint64_t sample = m_cycles ? *m_cycles / kCyclesPerSample : m_rendered;
        // columns reused: ssa = IOP RAM address, lsa = TSA (16-bit units), blocks = bytes, repeat = direction (1 = to SPU), maxShift = ADMAS
        std::fprintf(log, "%llu,%.4f,%s,%u,,0x%x,0x%x,,,,,,%llu,,,%u,,%u,,,,,\n", static_cast<unsigned long long>(sample), static_cast<double>(sample) / 48000.0, kind,
                     core, iopAddress, tsaWords, static_cast<unsigned long long>(bytes), toSpu ? 1u : 0u, admas);
        std::fflush(log);
    }

    void Spu2::catchUp()
    {
        if (!m_cycles)
            return;
        const uint64_t due = *m_cycles / kCyclesPerSample;
        if (due < m_rendered) // the IOP time was reset (reboot): restart the sample clock
        {
            m_rendered = due;
            return;
        }
        if (due == m_rendered)
            return;
        uint64_t count = due - m_rendered;
        if (count > 48000u * 10u) // a long idle jump: keep the newest 10 s of time
        {
            m_rendered = due - 48000u * 10u;
            count = 48000u * 10u;
        }
        renderSamples(static_cast<size_t>(count));
    }

    void Spu2::renderSamples(size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            renderOne();
        m_rendered += count;
    }

    size_t Spu2::takeMix(std::vector<int16_t> &out)
    {
        catchUp();
        const size_t n = m_out.size();
        out.insert(out.end(), m_out.begin(), m_out.end());
        m_out.clear();
        return n;
    }

    size_t Spu2::takeVoices(std::vector<int16_t> &out)
    {
        catchUp();
        const size_t n = m_voicesOut.size();
        out.insert(out.end(), m_voicesOut.begin(), m_voicesOut.end());
        m_voicesOut.clear();
        return n;
    }

    size_t Spu2::takeAdmaMix(std::vector<int16_t> &out)
    {
        catchUp();
        const size_t n = m_admaOut.size();
        out.insert(out.end(), m_admaOut.begin(), m_admaOut.end());
        m_admaOut.clear();
        return n;
    }

    void Spu2::keyOn(uint32_t core, uint32_t index)
    {
        Voice &v = m_voices[core][index];
        const uint32_t base = core * kCoreStride;
        const uint32_t ssa = ((r(base + 0x1C0u + index * 12u) & 0xFu) << 16u) | r(base + 0x1C2u + index * 12u);
        const uint32_t lsa = ((r(base + 0x1C4u + index * 12u) & 0xFu) << 16u) | r(base + 0x1C6u + index * 12u);
        if (FILE *log = konLog())
        {
            const uint32_t mask = static_cast<uint32_t>(m_ram.size()) - 1u;
            uint32_t addr = (ssa * 2u) & mask & ~0xFu;
            uint32_t blocks = 0, bad = 0, maxShift = 0;
            // bit `index` of a 24-voice mask register pair (low word at `off`, high word 2 bytes later)
            auto bitOf = [&](uint32_t off) -> uint32_t {
                const uint32_t mask24 = r(base + off) | (static_cast<uint32_t>(r(base + off + 2u)) << 16u);
                return (mask24 >> index) & 1u;
            };
            int endFlag = 0, repeat = 0;
            const bool firstZero = std::all_of(m_ram.begin() + addr, m_ram.begin() + addr + 16u, [](uint8_t b) { return b == 0u; });
            for (; blocks < 8192u; ++blocks, addr = (addr + 16u) & mask)
            {
                const uint8_t *b = m_ram.data() + addr;
                if ((b[0] >> 4) > 4u || (b[0] & 0xFu) > 12u)
                    ++bad;
                maxShift = std::max<uint32_t>(maxShift, b[0] & 0xFu);
                if (b[1] & 1u)
                {
                    endFlag = 1;
                    repeat = (b[1] >> 1) & 1u;
                    ++blocks;
                    break;
                }
            }
            std::fprintf(log, "%llu,%.4f,on,%u,%u,0x%x,0x%x,0x%x,0x%04x,0x%04x,0x%04x,0x%04x,%u,%u,%d,%d,%d,%u,%u,%u,%u,%u,%u\n",
                         static_cast<unsigned long long>(m_rendered), static_cast<double>(m_rendered) / 48000.0, core, index, ssa, lsa,
                         r(index * 16u + base + 4u), r(index * 16u + base + 6u), r(index * 16u + base + 8u), r(index * 16u + base),
                         r(index * 16u + base + 2u), blocks, bad, endFlag, repeat, firstZero ? 1 : 0, maxShift, bitOf(0x184u), bitOf(0x180u),
                         bitOf(0x18Cu), bitOf(0x188u), bitOf(0x190u));
            std::fflush(log);
        }
        v = Voice{};
        v.active = true;
        v.addr = (ssa * 2u) & (static_cast<uint32_t>(m_ram.size()) - 1u) & ~0xFu;
        v.loopAddr = (lsa * 2u) & (static_cast<uint32_t>(m_ram.size()) - 1u) & ~0xFu;
        v.phase = Phase::Attack;
        v.level = 0;
        m_endx[core] &= ~(1u << index);
        decodeBlock(v);
        v.pos = 0;
        v.cur = fetch(core, index);
        v.next = v.active ? fetch(core, index) : 0;
        ++m_stats.keyOns;
    }

    void Spu2::keyOff(uint32_t core, uint32_t index)
    {
        Voice &v = m_voices[core][index];
        if (FILE *log = konLog())
            std::fprintf(log, "%llu,%.4f,off,%u,%u,,,,,,,,,,,,,\n", static_cast<unsigned long long>(m_rendered),
                         static_cast<double>(m_rendered) / 48000.0, core, index);
        if (v.active && v.phase != Phase::Release)
        {
            v.phase = Phase::Release;
            v.envCounter = 0u;
        }
        ++m_stats.keyOffs;
    }

    // SPU ADPCM: 16 byte block = [shift:4 | filter:4][flags][14 bytes of 28 nibbles]. flags: bit0 end, bit1 repeat, bit2 loop start.
    void Spu2::decodeBlock(Voice &v)
    {
        const uint8_t *b = m_ram.data() + v.addr;
        const int shift = b[0] & 0xF;
        const int filter = std::min<int>(b[0] >> 4, 4);
        v.flags = b[1];
        if (v.flags & 4u)
            v.loopAddr = v.addr;
        for (int j = 0; j < 28; ++j)
        {
            int32_t nib = (b[2 + j / 2] >> (4 * (j & 1))) & 0xF;
            if (nib >= 8)
                nib -= 16;
            int32_t s = ((nib << 12) >> shift) + ((v.s1 * kFilter[filter][0] + v.s2 * kFilter[filter][1] + 32) >> 6);
            s = clamp16(s);
            v.samples[static_cast<size_t>(j)] = static_cast<int16_t>(s);
            v.s2 = v.s1;
            v.s1 = s;
        }
    }

    int32_t Spu2::fetch(uint32_t core, uint32_t index)
    {
        Voice &v = m_voices[core][index];
        if (v.pos >= 28u)
        {
            if (v.flags & 1u)
            {
                m_endx[core] |= 1u << index;
                ++m_stats.voiceEnds;
                if (v.flags & 2u)
                    v.addr = v.loopAddr;
                else
                {
                    v.active = false;
                    v.level = 0;
                    return 0;
                }
            }
            else
                v.addr = (v.addr + 16u) & (static_cast<uint32_t>(m_ram.size()) - 1u);
            decodeBlock(v);
            v.pos = 0;
        }
        return v.samples[v.pos++];
    }

    // Hardware ADSR (register semantics as in the SPU/SPU2 documentation): ADSR1 = [mode:1][attack shift:5][attack step:2][decay shift:4]
    // [sustain level:4], ADSR2 = [sustain mode:1][direction:1][.:1][sustain shift:5][sustain step:2][release mode:1][release shift:5].
    void Spu2::stepEnvelope(uint32_t core, uint32_t index)
    {
        Voice &v = m_voices[core][index];
        const uint32_t base = core * kCoreStride + index * 16u;
        const uint16_t adsr1 = r(base + 6u), adsr2 = r(base + 8u);
        int shift = 0, step = 0;
        bool decrease = false, exponential = false;
        switch (v.phase)
        {
        case Phase::Attack:
            shift = (adsr1 >> 10) & 31;
            step = 7 - ((adsr1 >> 8) & 3);
            exponential = (adsr1 & 0x8000u) != 0;
            break;
        case Phase::Decay:
            shift = (adsr1 >> 4) & 15;
            step = -8;
            decrease = exponential = true;
            break;
        case Phase::Sustain:
            shift = (adsr2 >> 8) & 31;
            decrease = (adsr2 & 0x4000u) != 0;
            step = decrease ? -8 + ((adsr2 >> 6) & 3) : 7 - ((adsr2 >> 6) & 3);
            exponential = (adsr2 & 0x8000u) != 0;
            break;
        case Phase::Release:
            shift = adsr2 & 31;
            step = -8;
            decrease = true;
            exponential = (adsr2 & 0x20u) != 0;
            break;
        }
        uint32_t increment = 0x8000u >> std::max(0, shift - 11);
        if (shift < 11)
            step *= 1 << (11 - shift);
        if (exponential && decrease)
            step = (step * v.level) >> 15;
        else if (exponential && !decrease && v.level > 0x6000)
            increment = std::max<uint32_t>(1u, increment >> 2);
        v.envCounter += increment;
        if (v.envCounter < 0x8000u)
            return;
        v.envCounter = 0;
        v.level = std::clamp(v.level + step, 0, 0x7FFF);
        switch (v.phase)
        {
        case Phase::Attack:
            if (v.level >= 0x7FFF)
                v.phase = Phase::Decay;
            break;
        case Phase::Decay:
            if (v.level <= static_cast<int32_t>(((adsr1 & 0xFu) + 1u) * 0x800u))
                v.phase = Phase::Sustain;
            break;
        case Phase::Release:
            if (v.level == 0)
                v.active = false;
            break;
        case Phase::Sustain:
            break;
        }
    }

    void Spu2::renderOne()
    {
        int32_t dry[2][2] = {{0, 0}, {0, 0}}; // [core][L,R]
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t base = core * kCoreStride;
            const uint32_t vmixL = r(base + 0x188u) | (static_cast<uint32_t>(r(base + 0x18Au) & 0xFFu) << 16u);
            const uint32_t vmixR = r(base + 0x190u) | (static_cast<uint32_t>(r(base + 0x192u) & 0xFFu) << 16u);
            for (uint32_t index = 0; index < kVoices; ++index)
            {
                Voice &v = m_voices[core][index];
                if (!v.active)
                    continue;
                const uint32_t vbase = base + index * 16u;
                const uint16_t volL = r(vbase), volR = r(vbase + 2u);
                if ((volL | volR) & 0x8000u)
                    ++m_stats.sweepVolumes;
                const int32_t frac = static_cast<int32_t>(v.counter & 0xFFFu);
                int32_t sample = v.cur + (((v.next - v.cur) * frac) >> 12);
                sample = (sample * v.level) >> 15;
                if ((vmixL >> index) & 1u)
                    dry[core][0] += (sample * volume(volL)) >> 15;
                if ((vmixR >> index) & 1u)
                    dry[core][1] += (sample * volume(volR)) >> 15;
                // advance the sample position and the envelope
                uint32_t pitch = r(vbase + 4u) & 0x3FFFu;
                v.counter += pitch;
                while (v.counter >= 0x1000u && v.active)
                {
                    v.counter -= 0x1000u;
                    v.cur = v.next;
                    v.next = fetch(core, index);
                }
                if (v.active)
                    stepEnvelope(core, index);
            }
        }
        // AutoDMA input of core 0 (BVOL), kept apart from the voices until the final mix.
        int32_t adma[2] = {0, 0};
        auto &fifo = m_adma[0];
        if (!m_admaRecover)
        {
            if (fifo.size() >= 2)
            {
                adma[0] = fifo[0];
                adma[1] = fifo[1];
                fifo.pop_front();
                fifo.pop_front();
            }
            else if (m_stats.admaFrames > 0)
                ++m_stats.admaUnderruns;
        }
        else
        {
            if (!m_admaPrimed && static_cast<double>(fifo.size() / 2u) >= kAdmaPrimeFrames)
            {
                m_admaPrimed = true;
                m_admaPos = 1.0; // one history frame in front for the cubic interpolator
                if (m_underrun && admaLog())
                    std::fprintf(stderr, "[adma] re-primed t=%llu\n", static_cast<unsigned long long>(m_rendered));
                m_underrun = false;
            }
            const size_t frames = fifo.size() / 2u;
            const size_t index = static_cast<size_t>(m_admaPos);
            // 4-point cubic (Catmull-Rom) interpolation: the position moves by ~0.9997 frames per sample, and a linear
            // interpolator would low-pass the audio in proportion to the fractional position. One history frame is kept in front.
            if (m_admaPrimed && index >= 1u && index + 2u < frames)
            {
                const double t = m_admaPos - static_cast<double>(index);
                for (size_t ch = 0; ch < 2u; ++ch)
                {
                    const double p0 = fifo[(index - 1u) * 2u + ch], p1 = fifo[index * 2u + ch], p2 = fifo[(index + 1u) * 2u + ch],
                                 p3 = fifo[(index + 2u) * 2u + ch];
                    const double v = 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t +
                                            (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t * t * t);
                    adma[ch] = static_cast<int32_t>(std::lround(std::clamp(v, -32768.0, 32767.0)));
                }
                m_admaPos += m_admaStep;
                while (m_admaPos >= 2.0)
                {
                    fifo.erase(fifo.begin(), fifo.begin() + 2);
                    m_admaPos -= 1.0;
                }
            }
            else if (m_admaPrimed || m_underrun)
            {
                // ran dry: report it, output silence and prime again before resuming
                if (!m_underrun && admaLog())
                    std::fprintf(stderr, "[adma] underrun t=%llu\n", static_cast<unsigned long long>(m_rendered));
                m_underrun = true;
                m_admaPrimed = false;
                ++m_stats.admaUnderruns;
            }
        }
        // Final stage through the cores: core 0 (voices + AutoDMA input via BVOL, MVOL) -> core 1 external input (AVOL, gated by its MMIX)
        // -> core 1 (own voices, MVOL). Computed on demand for three views that share one timeline: everything, voices only (AutoDMA
        // muted) and AutoDMA only (voices muted). The voices and the AutoDMA input never share state; only this final sum meets them.
        auto finalStage = [&](bool useVoices, bool useAdma, int ch) -> int16_t
        {
            const int32_t bvol0 = static_cast<int16_t>(r(kMasterBase + 0xCu + ch * 2u)); // core 0 BVOLL/R
            const int32_t mvol0 = volume(r(kMasterBase + ch * 2u));
            const int32_t core0 = clamp16((((useVoices ? dry[0][ch] : 0) + (useAdma ? ((adma[ch] * bvol0) >> 15) : 0)) * mvol0) >> 15);
            const int32_t avol1 = static_cast<int16_t>(r(kMasterBase + kMasterStride + 8u + ch * 2u)); // core 1 AVOLL/R
            const int32_t mvol1 = volume(r(kMasterBase + kMasterStride + ch * 2u));
            const bool external = (r(kCoreStride + 0x198u) & 0x000Cu) != 0u; // core 1 MMIX: core 0 output enabled
            const int32_t ext = external ? ((core0 * avol1) >> 15) : 0;
            return static_cast<int16_t>(clamp16((((useVoices ? dry[1][ch] : 0) + ext) * mvol1) >> 15));
        };
        if (m_collect && m_out.size() < kMaxOutputSamples)
        {
            m_out.push_back(finalStage(true, true, 0));
            m_out.push_back(finalStage(true, true, 1));
        }
        if (m_collectVoices && m_voicesOut.size() < kMaxOutputSamples)
        {
            m_voicesOut.push_back(finalStage(true, false, 0));
            m_voicesOut.push_back(finalStage(true, false, 1));
        }
        if (m_collectAdma && m_admaOut.size() < kMaxOutputSamples)
        {
            m_admaOut.push_back(finalStage(false, true, 0));
            m_admaOut.push_back(finalStage(false, true, 1));
        }
        ++m_stats.samples;
    }
} // namespace ps2x::iop::detail
