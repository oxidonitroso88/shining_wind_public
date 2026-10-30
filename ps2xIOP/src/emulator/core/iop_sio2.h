#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

namespace ps2x::iop::detail
{
    // Host controller sample in PS2 wire format: buttons are active low (bit set = released), sticks are 0..255
    // with 0x80 centred, in the order right X, right Y, left X, left Y.
    struct PadSample
    {
        uint16_t buttons = 0xFFFFu;
        std::array<uint8_t, 4> sticks{0x80u, 0x80u, 0x80u, 0x80u};
    };

    using PadSource = std::function<bool(int port, PadSample &sample)>;

    // SIO2 serial controller (registers 0x1F808200..0x1F8082FF) with a DualShock 2 attached to port 0.
    // The real SIO2MAN/PADMAN modules program SEND3 frames, push command bytes into the FIFO, start the transfer
    // through CTRL and wait for the SIO2 interrupt (IRQ 17); this class answers those frames.
    class Sio2
    {
    public:
        static constexpr uint32_t RegisterBase = 0x1F808200u;
        static constexpr uint32_t RegisterEnd = 0x1F808300u;
        static constexpr int Irq = 17;

        struct Completion
        {
            int irq = Irq;
            uint64_t delayCycles = 0;
        };

        void reset();
        void setPadSource(PadSource source) { m_padSource = std::move(source); }

        [[nodiscard]] uint32_t read(uint32_t address, unsigned width);
        void write(uint32_t address, unsigned width, uint32_t value);
        [[nodiscard]] std::optional<Completion> takeCompletion() noexcept;

        [[nodiscard]] static bool contains(uint32_t physical) noexcept { return physical >= RegisterBase && physical < RegisterEnd; }

    private:
        struct PadState
        {
            bool config = false;
            bool analog = false;
            bool pressure = false;
            bool locked = false;
            std::array<uint8_t, 6> rumbleMap{0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
        };

        void startTransfer();
        void markDisconnected(int port);
        void padFrame(int port, const std::deque<uint8_t> &tx, std::deque<uint8_t> &rx);
        [[nodiscard]] static bool traceEnabled();

        std::array<uint32_t, 16> m_send3{};
        std::array<uint32_t, 8> m_portControl{};
        uint32_t m_ctrl = 0;
        uint32_t m_recv1 = 0;
        uint32_t m_recv2 = 0;
        uint32_t m_recv3 = 0;
        uint32_t m_istat = 0;
        uint32_t m_unknown78 = 0;
        uint32_t m_unknown7c = 0;
        std::deque<uint8_t> m_tx;
        std::deque<uint8_t> m_rx;
        std::array<PadState, 4> m_pads{};
        PadSource m_padSource;
        std::optional<Completion> m_completion;
        uint64_t m_polls = 0;
    };
}
