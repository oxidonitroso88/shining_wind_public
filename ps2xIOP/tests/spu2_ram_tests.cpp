// SPU2 sound RAM (milestone M0): DMA channels and the CPU data port fill/read the 2 MB sound RAM through each core's TSA.
#include "iop_compat_test_support.h"
#include "emulator/core/iop_memory.h"

#include <cstring>
#include <iostream>
#include <vector>

using namespace ps2x::iop::detail;

int main()
{
    try
    {
        size_t checks = 0;
        auto check = [&](bool ok, const char *why) { iop_test::require(ok, why); ++checks; };
        constexpr uint32_t kSpuBase = 0x1F900000u, kCore1 = 0x400u;
        constexpr uint32_t kMadr0 = 0x1F8010C0u, kBcr0 = 0x1F8010C4u, kChcr0 = 0x1F8010C8u;
        constexpr uint32_t kMadr1 = 0x1F801500u, kBcr1 = 0x1F801504u, kChcr1 = 0x1F801508u;
        auto setTsa = [&](IopMemory &m, uint32_t core, uint32_t words) {
            m.write16(kSpuBase + core * kCore1 + 0x1A8u, static_cast<uint16_t>(words >> 16u));
            m.write16(kSpuBase + core * kCore1 + 0x1AAu, static_cast<uint16_t>(words & 0xFFFFu));
        };

        IopMemory memory;
        check(memory.spuRam().size() == IopMemory::SpuRamSize, "2 MB of sound RAM");
        check(std::all_of(memory.spuRam().begin(), memory.spuRam().end(), [](uint8_t b) { return b == 0; }), "sound RAM starts empty");

        // CPU data port: 16-bit words at TSA, TSA advances.
        setTsa(memory, 0, 0x2800u);
        memory.write16(kSpuBase + 0x1ACu, 0x1234u);
        memory.write16(kSpuBase + 0x1ACu, 0xABCDu);
        check(memory.spuRam()[0x5000] == 0x34 && memory.spuRam()[0x5001] == 0x12 && memory.spuRam()[0x5002] == 0xCD && memory.spuRam()[0x5003] == 0xAB,
              "data port writes little-endian words at TSA*2");
        memory.write16(kSpuBase + 0x1ACu, 0x5555u);
        check(memory.spuRam()[0x5004] == 0x55, "TSA advanced after each word");
        // The register file still reads back what was written (unchanged behaviour for the IOP code that polls it).
        check(memory.read16(kSpuBase + 0x1A8u) == 0u && memory.read16(kSpuBase + 0x1AAu) == 0x2800u, "TSA registers read back");

        // DMA to the SPU: IOP RAM -> sound RAM at TSA (core 0 channel), TSA advances by the transferred words.
        uint8_t pattern[64];
        for (int i = 0; i < 64; ++i)
            pattern[i] = static_cast<uint8_t>(0x80 + i);
        check(memory.writeRam(0x30000u, pattern, sizeof(pattern)), "stage data in IOP RAM");
        setTsa(memory, 0, 0x4000u); // byte 0x8000
        memory.write32(kMadr0, 0x30000u);
        memory.write32(kBcr0, 16u); // 16 words * 1 block
        memory.write32(kChcr0, 0x01000201u);
        check(std::memcmp(memory.spuRam().data() + 0x8000, pattern, sizeof(pattern)) == 0, "DMA copied 64 bytes to sound RAM");
        check(memory.takeDmaStart().has_value(), "the DMA still schedules its completion interrupt");

        // Second core uses its own TSA.
        setTsa(memory, 1, 0x8000u); // byte 0x10000
        memory.write32(kMadr1, 0x30000u);
        memory.write32(kBcr1, 16u);
        memory.write32(kChcr1, 0x01000201u);
        check(std::memcmp(memory.spuRam().data() + 0x10000, pattern, sizeof(pattern)) == 0, "core 1 DMA lands at its own TSA");
        check(std::memcmp(memory.spuRam().data() + 0x8000, pattern, sizeof(pattern)) == 0, "core 0 data untouched");

        // DMA from the SPU (read direction) fills IOP RAM.
        uint8_t zero[64] = {};
        check(memory.writeRam(0x31000u, zero, sizeof(zero)), "clear destination");
        setTsa(memory, 0, 0x4000u);
        memory.write32(kMadr0, 0x31000u);
        memory.write32(kBcr0, 16u);
        memory.write32(kChcr0, 0x01000200u); // bit 0 clear: SPU -> memory
        uint8_t back[64] = {};
        check(memory.readRam(0x31000u, back, sizeof(back)) && std::memcmp(back, pattern, sizeof(pattern)) == 0, "read-direction DMA returns the sound RAM");

        // Consecutive DMAs continue where the previous one stopped (TSA advances).
        setTsa(memory, 0, 0x6000u);
        memory.write32(kMadr0, 0x30000u);
        memory.write32(kBcr0, 16u);
        memory.write32(kChcr0, 0x01000201u);
        memory.write32(kMadr0, 0x30000u);
        memory.write32(kBcr0, 16u);
        memory.write32(kChcr0, 0x01000201u);
        check(std::memcmp(memory.spuRam().data() + 0xC000 + 64, pattern, sizeof(pattern)) == 0, "second DMA continues after the first");

        // Wrap-around at the end of the 2 MB.
        setTsa(memory, 0, 0xFFFF0u); // 16 words (32 bytes) before the end
        memory.write32(kMadr0, 0x30000u);
        memory.write32(kBcr0, 16u);
        memory.write32(kChcr0, 0x01000201u);
        check(memory.spuRam()[IopMemory::SpuRamSize - 32] == pattern[0] && memory.spuRam()[0] == pattern[32], "transfer wraps around the sound RAM");

        // AutoDMA (REG_ADMAS != 0) is PCM input, not a sound RAM write: pairs of 0x200 bytes [L][R] become interleaved frames,
        // the sound RAM and the TSA are untouched. This is what the game does on core 0 (milestone M1).
        {
            IopMemory adma;
            uint8_t block[0x800];
            for (int i = 0; i < 0x400; ++i)
            {
                const int16_t value = static_cast<int16_t>(i < 0x100 ? 1000 + i : (i < 0x200 ? -2000 - i : (i < 0x300 ? 3000 + i : -4000 - i)));
                std::memcpy(block + i * 2, &value, 2); // quarters: L0, R0, L1, R1 (256 samples each)
            }
            check(adma.writeRam(0x30000u, block, sizeof(block)), "stage an AutoDMA block");
            setTsa(adma, 0, 0x4000u);
            adma.write16(kSpuBase + 0x1B0u, 1u); // REG_ADMAS: AutoDMA on
            adma.write32(kMadr0, 0x30000u);
            adma.write32(kBcr0, 0x200u); // 512 words = 2048 bytes
            adma.write32(kChcr0, 0x01000201u);
            check(std::all_of(adma.spuRam().begin(), adma.spuRam().end(), [](uint8_t b) { return b == 0; }), "AutoDMA does not write the sound RAM");
            check(adma.admaBlocks(0) == 1u, "one block queued");
            std::vector<int16_t> pcm;
            check(adma.takeAdmaPcm(0, pcm) == 1024u, "512 frames = 1024 samples");
            check(pcm[0] == 1000 && pcm[1] == -2000 - 0x100 && pcm[2] == 1001 && pcm[3] == -2000 - 0x101, "interleaved L,R from the [L 0x200][R 0x200] pairs");
            check(pcm[2 * 256] == 3000 + 0x200 && pcm[2 * 256 + 1] == -4000 - 0x300, "second pair of the block");
            std::vector<int16_t> again;
            check(adma.takeAdmaPcm(0, again) == 0u, "the queue is drained");
            adma.write16(kSpuBase + 0x1B0u, 0u); // AutoDMA off: back to a plain sound RAM transfer
            adma.write32(kMadr0, 0x30000u);
            adma.write32(kBcr0, 0x200u);
            adma.write32(kChcr0, 0x01000201u);
            check(adma.spuRam()[0x8000] == block[0] && adma.spuRam()[0x8001] == block[1], "plain DMA still writes the sound RAM");
        }

        memory.reset();
        check(std::all_of(memory.spuRam().begin(), memory.spuRam().end(), [](uint8_t b) { return b == 0; }), "reset clears the sound RAM");
        std::cout << "PASS " << checks << " SPU2 RAM checks\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
