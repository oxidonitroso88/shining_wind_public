#include "iop_sio2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // Register offsets from 0x1F808200
        constexpr uint32_t kSend3End = 0x40u;
        constexpr uint32_t kPortControlBase = 0x40u;
        constexpr uint32_t kPortControlEnd = 0x60u;
        constexpr uint32_t kFifoOut = 0x60u; // IOP -> device bytes (DATA_OUT in the SDK naming)
        constexpr uint32_t kFifoIn = 0x64u;  // device -> IOP bytes (DATA_IN)
        constexpr uint32_t kCtrl = 0x68u;
        constexpr uint32_t kRecv1 = 0x6Cu;
        constexpr uint32_t kRecv2 = 0x70u;
        constexpr uint32_t kRecv3 = 0x74u;
        constexpr uint32_t kUnknown78 = 0x78u;
        constexpr uint32_t kUnknown7c = 0x7Cu;
        constexpr uint32_t kIstat = 0x80u;

        constexpr uint32_t kRecv1Connected = 0x1100u;

        uint64_t g_traceCount = 0;
        constexpr uint64_t kTraceLimit = 4000;

        bool tracing()
        {
            static const bool enabled = std::getenv("PS2X_SIO2_LOG") != nullptr;
            return enabled && g_traceCount < kTraceLimit;
        }
    }

    bool Sio2::traceEnabled()
    {
        return tracing();
    }

    void Sio2::reset()
    {
        m_send3.fill(0u);
        m_portControl.fill(0u);
        m_ctrl = m_recv1 = m_recv2 = m_recv3 = m_istat = m_unknown78 = m_unknown7c = 0u;
        m_tx.clear();
        m_rx.clear();
        m_pads = {};
        m_completion.reset();
        m_polls = 0;
    }

    std::optional<Sio2::Completion> Sio2::takeCompletion() noexcept
    {
        std::optional<Completion> result = m_completion;
        m_completion.reset();
        return result;
    }

    uint32_t Sio2::read(uint32_t address, unsigned width)
    {
        const uint32_t offset = address - RegisterBase;
        uint32_t value = 0u;
        if (offset == kFifoOut || offset == kFifoIn)
        {
            if (!m_rx.empty())
            {
                value = m_rx.front();
                m_rx.pop_front();
            }
            else
                value = 0xFFu;
            return value;
        }

        const uint32_t aligned = offset & ~3u;
        if (aligned < kSend3End)
            value = m_send3[aligned / 4u];
        else if (aligned < kPortControlEnd)
            value = m_portControl[(aligned - kPortControlBase) / 4u];
        else
            switch (aligned)
            {
            case kCtrl: value = m_ctrl; break;
            case kRecv1: value = m_recv1; break;
            case kRecv2: value = m_recv2; break;
            case kRecv3: value = m_recv3; break;
            case kUnknown78: value = m_unknown78; break;
            case kUnknown7c: value = m_unknown7c; break;
            case kIstat: value = m_istat; break;
            default: break;
            }
        if (width < 4u)
            value = (value >> ((offset & 3u) * 8u)) & ((1u << (width * 8u)) - 1u);
        return value;
    }

    void Sio2::write(uint32_t address, unsigned width, uint32_t value)
    {
        const uint32_t offset = address - RegisterBase;
        static uint32_t registerWrites = 0u;
        if (tracing() && registerWrites < 60u && offset != kFifoOut && offset != kFifoIn)
            std::fprintf(stderr, "[SIO2] write +0x%02x w%u = 0x%x\n", offset, width, value), ++registerWrites, ++g_traceCount;
        if (offset == kFifoOut || offset == kFifoIn)
        {
            m_tx.push_back(static_cast<uint8_t>(value));
            return;
        }

        const uint32_t aligned = offset & ~3u;
        uint32_t *target = nullptr;
        if (aligned < kSend3End)
            target = &m_send3[aligned / 4u];
        else if (aligned < kPortControlEnd)
            target = &m_portControl[(aligned - kPortControlBase) / 4u];
        else
            switch (aligned)
            {
            case kCtrl: target = &m_ctrl; break;
            case kIstat: target = &m_istat; break;
            case kUnknown78: target = &m_unknown78; break;
            case kUnknown7c: target = &m_unknown7c; break;
            default: break;
            }
        if (!target)
            return;
        if (width < 4u)
        {
            const uint32_t shift = (offset & 3u) * 8u;
            const uint32_t mask = ((1u << (width * 8u)) - 1u) << shift;
            value = (*target & ~mask) | ((value << shift) & mask);
        }
        *target = value;

        if (aligned == kCtrl)
        {
            if ((value & 1u) != 0u)
            {
                // The SDK keeps the reset bits (0xC) set while starting, so a start takes priority over a reset.
                m_ctrl &= ~1u;
                startTransfer();
            }
            else if ((value & 0xCu) != 0u)
            {
                // Reset of the transfer state machine: drop both FIFOs.
                m_tx.clear();
                m_rx.clear();
            }
        }
    }

    void Sio2::startTransfer()
    {
        m_rx.clear();
        m_recv1 = kRecv1Connected;
        size_t totalBytes = 0u;
        for (size_t frame = 0; frame < m_send3.size(); ++frame)
        {
            const uint32_t descriptor = m_send3[frame];
            if (descriptor == 0u)
                break;
            const int port = static_cast<int>(descriptor & 3u);
            const size_t txCount = (descriptor >> 8u) & 0x1FFu;
            const size_t rxCount = (descriptor >> 18u) & 0x1FFu;

            std::deque<uint8_t> tx;
            for (size_t i = 0; i < txCount; ++i)
            {
                if (m_tx.empty())
                {
                    tx.push_back(0u);
                    continue;
                }
                tx.push_back(m_tx.front());
                m_tx.pop_front();
            }

            std::deque<uint8_t> rx;
            const uint8_t device = tx.empty() ? 0u : tx.front();
            if (device == 0x01u)
                padFrame(port, tx, rx);
            else
            {
                // Memory cards (0x81), multitaps (0x21) and anything else: nothing attached.
                markDisconnected(port);
                rx.assign(std::max(txCount, rxCount), 0xFFu);
            }
            rx.resize(std::max(rxCount, rx.size()), 0u);
            rx.resize(rxCount);
            for (uint8_t byte : rx)
                m_rx.push_back(byte);
            totalBytes += std::max(txCount, rxCount);

            if (tracing())
            {
                // Log every frame at the start, then only frames whose reply changed (button changes, mode changes).
                static uint32_t shown = 0u;
                static std::deque<uint8_t> lastReply[4];
                const bool changed = port < 0 || port > 3 || lastReply[port] != rx;
                if (shown < 12u || changed)
                {
                    ++shown;
                    if (port >= 0 && port <= 3)
                        lastReply[port] = rx;
                    std::fprintf(stderr, "[SIO2] frame %zu send3=0x%x port=%d tx(%zu):", frame, descriptor, port, txCount);
                    for (uint8_t b : tx)
                        std::fprintf(stderr, " %02x", b);
                    std::fprintf(stderr, " -> rx(%zu):", rx.size());
                    for (uint8_t b : rx)
                        std::fprintf(stderr, " %02x", b);
                    std::fprintf(stderr, " recv1=0x%x\n", m_recv1);
                    ++g_traceCount;
                }
            }
        }
        m_tx.clear();
        m_recv2 = 0xFu;
        m_recv3 = 0u;
        m_istat = 0u;
        // Roughly 16 us per byte on the serial link plus a fixed setup cost, in IOP cycles (36.864 MHz).
        m_completion = Completion{Irq, 2000u + totalBytes * 600u};
    }

    void Sio2::markDisconnected(int port)
    {
        // PADMAN tests bit (16 + port) of RECV1 to learn that a frame found no device on that port; the low bits keep
        // the "transfer completed" pattern. A frame that does answer leaves its bit clear, so a transfer that polls
        // a present pad on port 0 and an empty port 1 reports port 0 as healthy.
        m_recv1 |= (0x10000u << (port & 3)) | 0xD000u;
    }

    void Sio2::padFrame(int port, const std::deque<uint8_t> &tx, std::deque<uint8_t> &rx)
    {
        const size_t size = std::max<size_t>(tx.size(), 3u);
        if (port < 0 || port > 3 || port != 0)
        {
            markDisconnected(port);
            rx.assign(size, 0xFFu);
            return;
        }
        PadState &pad = m_pads[static_cast<size_t>(port)];
        PadSample sample;
        if (m_padSource)
            (void)m_padSource(port, sample);

        const uint8_t command = tx.size() > 1 ? tx[1] : 0u;
        const auto arg = [&tx](size_t index) -> uint8_t { return index < tx.size() ? tx[index] : 0u; };

        const uint8_t modeId = pad.analog ? (pad.pressure ? 0x79u : 0x73u) : 0x41u;
        std::vector<uint8_t> body; // bytes after the 3 byte header
        uint8_t id = pad.config ? 0xF3u : modeId;

        const auto pollData = [&]() {
            body.push_back(static_cast<uint8_t>(sample.buttons & 0xFFu));
            body.push_back(static_cast<uint8_t>(sample.buttons >> 8u));
            if (pad.analog)
            {
                for (uint8_t stick : sample.sticks)
                    body.push_back(stick);
                if (pad.pressure)
                {
                    const uint16_t b = sample.buttons;
                    const auto pressed = [&b](uint16_t mask) { return static_cast<uint8_t>((b & mask) == 0u ? 0xFFu : 0x00u); };
                    for (uint16_t mask : {0x0020u, 0x0080u, 0x0010u, 0x0040u, 0x1000u, 0x2000u, 0x4000u, 0x8000u, 0x0400u, 0x0800u, 0x0100u, 0x0200u})
                        body.push_back(pressed(mask));
                }
            }
        };

        switch (command)
        {
        case 0x42u: // read data / poll
            pollData();
            break;
        case 0x43u: // enter / exit configuration mode
            if (pad.config)
            {
                body.assign(6u, 0u);
                if (arg(3) == 0u)
                    pad.config = false;
            }
            else
            {
                pollData();
                if (arg(3) == 1u)
                    pad.config = true;
            }
            break;
        case 0x44u: // set mode (digital / analog) and lock
            body.assign(6u, 0u);
            pad.analog = arg(3) == 1u;
            pad.locked = arg(4) == 3u;
            break;
        case 0x45u: // query model
            body = {0x03u, 0x02u, static_cast<uint8_t>(pad.analog ? 1u : 0u), 0x02u, 0x01u, 0x00u};
            break;
        case 0x46u: // query act
            body = arg(3) == 0u ? std::vector<uint8_t>{0x00u, 0x00u, 0x01u, 0x02u, 0x00u, 0x0Au}
                                : std::vector<uint8_t>{0x00u, 0x00u, 0x01u, 0x01u, 0x01u, 0x14u};
            break;
        case 0x47u: // query comb
            body = {0x00u, 0x00u, 0x02u, 0x00u, 0x01u, 0x00u};
            break;
        case 0x4Cu: // query mode
            body = arg(3) == 0u ? std::vector<uint8_t>{0x00u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u}
                                : std::vector<uint8_t>{0x00u, 0x00u, 0x00u, 0x07u, 0x00u, 0x00u};
            break;
        case 0x4Du: // vibration mapping
            body.assign(pad.rumbleMap.begin(), pad.rumbleMap.end());
            for (size_t i = 0; i < pad.rumbleMap.size(); ++i)
                pad.rumbleMap[i] = arg(3 + i);
            break;
        case 0x4Fu: // pressure mode
            body = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x5Au};
            pad.analog = true;
            pad.pressure = (arg(5) & 3u) == 3u;
            break;
        default:
            body.assign(6u, 0u);
            break;
        }

        rx.clear();
        rx.push_back(0xFFu);
        rx.push_back(id);
        rx.push_back(0x5Au);
        for (uint8_t byte : body)
            rx.push_back(byte);
        ++m_polls;
    }
}
