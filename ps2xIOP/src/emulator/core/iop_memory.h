#pragma once

#include "iop_spu2.h"
#include "iop_sio2.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory
    {
    public:
        static constexpr uint32_t RamSize = 2u * 1024u * 1024u;
        static constexpr uint32_t ScratchBase = 0x1F800000u;
        static constexpr uint32_t ScratchSize = 0x400u;
        static constexpr uint32_t HardwareBase = 0x1F801000u;
        static constexpr uint32_t HardwareEnd = 0x1F900000u;
        static constexpr uint32_t Spu2Base = 0x1F900000u;
        static constexpr uint32_t Spu2End = 0x1FA00000u;
        static constexpr uint32_t SifBase = 0x1D000000u;
        static constexpr uint32_t SifEnd = 0x1D001000u;
        static constexpr uint32_t HeapBase = 0x0006C000u;
        static constexpr uint32_t HeapLimit = 0x001F0000u;

        struct Allocation
        {
            uint32_t address = 0;
            uint32_t size = 0;
        };

        struct DmaStart
        {
            int irq = 0;
            uint64_t delayCycles = 0;
        };

        IopMemory();
        ~IopMemory();

        void reset();

        [[nodiscard]] uint8_t read8(uint32_t address) const;
        [[nodiscard]] uint16_t read16(uint32_t address) const;
        [[nodiscard]] uint32_t read32(uint32_t address) const;
        void write8(uint32_t address, uint8_t value);
        void write16(uint32_t address, uint16_t value);
        void write32(uint32_t address, uint32_t value);

        [[nodiscard]] bool readRam(uint32_t address, void *destination, size_t size) const;
        [[nodiscard]] bool writeRam(uint32_t address, const void *source, size_t size);
        [[nodiscard]] bool zeroRam(uint32_t address, size_t size);
        [[nodiscard]] bool ownsRamRange(uint32_t address, size_t size) const;
        [[nodiscard]] bool isHardwareAddress(uint32_t address) const;
        [[nodiscard]] std::string readString(uint32_t address, size_t limit = 1024u) const;

        [[nodiscard]] uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt);
        [[nodiscard]] bool freeAllocation(uint32_t address);
        [[nodiscard]] uint32_t maxFreeMemory() const;   // largest contiguous free block
        [[nodiscard]] uint32_t totalFreeMemory() const; // sum of all free bytes in the heap
        [[nodiscard]] const std::vector<Allocation> &allocations() const noexcept { return m_allocations; }
        [[nodiscard]] std::optional<Allocation> allocationContaining(uint32_t address) const;

        [[nodiscard]] uint32_t interruptStatus() const noexcept { return m_interruptStatus; }
        [[nodiscard]] uint32_t interruptMask() const noexcept { return m_interruptMask; }
        [[nodiscard]] uint32_t interruptControl() const noexcept { return m_interruptControl; }
        void setInterruptStatus(uint32_t value) noexcept { m_interruptStatus = value; }
        void setInterruptMask(uint32_t value) noexcept { m_interruptMask = value; }
        void setInterruptControl(uint32_t value) noexcept { m_interruptControl = value & 1u; }

        [[nodiscard]] bool hasDmaStart() const noexcept { return m_dmaStart.has_value(); }
        [[nodiscard]] std::optional<DmaStart> takeDmaStart() noexcept;
        [[nodiscard]] std::span<const uint8_t> ram() const noexcept { return m_ram; }
        // SPU2 sound RAM (2 MB), filled by the SPU DMA channels and the CPU data port; the voices/mixer are not emulated yet.
        static constexpr uint32_t SpuRamSize = 2u * 1024u * 1024u;
        [[nodiscard]] std::span<const uint8_t> spuRam() const noexcept { return m_spuRam; }
        // AutoDMA (REG_ADMAS != 0) does not write the sound RAM: the SPU pulls the blocks as PCM input (milestone M1, verified on
        // the game: core 0 receives 2048 byte blocks every 10.67 ms = 48 kHz stereo 16-bit, laid out as pairs of 0x200 bytes
        // [left 256 samples][right 256 samples]). The decoded stream is queued here as interleaved L,R samples for the mixer.
        // Returns the number of 16-bit samples moved into `out` (appended). The queue keeps the newest ~10 s.
        size_t takeAdmaPcm(uint32_t core, std::vector<int16_t> &out);
        [[nodiscard]] uint64_t admaBlocks(uint32_t core) const noexcept { return m_admaBlocks[core & 1u]; }

        [[nodiscard]] static uint32_t physicalAddress(uint32_t address) noexcept;

        void setPadSource(PadSource source) { m_sio2.setPadSource(std::move(source)); }
        // The IOP cycle counter that clocks the SPU2 voice engine (768 cycles per output sample).
        void setCycleSource(const uint64_t *cycles) noexcept { m_spu2.setCycleSource(cycles); m_cycleSource = cycles; }
        // Diagnostics: the CPU reports the address of the instruction it executes so PS2X_IOP_WATCH can name the writer.
        void setExecPc(uint32_t pc) noexcept { m_execPc = pc; }
        [[nodiscard]] uint64_t diagCycles() const noexcept { return m_cycleSource ? *m_cycleSource : 0u; }
        [[nodiscard]] Spu2 &spu2() noexcept { return m_spu2; }

    private:
        [[nodiscard]] uint32_t readHardware32(uint32_t address) const;
        void writeHardware32(uint32_t address, uint32_t value);
        void markOwned(uint32_t address, size_t size);
        void watchWrite(uint32_t physical, size_t size, const char *kind);
        void flushWatchRun();
        const uint64_t *m_cycleSource = nullptr;
        uint32_t m_execPc = 0u;
        uint32_t m_watchLo = 0u, m_watchHi = 0u; // PS2X_IOP_WATCH=lo-hi (hex, physical IOP RAM)
        uint64_t m_watchFrom = 0u;
        struct WatchRun { uint32_t pc = 0, first = 0, next = 0, n = 0; uint64_t cyc0 = 0, cyc1 = 0; const char *kind = nullptr; uint32_t v0 = 0; } m_watchRun;
        uint32_t m_watchLines = 0u;
        void spu2Write16(uint32_t physical, uint16_t value);
        void spu2Dma(uint32_t chcrAddress, uint32_t chcr, uint64_t transferWords);
        void collectSio2Completion();
        [[nodiscard]] std::vector<Allocation> sortedAllocations() const;

        std::vector<uint8_t> m_ram;
        std::vector<uint8_t> m_owned;
        std::vector<uint8_t> m_scratch;
        std::unordered_map<uint32_t, uint32_t> m_hardware;
        std::vector<uint8_t> m_spuRam = std::vector<uint8_t>(SpuRamSize, uint8_t{0});
        std::vector<int16_t> m_admaPcm[2];
        uint64_t m_admaBlocks[2] = {0u, 0u};
        uint32_t m_spuTransferAddress[2] = {0u, 0u}; // TSA of each core, in 16-bit units
        mutable Spu2 m_spu2{m_spuRam};
        void pumpMixWav();
        // Diagnostics: an AutoDMA block of pure silence right after audible ones (the game ran out of PCM). Consumed by the emulator.
        uint32_t m_audibleRun = 0u;
        uint64_t m_zeroBlockEvents = 0u;
        uint32_t m_noiseRun = 0u;
        uint64_t m_noiseEvents = 0u, m_noiseSeen = 0u;
        uint64_t m_zeroBlockSeen = 0u;
    public:
        [[nodiscard]] bool takeZeroBlockEvent() noexcept
        {
            const bool fresh = m_zeroBlockEvents != m_zeroBlockSeen;
            m_zeroBlockSeen = m_zeroBlockEvents;
            return fresh;
        }
        void pumpAudioOutput(); // renders due SPU2 samples and delivers them to the audio sink / WAV writers
        [[nodiscard]] bool takeNoiseEvent() noexcept { const bool fresh = m_noiseEvents != m_noiseSeen; m_noiseSeen = m_noiseEvents; return fresh; }
        [[nodiscard]] uint64_t admaBlockCount() const noexcept { return m_admaBlocks[0]; }
    private:
        std::vector<Allocation> m_allocations;
        uint32_t m_heapCursor = HeapBase;
        uint32_t m_interruptStatus = 0;
        uint32_t m_interruptMask = 0;
        uint32_t m_interruptControl = 1;
        std::optional<DmaStart> m_dmaStart;
        mutable Sio2 m_sio2;
    };
}
