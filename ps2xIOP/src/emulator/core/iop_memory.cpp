#include <vector>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <map>
#include "iop_memory.h"
#include "ps2x/iop/audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    // PS2X_SPU2_LOG=1: counts of SPU2 register reads/writes per address, printed every ~10 s (diagnostic only).
    void spu2Count(bool write, uint32_t address)
    {
        static const bool on = std::getenv("PS2X_SPU2_LOG") != nullptr;
        if (!on)
            return;
        static std::map<std::pair<uint32_t, bool>, uint64_t> counts;
        static auto last = std::chrono::steady_clock::now();
        ++counts[{address, write}];
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(10))
        {
            last = now;
            std::vector<std::pair<uint64_t, std::pair<uint32_t, bool>>> rows;
            for (const auto &c : counts)
                rows.push_back({c.second, c.first});
            std::sort(rows.rbegin(), rows.rend());
            std::fprintf(stderr, "[spu2]");
            for (size_t i = 0; i < rows.size() && i < 14; ++i)
                std::fprintf(stderr, " %c%05x=%llu", rows[i].second.second ? 'W' : 'R', rows[i].second.first & 0xFFFFFu, static_cast<unsigned long long>(rows[i].first));
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
            counts.clear();
        }
    }
}


namespace
{
    // PS2X_SPU2_TRACE=<prefix>: milestone M1 diagnostics. <prefix>.txt gets one line per event (D = DMA block with destination TSA,
    // K = key-on voice with its registers, F = key-off, A = ATTR write, M = ADMAS write), <prefix>.bin gets the bytes of every
    // non-silent DMA block (offset in the D line) and <prefix>.ram.N a snapshot of the sound RAM every 30 s (4 snapshots).
    struct Spu2Trace
    {
        std::FILE *text = nullptr;
        std::FILE *bin = nullptr;
        std::string prefix;
        uint64_t binBytes = 0;
        int snapshots = 0;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        Spu2Trace()
        {
            const char *env = std::getenv("PS2X_SPU2_TRACE");
            if (env == nullptr || *env == 0)
                return;
            prefix = env;
            text = std::fopen((prefix + ".txt").c_str(), "wb");
            bin = std::fopen((prefix + ".bin").c_str(), "wb");
        }
        ~Spu2Trace()
        {
            if (text)
                std::fclose(text);
            if (bin)
                std::fclose(bin);
        }
        bool on() const { return text != nullptr; }
        long ms() const
        {
            return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        }
    };

    Spu2Trace &spu2Trace()
    {
        static Spu2Trace trace;
        return trace;
    }
}


namespace
{
    // PS2X_SPU2_WAV=<file>: writes the core 0 AutoDMA stream as a 48 kHz stereo 16-bit WAV (verifiable output of milestone M1).
    struct AdmaWav
    {
        std::FILE *file = nullptr;
        uint32_t dataBytes = 0;
        explicit AdmaWav(const char *variable)
        {
            const char *env = std::getenv(variable);
            if (env == nullptr || *env == 0)
                return;
            file = std::fopen(env, "wb");
            if (file)
                writeHeader();
        }
        void writeHeader()
        {
            uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                             0x80, 0xBB, 0, 0, 0, 0x6E, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
            const uint32_t riff = 36u + dataBytes;
            std::memcpy(h + 4, &riff, 4);
            std::memcpy(h + 40, &dataBytes, 4);
            std::fseek(file, 0, SEEK_SET);
            std::fwrite(h, 1, sizeof(h), file);
            std::fseek(file, 0, SEEK_END);
        }
        void append(const int16_t *samples, size_t count)
        {
            if (!file || dataBytes > (200u << 20))
                return;
            std::fwrite(samples, 2, count, file);
            dataBytes += static_cast<uint32_t>(count * 2u);
            if ((dataBytes & 0xFFFFFu) < count * 2u) // refresh the header about every MB so a killed process leaves a valid file
                writeHeader();
        }
        ~AdmaWav()
        {
            if (file)
            {
                writeHeader();
                std::fclose(file);
            }
        }
    };

