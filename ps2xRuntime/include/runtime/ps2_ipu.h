#ifndef PS2_IPU_H
#define PS2_IPU_H

// PlayStation 2 Image Processing Unit (IPU) emulation.
//
// The IPU is the MPEG-2 helper of the EE: a bit-stream reader fed through the IN FIFO (DMA channel 4,
// IPU_TO, or the 128-bit port at 0x10007010), a set of commands (macroblock decoding, VLC decoding,
// colour space conversion, ...) and an OUT FIFO (DMA channel 3, IPU_FROM, or the 128-bit port at 0x10007000).
//
// This class models the unit itself and is independent from the rest of the runtime: DMA is attached
// through two callbacks, interrupts are reported through a third one. Everything is synchronous: a command
// runs as far as the available input and output allow whenever something changes (register access, FIFO
// access, DMA kick), and simply stays pending (BUSY) otherwise, exactly as the hardware stalls.
//
// The implementation is written from the MPEG-2 specification (ISO/IEC 13818-2) and from the behaviour
// documented in the "EE User's Manual"; Play! and PCSX2 were used only as technical references to contrast
// the observable behaviour (register layouts, result formats of VDEC/FDEC, output ordering of BDEC).

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace ps2
{
    class Ipu
    {
    public:
        static constexpr uint32_t kRegisterBase = 0x10002000u;
        static constexpr uint32_t kRegisterEnd = 0x10002040u;
        static constexpr uint32_t kOutFifoAddress = 0x10007000u;
        static constexpr uint32_t kInFifoAddress = 0x10007010u;
        static constexpr size_t kFifoQwords = 8u;

        enum Command : uint32_t
        {
            kBclr = 0,
            kIdec = 1,
            kBdec = 2,
            kVdec = 3,
            kFdec = 4,
            kSetiq = 5,
            kSetvq = 6,
            kCsc = 7,
            kPack = 8,
            kSetth = 9,
        };

        // IPU_CTRL bit layout.
        static constexpr uint32_t kCtrlIfcShift = 0u;  // [3:0]  IN FIFO count (read only)
        static constexpr uint32_t kCtrlOfcShift = 4u;  // [7:4]  OUT FIFO count (read only)
        static constexpr uint32_t kCtrlCbpShift = 8u;  // [13:8] coded block pattern of the last macroblock
        static constexpr uint32_t kCtrlEcd = 1u << 14; // end code detected / VLC error
        static constexpr uint32_t kCtrlScd = 1u << 15; // start code detected
        static constexpr uint32_t kCtrlIdpShift = 16u; // [17:16] intra DC precision
        static constexpr uint32_t kCtrlAs = 1u << 20;  // alternate scan
        static constexpr uint32_t kCtrlIvf = 1u << 21; // intra VLC format (table B.15)
        static constexpr uint32_t kCtrlQst = 1u << 22; // non linear quantiser scale
        static constexpr uint32_t kCtrlMp1 = 1u << 23; // MPEG-1 bit stream
        static constexpr uint32_t kCtrlPctShift = 24u; // [26:24] picture coding type (1=I 2=P 3=B 4=D)
        static constexpr uint32_t kCtrlRst = 1u << 30; // reset (write only)
        static constexpr uint32_t kCtrlBusy = 1u << 31;

        // DMA hooks. The source returns how many quadwords it copied (at most maxQwords); a return of 0 means
        // "nothing available right now" (channel idle or finished). The sink returns how many quadwords it
        // accepted.
        using DmaSource = std::function<size_t(uint8_t *dst, size_t maxQwords)>;
        using DmaSink = std::function<size_t(const uint8_t *src, size_t qwords)>;
        using InterruptHook = std::function<void()>;

        Ipu();

        void setDmaSource(DmaSource source) { m_source = std::move(source); }
        void setDmaSink(DmaSink sink) { m_sink = std::move(sink); }
        void setInterruptHook(InterruptHook hook) { m_interrupt = std::move(hook); }

        // Power-on reset (also clears the quantiser matrices and the VQ CLUT).
        void reset();

        // Register interface. `offset` is relative to 0x10002000 (CMD=0x00, CTRL=0x10, BP=0x20, TOP=0x30).
        uint32_t readRegister(uint32_t offset);
        void writeRegister(uint32_t offset, uint32_t value);

        // Direct FIFO ports (128-bit accesses). writeInFifo returns false when the FIFO is full.
        bool writeInFifo(const uint8_t *qword);
        bool readOutFifo(uint8_t *qword);

        // Runs pending work until nothing more can progress. Call after any external event (DMA kick,
        // channel state change).
        void run();

        bool busy() const { return m_busy; }
        uint32_t ctrl() const;
        uint32_t bitPosition() const { return m_bp; }
        uint32_t inFifoQwords() const { return static_cast<uint32_t>(m_fifo.size() > kFifoQwords ? kFifoQwords : m_fifo.size()); }
        uint32_t outFifoQwords() const;

        // Diagnostics.
        uint64_t commandCount() const { return m_commandCount; }
        uint64_t macroblockCount() const { return m_macroblockCount; }
        const std::array<uint8_t, 64> &intraMatrix() const { return m_intraMatrix; }
        const std::array<uint8_t, 64> &nonIntraMatrix() const { return m_nonIntraMatrix; }

        // Tables and kernels exposed for tests.
        static void inverseDct8x8(const int16_t *coefficients, int16_t *samples);
        static void yuv420ToRgb32(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, uint32_t th0, uint32_t th1, uint8_t *rgba);

    private:
        struct Snapshot
        {
            std::array<uint8_t, 32> window;
            uint32_t fp;
            uint32_t bp;
            std::deque<std::array<uint8_t, 16>> fifo;
            int32_t dc[3];
        };

        void runLoop();

        // --- bit stream -------------------------------------------------------------------------------
        void pullInput();
        bool loadWindow(uint32_t bitsNeeded);
        bool peekBits(uint32_t count, uint32_t &value);
        bool skipBits(uint32_t count);
        bool getBits(uint32_t count, uint32_t &value);
        bool readBytes(uint8_t *dst, uint32_t count);
        int peekVlc(const void *lut, int32_t &value, uint32_t &length);
        void normalizeWindow();
        void beginAttempt();
        void commitAttempt();
        void rollbackAttempt();
        void clearInput();
        uint32_t availableBits() const { return m_fp * 128u > m_bp ? m_fp * 128u - m_bp : 0u; }

        // --- commands ---------------------------------------------------------------------------------
        void startCommand(uint32_t value);
        bool stepCommand();
        void finishCommand(bool raiseInterrupt = true);
        bool skipFirstBits();
        void raiseEnd(bool startCode);
        bool stepBdec();
        bool stepIdec();
        bool stepVdec();
        bool stepFdec();
        bool stepSetiq();
        bool stepSetvq();
        bool stepCsc();
        bool stepPack();
        int decodeBlock(uint32_t blockIndex, bool intra, uint32_t quantiserScale, int32_t *block);
        int decodeMacroblock(bool intra, uint32_t cbp, bool fieldDct, uint32_t quantiserScale, int16_t *out384);

        // --- output -----------------------------------------------------------------------------------
        bool pushOutput();
        void appendOutput(const uint8_t *data, size_t bytes);
        bool outputHasRoom() const { return m_out.size() <= kFifoQwords * 16u; }

        // --- state ------------------------------------------------------------------------------------
        DmaSource m_source;
        DmaSink m_sink;
        InterruptHook m_interrupt;

        std::deque<std::array<uint8_t, 16>> m_fifo;
        std::array<uint8_t, 32> m_window{};
        uint32_t m_fp = 0;
        uint32_t m_bp = 0;
        bool m_attempting = false;
        Snapshot m_snapshot;
        std::vector<std::array<uint8_t, 16>> m_attemptPulled;

        std::deque<uint8_t> m_out;

        uint32_t m_ctrl = 0; // stored bits (IDP, AS, IVF, QST, MP1, PCT, ECD, SCD)
        uint32_t m_cbp = 0;
        uint32_t m_top = 0;
        uint32_t m_cmdData = 0;
        uint32_t m_lastCommand = 0xFFFFFFFFu;
        bool m_busy = false;
        bool m_running = false;

        // Current command.
        uint32_t m_cmdId = 0;
        uint32_t m_cmdOption = 0;
        uint32_t m_cmdPhase = 0;
        uint32_t m_mbRemaining = 0;
        uint32_t m_quantiserScaleCode = 0;

        // Decoder state.
        std::array<uint8_t, 64> m_intraMatrix{};
        std::array<uint8_t, 64> m_nonIntraMatrix{};
        std::array<uint16_t, 16> m_vqClut{};
        uint32_t m_th0 = 0;
        uint32_t m_th1 = 0;
        int32_t m_dcPredictor[3] = {128, 128, 128};

        uint64_t m_commandCount = 0;
        uint64_t m_macroblockCount = 0;
    };
} // namespace ps2

#endif // PS2_IPU_H
