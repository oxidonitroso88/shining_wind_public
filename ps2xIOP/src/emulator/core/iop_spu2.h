#pragma once

// SPU2 voice engine (audio milestone M2). Two cores, 24 ADPCM voices each, driven by the IOP cycle counter (768 IOP cycles per output
// sample = 36.864 MHz / 48 kHz), so the output is a deterministic function of the register writes and the IOP time.
//
// Scope of M2: ADPCM decoding with loop/end flags, pitch (linear interpolation), hardware ADSR, fixed volumes, KON/KOFF, ENDX/NAX/ENVX,
// dry mixing (VMIXL/VMIXR), master volumes, the AutoDMA PCM input of core 0 as a separate FIFO (kept apart from the voices), and the
// core 0 -> core 1 external input. Not emulated (counted and reported): reverb, noise, pitch modulation, volume sweeps.
// Register layout, flag semantics and the observed routing: docs/audio-m2-2026-10-13.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::iop::detail
{
    class Spu2
    {
    public:
        static constexpr uint32_t kCyclesPerSample = 768u;
        static constexpr uint32_t kVoices = 24u;
        static constexpr uint32_t kRegisterBytes = 0x800u; // offsets 0x000-0x7FF from 0x1F900000 (core 0, core 1, master volumes)

        explicit Spu2(std::vector<uint8_t> &ram) : m_ram(ram) { reset(); }

        void reset();
        // The IOP's running cycle counter. Without one, time only advances through renderSamples().
        void setCycleSource(const uint64_t *cycles) noexcept { m_cycles = cycles; }

        // Register write (16 bit). `offset` is the byte offset from 0x1F900000. Renders the samples due so far first, so a write takes
        // effect at the right sample.
        void write16(uint32_t offset, uint16_t value);
        // Dynamic registers (ENDX, NAX, ENVX, VOLX) as the hardware would show them now; false = not an engine register.
        bool readDynamic(uint32_t offset, uint16_t &value);

        // AutoDMA input (separate from the voices): interleaved L,R frames consumed one per output sample.
        // The AutoDMA stream is the emulated game's own clock: blocks arrive at the period of the block DMA plus the game's interrupt
        // latency, which differs from exactly 48 kHz of the IOP cycle clock by a few hundred ppm. With rate recovery (default) the
        // engine follows that rate from the queue level (first-order loop around kAdmaTargetFrames, linear interpolation) instead of
        // letting the queue run dry (one missing frame every few blocks = a click) or grow. Without it, one frame per output sample.
        void pushAdma(uint32_t core, const int16_t *interleaved, size_t samples);
        // Diagnostics (PS2X_SPU2_KON_LOG): one CSV row for a DMA to/from sound RAM or for an AutoDMA block ("dma", "adma", "adma-noise").
        void logDma(const char *kind, uint32_t core, uint32_t iopAddress, uint64_t bytes, bool toSpu, uint32_t tsaWords, uint32_t admas);
        void setAdmaRateRecovery(bool on) noexcept { m_admaRecover = on; }
        [[nodiscard]] double admaStep() const noexcept { return m_admaStep; }
        [[nodiscard]] size_t admaQueuedFrames() const noexcept { return m_adma[0].size() / 2u; }

        // Renders every sample due at the current IOP cycle (no-op without a cycle source).
        void catchUp();
        // Renders `count` output samples now (tests and offline use).
        void renderSamples(size_t count);
        // The mix is only stored while somebody collects it (a WAV writer, a test, later the audio device).
        void setCollecting(bool on) noexcept { m_collect = on; }
        // Moves the mixed output (interleaved L,R, 48 kHz) into `out`; returns the number of 16-bit samples.
        size_t takeMix(std::vector<int16_t> &out);
        // The same timeline as three separate views: voices only (AutoDMA muted) and AutoDMA only (voices muted), each through the
        // same core volumes and routing. Collected only while enabled.
        void setCollectingVoices(bool on) noexcept { m_collectVoices = on; }
        void setCollectingAdma(bool on) noexcept { m_collectAdma = on; }
        size_t takeVoices(std::vector<int16_t> &out);
        size_t takeAdmaMix(std::vector<int16_t> &out);

        struct Stats
        {
            uint64_t samples = 0, keyOns = 0, keyOffs = 0, voiceEnds = 0, admaUnderruns = 0, admaFrames = 0;
            uint64_t sweepVolumes = 0, noiseVoices = 0, reverbSends = 0, pitchModVoices = 0;
        };
        [[nodiscard]] const Stats &stats() const noexcept { return m_stats; }

        // Introspection for tests.
        [[nodiscard]] bool voiceActive(uint32_t core, uint32_t voice) const { return m_voices[core & 1u][voice % kVoices].active; }
        [[nodiscard]] uint32_t voiceEnvelope(uint32_t core, uint32_t voice) const { return m_voices[core & 1u][voice % kVoices].level; }
        [[nodiscard]] uint16_t reg(uint32_t offset) const { return m_regs[(offset / 2u) % (kRegisterBytes / 2u)]; }

    private:
        enum class Phase : uint8_t { Attack, Decay, Sustain, Release };

        struct Voice
        {
            bool active = false;
            uint32_t addr = 0;     // byte address of the block being played
            uint32_t loopAddr = 0; // byte address to jump to after an end+repeat block
            std::array<int16_t, 28> samples{};
            uint32_t pos = 28;
            uint8_t flags = 0;
            int32_t s1 = 0, s2 = 0; // ADPCM filter history
            int32_t cur = 0, next = 0;
            uint32_t counter = 0;   // 12.12 fixed point position between cur and next
            Phase phase = Phase::Attack;
            int32_t level = 0;      // envelope 0..0x7FFF
            uint32_t envCounter = 0;
        };

        uint16_t &r(uint32_t offset) { return m_regs[offset / 2u]; }
        [[nodiscard]] uint16_t r(uint32_t offset) const { return m_regs[offset / 2u]; }
        void keyOn(uint32_t core, uint32_t voice);
        void keyOff(uint32_t core, uint32_t voice);
        void decodeBlock(Voice &v);
        int32_t fetch(uint32_t core, uint32_t index);
        void stepEnvelope(uint32_t core, uint32_t index);
        void renderOne();
        static int32_t volume(uint16_t value);

        std::vector<uint8_t> &m_ram;
        const uint64_t *m_cycles = nullptr;
        uint64_t m_rendered = 0; // output samples produced so far
        std::array<uint16_t, kRegisterBytes / 2u> m_regs{};
        std::array<std::array<Voice, kVoices>, 2> m_voices{};
        uint32_t m_endx[2] = {0u, 0u};
        std::deque<int16_t> m_adma[2];
        bool m_admaRecover = true;
        double m_admaPos = 0.0;    // fractional read position inside m_adma[0], in frames
        double m_admaStep = 1.0;   // frames consumed per output sample
        double m_admaLevel = 0.0;  // smoothed queue level above the target, in frames
        bool m_admaPrimed = false; // playback starts once the queue holds kAdmaPrimeFrames
        bool m_underrun = false;
        std::vector<int16_t> m_out;
        bool m_collect = false, m_collectVoices = false, m_collectAdma = false;
        std::vector<int16_t> m_voicesOut, m_admaOut;
        Stats m_stats{};
    };
} // namespace ps2x::iop::detail