    AdmaWav &admaWav()
    {
        static AdmaWav wav("PS2X_SPU2_WAV");
        return wav;
    }

    // PS2X_SPU2_MIX_WAV=<file>: the final SPU2 mix (voices + AutoDMA stream through the cores) as a 48 kHz stereo WAV.
    AdmaWav &mixWav()
    {
        static AdmaWav wav("PS2X_SPU2_MIX_WAV");
        return wav;
    }

    // PS2X_SPU2_VOICES_WAV / PS2X_SPU2_ADMAMIX_WAV: the same timeline with only the ADPCM voices, or only the AutoDMA stream.
    AdmaWav &voicesWav()
    {
        static AdmaWav wav("PS2X_SPU2_VOICES_WAV");
        return wav;
    }

    AdmaWav &admaMixWav()
    {
        static AdmaWav wav("PS2X_SPU2_ADMAMIX_WAV");
        return wav;
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr int kDmaSpu0Irq = 0x24;
        constexpr int kDmaSpu1Irq = 0x28;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        reset();
        // PS2X_IOP_RAMWATCH=<lo>-<hi> (hex IOP RAM addresses, optional @<first IOP cycle to log>): logs who writes that range (instruction address, cycle, runs of sequential stores).
        if (const char *w = std::getenv("PS2X_IOP_RAMWATCH"); w != nullptr && *w != '\0')
        {
            unsigned lo = 0, hi = 0;
            unsigned long long from = 0;
            if (std::sscanf(w, "%x-%x@%llu", &lo, &hi, &from) >= 2 && hi > lo)
            {
                m_watchFrom = from;
                m_watchLo = lo;
                m_watchHi = hi;
            }
        }
    }

    void IopMemory::flushWatchRun()
    {
        if (m_watchRun.n == 0u)
            return;
        if (m_watchLines < 400000u)
        {
            ++m_watchLines;
            std::fprintf(stderr, "[iopwatch] cyc=%llu..%llu pc=%08x %s addr=%05x..%05x n=%u v0=%08x\n",
                         static_cast<unsigned long long>(m_watchRun.cyc0), static_cast<unsigned long long>(m_watchRun.cyc1),
                         m_watchRun.pc, m_watchRun.kind, m_watchRun.first, m_watchRun.next, m_watchRun.n, m_watchRun.v0);
        }
        m_watchRun.n = 0u;
    }

    void IopMemory::watchWrite(uint32_t phys, size_t size, const char *kind)
    {
        if (phys + size <= m_watchLo || phys >= m_watchHi)
            return;
        const uint64_t cyc = m_cycleSource ? *m_cycleSource : 0u;
        if (cyc < m_watchFrom)
            return;
        uint32_t v0 = 0u;
        if (phys + 4u <= RamSize)
            std::memcpy(&v0, m_ram.data() + phys, 4);
        // merge sequential stores of the same instruction into one line (a copy loop is one run)
        if (m_watchRun.n != 0u && m_watchRun.pc == m_execPc && m_watchRun.kind == kind && m_watchRun.next == phys)
        {
            m_watchRun.next = phys + static_cast<uint32_t>(size);
            ++m_watchRun.n;
            m_watchRun.cyc1 = cyc;
            return;
        }
        flushWatchRun();
        m_watchRun = WatchRun{m_execPc, phys, phys + static_cast<uint32_t>(size), 1u, cyc, cyc, kind, v0};
    }

    IopMemory::~IopMemory()
    {
        flushWatchRun();
        // Diagnostic summary of the voice engine (what it mixed and what it had to ignore) when the mix is being captured.
        if (mixWav().file || voicesWav().file || std::getenv("PS2X_SPU2_LOG"))
        {
            const auto &st = m_spu2.stats();
            std::fprintf(stderr,
                         "[spu2:engine] samples=%llu keyOns=%llu keyOffs=%llu voiceEnds=%llu admaFrames=%llu admaUnderruns=%llu | not emulated: sweepVolumes=%llu noise=%llu reverbSends=%llu pitchMod=%llu\n",
                         static_cast<unsigned long long>(st.samples), static_cast<unsigned long long>(st.keyOns),
                         static_cast<unsigned long long>(st.keyOffs), static_cast<unsigned long long>(st.voiceEnds),
                         static_cast<unsigned long long>(st.admaFrames), static_cast<unsigned long long>(st.admaUnderruns),
                         static_cast<unsigned long long>(st.sweepVolumes), static_cast<unsigned long long>(st.noiseVoices),
                         static_cast<unsigned long long>(st.reverbSends), static_cast<unsigned long long>(st.pitchModVoices));
        }
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        std::fill(m_spuRam.begin(), m_spuRam.end(), uint8_t{0});
        m_spu2.reset();
        m_spu2.setCollecting(mixWav().file != nullptr);
        m_spu2.setCollectingVoices(voicesWav().file != nullptr);
        m_spu2.setCollectingAdma(admaMixWav().file != nullptr);
        m_admaPcm[0].clear();
        m_admaPcm[1].clear();
        m_admaBlocks[0] = m_admaBlocks[1] = 0u;
        m_spuTransferAddress[0] = m_spuTransferAddress[1] = 0u;
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_dmaStart.reset();
        m_sio2.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (Sio2::contains(phys))
            return static_cast<uint8_t>(m_sio2.read(phys, 1u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if (Sio2::contains(phys) && (phys & 1u) == 0u)
            return static_cast<uint16_t>(m_sio2.read(phys, 2u));
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && Sio2::contains(phys))
            return m_sio2.read(phys, 4u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            if (m_watchHi) watchWrite(phys, 1u, "w8");
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (Sio2::contains(phys))
        {
            m_sio2.write(phys, 1u, value);
            collectSio2Completion();
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            if (m_watchHi) watchWrite(phys, 2u, "w16");
            return;
        }
        if (Sio2::contains(phys) && (phys & 1u) == 0u)
        {
            m_sio2.write(phys, 2u, value);
            collectSio2Completion();
            return;
        }
        if (phys >= Spu2Base && phys < Spu2End && (phys & 1u) == 0u)
        {
            spu2Write16(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    // 16-bit SPU2 register write: stored exactly as before (aligned 32-bit word, read-modify-write) plus the side effects that
    // give the sound RAM its contents: TSA (transfer start address) and the CPU data port. The key-on registers are logged
    // (PS2X_SPU2_LOG) with the voice parameters, to learn how the game uses the voices before a mixer exists.
    void IopMemory::spu2Write16(uint32_t phys, uint16_t value)
    {
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 2u) * 8u;
        current = (current & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
        m_spu2.write16(phys - Spu2Base, value);

        const uint32_t core = ((phys - Spu2Base) >= 0x400u) ? 1u : 0u;
        const uint32_t offset = (phys - Spu2Base) & 0x3FFu;
        if (Spu2Trace &trace = spu2Trace(); trace.on())
        {
            if (offset == 0x19Au)
                std::fprintf(trace.text, "A %ld %u %04x\n", trace.ms(), core, value);
            else if (offset == 0x1B0u)
                std::fprintf(trace.text, "M %ld %u %04x\n", trace.ms(), core, value);
            else if ((offset == 0x1A4u || offset == 0x1A6u) && value != 0u)
                std::fprintf(trace.text, "F %ld %u %u %04x\n", trace.ms(), core, offset == 0x1A6u ? 1u : 0u, value);
        }
        if (offset == 0x1A8u)
            m_spuTransferAddress[core] = (m_spuTransferAddress[core] & 0x0000FFFFu) | (static_cast<uint32_t>(value & 0xFu) << 16u);
        else if (offset == 0x1AAu)
            m_spuTransferAddress[core] = (m_spuTransferAddress[core] & 0xFFFF0000u) | value;
        else if (offset == 0x1ACu) // data port: one 16-bit word at TSA, TSA advances
        {
            const uint32_t byteAddress = (m_spuTransferAddress[core] * 2u) & (SpuRamSize - 1u);
            std::memcpy(m_spuRam.data() + byteAddress, &value, sizeof(value));
            ++m_spuTransferAddress[core];
        }
        else if ((offset == 0x1A0u || offset == 0x1A2u) && value != 0u)
        {
            if (Spu2Trace &trace = spu2Trace(); trace.on())
            {
                const uint32_t base = Spu2Base + core * 0x400u;
                auto reg = [&](uint32_t off) { return static_cast<uint32_t>(read16(base + off)); };
                for (uint32_t bit = 0; bit < 16u; ++bit)
                {
                    const uint32_t voice = bit + (offset == 0x1A2u ? 16u : 0u);
                    if (((value >> bit) & 1u) == 0u || voice >= 24u)
                        continue;
                    const uint32_t ssa = ((reg(0x1C0u + voice * 12u) & 0xFu) << 16u) | reg(0x1C2u + voice * 12u);
                    const uint32_t lsa = ((reg(0x1C4u + voice * 12u) & 0xFu) << 16u) | reg(0x1C6u + voice * 12u);
                    if (ssa != 0x2800u)
                    {
                        static long lastDump = -100000;
                        if (trace.ms() - lastDump > 1000)
                        {
                            lastDump = trace.ms();
                            std::fprintf(trace.text, "R %ld %u", trace.ms(), core);
                            std::vector<std::pair<uint32_t, uint32_t>> regs(m_hardware.begin(), m_hardware.end());
                            std::sort(regs.begin(), regs.end());
                            for (const auto &entry : regs)
                                if (entry.first >= Spu2Base && entry.first < Spu2End)
                                    std::fprintf(trace.text, " %x=%08x", entry.first & 0xFFFFu, entry.second);
                            std::fprintf(trace.text, "\n");
                        }
                    }
                    std::fprintf(trace.text, "K %ld %u %u ssa=%x lsa=%x pitch=%x adsr=%04x/%04x vol=%04x/%04x attr=%04x admas=%04x mmix=%04x\n",
                                 trace.ms(), core, voice, ssa, lsa, reg(voice * 16u + 4u), reg(voice * 16u + 6u), reg(voice * 16u + 8u),
                                 reg(voice * 16u), reg(voice * 16u + 2u), reg(0x19Au), reg(0x1B0u), reg(0x198u));
                }
            }
            static const bool log = std::getenv("PS2X_SPU2_LOG") != nullptr;
            static int lines = 0;
            if (!log || lines > 400)
                return;
            for (uint32_t bit = 0; bit < 16u; ++bit)
            {
                if (((value >> bit) & 1u) == 0u)
                    continue;
                const uint32_t voice = bit + (offset == 0x1A2u ? 16u : 0u);
                if (voice >= 24u)
                    continue;
                const uint32_t base = Spu2Base + core * 0x400u;
                auto reg = [&](uint32_t off) { return static_cast<uint32_t>(read16(base + off)); };
                const uint32_t ssa = ((reg(0x1C0u + voice * 12u) & 0xFu) << 16u) | reg(0x1C2u + voice * 12u);
                const uint32_t byteAddress = (ssa * 2u) & (SpuRamSize - 1u);
                const uint8_t first = m_spuRam[byteAddress];
                std::fprintf(stderr,
                             "[spu2:kon] core=%u voice=%u ssa=0x%x(byte 0x%x) pitch=0x%x adsr=%04x/%04x vol=%04x/%04x block0=%02x %02x (%s)\n",
                             core, voice, ssa, byteAddress, reg(voice * 16u + 4u), reg(voice * 16u + 6u), reg(voice * 16u + 8u),
                             reg(voice * 16u), reg(voice * 16u + 2u), first, m_spuRam[byteAddress + 1u],
                             ((first >> 4) <= 4u && (first & 0xFu) <= 12u) ? "vag-like header" : "NOT a vag header");
                ++lines;
                if (ssa != 0x2800u)
                    if (const char *dump = std::getenv("PS2X_SPU2_RAM_DUMP"); dump != nullptr && *dump != 0)
                        if (static bool done = false; !done)
                        {
                            done = true;
                            if (FILE *file = std::fopen(dump, "wb"))
                            {
                                std::fwrite(m_spuRam.data(), 1, m_spuRam.size(), file);
                                std::fclose(file);
                            }
                        }
            }
        }
    }

    // Moves the engine's mix to the WAV file (PS2X_SPU2_MIX_WAV) and/or the real-time audio sink.
    void IopMemory::pumpMixWav()
    {
        std::vector<int16_t> block;
        AudioSink *const sink = audioSink();
        if (sink != nullptr)
            m_spu2.setCollecting(true);
        if ((mixWav().file || sink != nullptr) && m_spu2.takeMix(block) != 0u)
        {
            if (mixWav().file)
                mixWav().append(block.data(), block.size());
            // Real-time output (M3): a wait-free hand-over; the device side never feeds anything back into the emulation.
            if (sink != nullptr && !audioFastForward() && !audioMuted())
                sink->submit(block.data(), block.size() / 2u);
        }
        block.clear();
        if (voicesWav().file && m_spu2.takeVoices(block) != 0u)
            voicesWav().append(block.data(), block.size());
        block.clear();
        if (admaMixWav().file && m_spu2.takeAdmaMix(block) != 0u)
            admaMixWav().append(block.data(), block.size());
    }

    // Renders the SPU2 samples due at the current IOP time and hands them to the consumers (WAV files, audio device). Called often by the
    // emulator so the mix is delivered continuously, not only when an AutoDMA block arrives. Rendering earlier than before does not change the
    // output: every register write and every AutoDMA push already renders up to the current cycle first.
    void IopMemory::pumpAudioOutput()
    {
        if (audioSink() == nullptr)
            return;
        m_spu2.catchUp();
        pumpMixWav();
    }

    size_t IopMemory::takeAdmaPcm(uint32_t core, std::vector<int16_t> &out)
    {
        auto &queue = m_admaPcm[core & 1u];
        const size_t count = queue.size();
        out.insert(out.end(), queue.begin(), queue.end());
        queue.clear();
        return count;
    }

    // SPU DMA: IOP RAM <-> SPU RAM through the core's TSA (the DMA hardware itself has no address register for sound RAM).
    void IopMemory::spu2Dma(uint32_t chcrAddress, uint32_t chcr, uint64_t transferWords)
    {
        const uint32_t core = chcrAddress == 0x1F801508u ? 1u : 0u;
        uint32_t madr = 0u;
        if (const auto current = m_hardware.find(chcrAddress - 8u); current != m_hardware.end())
            madr = current->second;
        const uint64_t bytes = transferWords * 4u;
        const uint32_t iop = madr & (RamSize - 1u);
        if (bytes == 0u || iop + bytes > RamSize)
            return;
        uint32_t spu = (m_spuTransferAddress[core] * 2u) & (SpuRamSize - 1u);
        const bool toSpu = (chcr & 1u) != 0u; // from memory
        const bool autoDma = (read16(Spu2Base + core * 0x400u + 0x1B0u) & 0x3u) != 0u;
        if (Spu2Trace &trace = spu2Trace(); trace.on())
        {
            long binOffset = -1;
            const uint8_t *blob = m_ram.data() + iop;
            if (trace.bin && trace.binBytes + bytes <= (96ull << 20) && std::any_of(blob, blob + bytes, [](uint8_t b) { return b != 0; }))
            {
                binOffset = static_cast<long>(trace.binBytes);
                std::fwrite(blob, 1, static_cast<size_t>(bytes), trace.bin);
                trace.binBytes += bytes;
            }
            const uint32_t admas = read16(Spu2Base + core * 0x400u + 0x1B0u), attr = read16(Spu2Base + core * 0x400u + 0x19Au);
            std::fprintf(trace.text, "D %ld %u tsa=%x bytes=%llu madr=%x dir=%s chcr=%08x admas=%04x attr=%04x bin=%ld\n", trace.ms(), core,
                         m_spuTransferAddress[core], static_cast<unsigned long long>(bytes), iop, toSpu ? "toSPU" : "fromSPU", chcr, admas, attr,
                         binOffset);
            if (trace.snapshots < 4 && trace.ms() >= 30000L * (trace.snapshots + 1))
            {
                if (std::FILE *snap = std::fopen((trace.prefix + ".ram." + std::to_string(trace.snapshots)).c_str(), "wb"))
                {
                    std::fwrite(m_spuRam.data(), 1, m_spuRam.size(), snap);
                    std::fclose(snap);
                }
                ++trace.snapshots;
            }
        }
        m_spu2.logDma(autoDma && toSpu ? "adma" : "dma", core, iop, bytes, toSpu, m_spuTransferAddress[core], read16(Spu2Base + core * 0x400u + 0x1B0u));
        if (autoDma && toSpu)
        {
            // AutoDMA input: pairs of 0x200 bytes [L][R] (not written to the sound RAM, TSA untouched).
            std::vector<int16_t> frames(static_cast<size_t>(bytes / 0x400u) * 512u);
            for (uint64_t pair = 0, out = 0; pair + 0x400u <= bytes; pair += 0x400u)
            {
                const uint8_t *left = m_ram.data() + iop + pair;
                const uint8_t *right = left + 0x200u;
                for (uint32_t i = 0; i < 256u; ++i, out += 2u)
                {
                    std::memcpy(&frames[static_cast<size_t>(out)], left + i * 2u, 2u);
                    std::memcpy(&frames[static_cast<size_t>(out) + 1u], right + i * 2u, 2u);
                }
            }
            if (core == 0u)
                admaWav().append(frames.data(), frames.size());
            auto &queue = m_admaPcm[core];
            queue.insert(queue.end(), frames.begin(), frames.end());
            constexpr size_t kKeep = 48000u * 2u * 10u;
            if (queue.size() > kKeep)
                queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(queue.size() - kKeep));
            ++m_admaBlocks[core];
            // The silence / noise detectors only feed PS2X_DUMP_ON_ZERO, PS2X_DUMP_ON_NOISE and the KON log: skip them when none is on.
            static const bool admaDiagnostics = std::getenv("PS2X_DUMP_ON_ZERO") != nullptr || std::getenv("PS2X_DUMP_ON_NOISE") != nullptr || std::getenv("PS2X_SPU2_KON_LOG") != nullptr;
            if (core == 0u && admaDiagnostics)
            {
                bool silent = true;
                for (const int16_t v : frames)
                    if (v != 0)
                    {
                        silent = false;
                        break;
                    }
                {
                    // noise-like block (diagnostics): loud with a high zero-crossing rate on the left channel
                    double energy = 0.0;
                    uint32_t crossings = 0;
                    for (size_t i = 0; i + 2u < frames.size(); i += 2u)
                    {
                        energy += static_cast<double>(frames[i]) * frames[i];
                        crossings += ((frames[i] < 0) != (frames[i + 2u] < 0)) ? 1u : 0u;
                    }
                    const double rms = std::sqrt(energy / static_cast<double>(std::max<size_t>(frames.size() / 2u, 1u)));
                    const bool noisy = rms > 9000.0 && crossings * 4u > frames.size() / 2u;
                    if (noisy)
                    {
                        if (m_noiseRun++ == 0u)
                        {
                            m_spu2.logDma("adma-noise", core, iop, bytes, true, m_spuTransferAddress[core], read16(Spu2Base + 0x1B0u));
                            ++m_noiseEvents;
                        }
                    }
                    else
                        m_noiseRun = 0u;
                }
                if (silent && m_audibleRun >= 8u)
                    ++m_zeroBlockEvents;
                m_audibleRun = silent ? 0u : m_audibleRun + 1u;
            }
            m_spu2.pushAdma(core, frames.data(), frames.size());
            pumpMixWav();
            return;
        }
        for (uint64_t done = 0; done < bytes;)
        {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(bytes - done, SpuRamSize - spu));
            if (toSpu)
                std::memcpy(m_spuRam.data() + spu, m_ram.data() + iop + done, chunk);
            else
                std::memcpy(m_ram.data() + iop + done, m_spuRam.data() + spu, chunk);
            done += chunk;
            spu = (spu + static_cast<uint32_t>(chunk)) & (SpuRamSize - 1u);
        }
        m_spuTransferAddress[core] = (m_spuTransferAddress[core] + static_cast<uint32_t>(bytes / 2u)) & 0xFFFFFu;
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            if (m_watchHi) watchWrite(phys, 4u, "w32");
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && Sio2::contains(phys))
        {
            m_sio2.write(phys, 4u, value);
            collectSio2Completion();
            return;
        }
        if ((phys & 3u) == 0u && phys >= Spu2Base && phys < Spu2End)
        {
            spu2Write16(phys, static_cast<uint16_t>(value & 0xFFFFu));
            spu2Write16(phys + 2u, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memmove(m_ram.data() + phys, source, size);
            markOwned(phys, size);
            if (m_watchHi) watchWrite(phys, size, "bulk");
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
            if (m_watchHi) watchWrite(phys, size, "zero");
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    void IopMemory::collectSio2Completion()
    {
        if (const auto completion = m_sio2.takeCompletion())
            m_dmaStart = DmaStart{completion->irq, completion->delayCycles};
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        if (address >= Spu2Base && address < Spu2End)
        {
            spu2Count(false, address);
            uint16_t lo = 0, hi = 0;
            const bool dynamicLo = m_spu2.readDynamic(address - Spu2Base, lo);
            const bool dynamicHi = m_spu2.readDynamic(address - Spu2Base + 2u, hi);
            if (dynamicLo || dynamicHi)
            {
                uint32_t word = 0u;
                if (const auto stored = m_hardware.find(address); stored != m_hardware.end())
                    word = stored->second;
                if (dynamicLo)
                    word = (word & 0xFFFF0000u) | lo;
                if (dynamicHi)
                    word = (word & 0x0000FFFFu) | (static_cast<uint32_t>(hi) << 16u);
                return word;
            }
        }
        const auto value = m_hardware.find(address);
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        if (address >= Spu2Base && address < Spu2End)
            spu2Count(true, address);
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        const bool secondCore = address == kDmaSpu1Chcr;
        m_hardware[address] = value & ~kDmaStart;

        const uint32_t statusAddress = 0x1F900344u + (secondCore ? 0x400u : 0u);
        const uint32_t alignedStatus = statusAddress & ~3u;
        const uint32_t shift = (statusAddress & 2u) * 8u;
        uint32_t status = 0u;
        if (const auto current = m_hardware.find(alignedStatus); current != m_hardware.end())
            status = current->second;
        status |= 0x80u << shift;
        m_hardware[alignedStatus] = status;

        const uint32_t blockControlAddress = address - sizeof(uint32_t);
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(blockControlAddress); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t transferWords = static_cast<uint64_t>(wordsPerBlock) * blockCount;
        // AutoDMA (ADMA): the SPU2 pulls the stream at its playback rate (48 kHz, 16 bit stereo = 192 KB/s), so a
        // block completes only after the audio it carries has been played. Plain DMA keeps the fast bus model.
        // REG_ADMAS is the 16 bit register at +0x1B0 of each core.
        const uint32_t admasAddress = 0x1F9001B0u + (secondCore ? 0x400u : 0u);
        uint32_t admasWord = 0u;
        if (const auto current = m_hardware.find(admasAddress & ~3u); current != m_hardware.end())
            admasWord = current->second;
        const bool autoDma = ((admasWord >> ((admasAddress & 2u) * 8u)) & 0x3u) != 0u;
        constexpr uint64_t kIopCyclesPerAdmaByte = 36'864'000ull / 192'000ull;
        spu2Dma(address, value, transferWords);
        if (static const bool log = std::getenv("PS2X_SPU2_LOG") != nullptr; log)
        {
            static int lines = 0;
            const uint32_t madrAddress = address - 8u; // CHCR - 8 = MADR
            uint32_t madr = 0u;
            if (const auto current = m_hardware.find(madrAddress); current != m_hardware.end())
                madr = current->second;
            if (lines++ < 80)
                std::fprintf(stderr, "[spu2:dma] core=%d madr=0x%x words=%llu auto=%d chcr=0x%x\n", secondCore ? 1 : 0, madr,
                             static_cast<unsigned long long>(transferWords), autoDma ? 1 : 0, value);
        }
        // Diagnostic: PS2X_SPU2_DUMP=<file> appends the raw bytes of every core DMA block (ADMA ping-pong buffers) to a file (max 64 MB).
        if (const char *dump = std::getenv("PS2X_SPU2_DUMP"); dump != nullptr && *dump != '\0')
        {
            static uint64_t written = 0;
            const uint32_t madrAddress = address - 8u;
            uint32_t madr = 0u;
            if (const auto current = m_hardware.find(madrAddress); current != m_hardware.end())
                madr = current->second;
            const uint64_t bytes = transferWords * 4u;
            if (written + bytes <= (64ull << 20) && (madr & 0x1FFFFFu) + bytes <= m_ram.size())
            {
                if (FILE *file = std::fopen(dump, written == 0 ? "wb" : "ab"))
                {
                    std::fwrite(m_ram.data() + (madr & 0x1FFFFFu), 1, static_cast<size_t>(bytes), file);
                    std::fclose(file);
                    written += bytes;
                }
            }
        }
        m_dmaStart = DmaStart{
            secondCore ? kDmaSpu1Irq : kDmaSpu0Irq,
            autoDma ? std::max<uint64_t>(transferWords * 4u * kIopCyclesPerAdmaByte, 64u)
                    : std::max<uint64_t>(transferWords * 2u, 64u),
        };
    }

    std::optional<IopMemory::DmaStart> IopMemory::takeDmaStart() noexcept
    {
        std::optional<DmaStart> result = m_dmaStart;
        m_dmaStart.reset();
        return result;
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        // First fit over the gaps between live blocks, like the original sysmem: freed memory is
        // reused (a bump-only cursor made long-lived churn look like an exhausted heap).
        const std::vector<Allocation> ordered = sortedAllocations();
        uint32_t candidate = alignUp(HeapBase, alignment);
        for (const auto &block : ordered)
        {
            if (candidate < block.address + block.size && block.address < candidate + size)
                candidate = alignUp(block.address + block.size, alignment);
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    std::vector<IopMemory::Allocation> IopMemory::sortedAllocations() const
    {
        std::vector<Allocation> ordered = m_allocations;
        std::sort(ordered.begin(), ordered.end(),
                  [](const Allocation &a, const Allocation &b)
                  { return a.address < b.address; });
        return ordered;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        uint32_t largest = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : sortedAllocations())
        {
            if (block.address > cursor)
                largest = std::max(largest, block.address - cursor);
            cursor = std::max(cursor, block.address + block.size);
        }
        if (cursor < HeapLimit)
            largest = std::max(largest, HeapLimit - cursor);
        return largest;
    }

    uint32_t IopMemory::totalFreeMemory() const
    {
        uint32_t used = 0u;
        for (const auto &block : m_allocations)
            used += block.size;
        const uint32_t capacity = HeapLimit - HeapBase;
        return used < capacity ? capacity - used : 0u;
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}
