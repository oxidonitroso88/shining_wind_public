// SIO2 serial controller + DualShock 2 model tests. The register sequences below mirror what the real SIO2MAN/PADMAN
// modules do (observed in Shining Wind): program SEND3 frames, push command bytes to the FIFO, start through CTRL,
// wait for IRQ 17, then read RECV1 and the reply bytes.

#include "emulator/core/iop_sio2.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using ps2x::iop::detail::PadSample;
using ps2x::iop::detail::Sio2;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool condition, const char *what)
    {
        ++g_checks;
        if (!condition)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what);
        }
    }

    constexpr uint32_t kBase = Sio2::RegisterBase;
    constexpr uint32_t kSend3 = kBase + 0x00u;
    constexpr uint32_t kFifo = kBase + 0x60u;
    constexpr uint32_t kCtrl = kBase + 0x68u;
    constexpr uint32_t kRecv1 = kBase + 0x6Cu;

    struct Transfer
    {
        std::vector<std::vector<uint8_t>> rx;
        uint32_t recv1 = 0;
        bool irq = false;
    };

    // frames: {port, tx bytes}. The reply of each frame has the same length as its command.
    Transfer run(Sio2 &sio2, const std::vector<std::pair<int, std::vector<uint8_t>>> &frames)
    {
        sio2.write(kCtrl, 4, 0x3BCu);
        uint32_t index = 0;
        for (const auto &[port, tx] : frames)
        {
            const uint32_t length = static_cast<uint32_t>(tx.size());
            sio2.write(kSend3 + 4u * index, 4, (length << 18) | (length << 8) | 0x40u | static_cast<uint32_t>(port));
            ++index;
        }
        sio2.write(kSend3 + 4u * index, 4, 0u);
        for (const auto &frame : frames)
            for (uint8_t byte : frame.second)
                sio2.write(kFifo, 1, byte);
        sio2.write(kCtrl, 4, 0x3BDu); // reset bits stay set while starting
        Transfer result;
        const auto completion = sio2.takeCompletion();
        result.irq = completion.has_value() && completion->irq == Sio2::Irq && completion->delayCycles > 0u;
        result.recv1 = sio2.read(kRecv1, 4);
        for (const auto &frame : frames)
        {
            std::vector<uint8_t> reply;
            for (size_t i = 0; i < frame.second.size(); ++i)
                reply.push_back(static_cast<uint8_t>(sio2.read(kFifo + 4u, 1)));
            result.rx.push_back(reply);
        }
        return result;
    }

    const std::vector<uint8_t> kPoll5 = {0x01, 0x42, 0x00, 0x00, 0x00};

    void testIdlePollAndDisconnectedPort()
    {
        Sio2 sio2;
        sio2.reset();
        const Transfer t = run(sio2, {{0, kPoll5}, {1, kPoll5}});
        check(t.irq, "transfer completes with IRQ 17");
        check(t.rx[0] == std::vector<uint8_t>({0xFF, 0x41, 0x5A, 0xFF, 0xFF}), "digital idle poll reply");
        check(t.rx[1] == std::vector<uint8_t>(5, 0xFF), "empty port replies 0xFF");
        check((t.recv1 & 0x10000u) == 0u, "connected port 0 has no error bit in RECV1");
        check((t.recv1 & 0x20000u) != 0u, "empty port 1 sets its error bit in RECV1");
    }

    void testButtonsReachTheReply()
    {
        Sio2 sio2;
        sio2.reset();
        PadSample sample;
        sample.buttons = 0xBFF7u; // Start + Cross pressed (active low)
        sample.sticks = {0x10, 0x20, 0x30, 0x40};
        sio2.setPadSource([&sample](int port, PadSample &out) {
            if (port != 0)
                return false;
            out = sample;
            return true;
        });
        const Transfer digital = run(sio2, {{0, kPoll5}});
        check(digital.rx[0] == std::vector<uint8_t>({0xFF, 0x41, 0x5A, 0xF7, 0xBF}), "digital reply carries buttons");

        // Enter config, switch to analog, leave config.
        (void)run(sio2, {{0, {0x01, 0x43, 0x00, 0x01, 0x00}}});
        const Transfer mode = run(sio2, {{0, {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00}}});
        check(mode.rx[0][1] == 0xF3, "config mode id is 0xF3");
        (void)run(sio2, {{0, {0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}}});
        const Transfer analog = run(sio2, {{0, {0x01, 0x42, 0, 0, 0, 0, 0, 0, 0}}});
        check(analog.rx[0] == std::vector<uint8_t>({0xFF, 0x73, 0x5A, 0xF7, 0xBF, 0x10, 0x20, 0x30, 0x40}), "analog reply carries sticks");
    }

    void testQueriesAndPressureMode()
    {
        Sio2 sio2;
        sio2.reset();
        (void)run(sio2, {{0, {0x01, 0x43, 0x00, 0x01, 0x00}}});
        const Transfer model = run(sio2, {{0, {0x01, 0x45, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}}});
        check(model.rx[0] == std::vector<uint8_t>({0xFF, 0xF3, 0x5A, 0x03, 0x02, 0x00, 0x02, 0x01, 0x00}), "query model reports a DualShock 2");
        const Transfer act0 = run(sio2, {{0, {0x01, 0x46, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}}});
        check(act0.rx[0][5] == 0x01 && act0.rx[0][6] == 0x02 && act0.rx[0][8] == 0x0A, "query act 0");
        (void)run(sio2, {{0, {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00}}});
        (void)run(sio2, {{0, {0x01, 0x4F, 0x00, 0xFF, 0xFF, 0x03, 0x00, 0x00, 0x00}}});
        (void)run(sio2, {{0, {0x01, 0x43, 0x00, 0x00, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A}}});
        std::vector<uint8_t> poll(21, 0u);
        poll[0] = 0x01;
        poll[1] = 0x42;
        const Transfer pressure = run(sio2, {{0, poll}});
        check(pressure.rx[0][1] == 0x79, "pressure mode id is 0x79");
        check(pressure.rx[0].size() == 21, "pressure reply has 21 bytes");
    }

    void testMemoryCardFrameIsAbsent()
    {
        Sio2 sio2;
        sio2.reset();
        const Transfer t = run(sio2, {{0, {0x81, 0x11, 0x00, 0x00}}});
        check(t.rx[0] == std::vector<uint8_t>(4, 0xFF), "memory card frame replies 0xFF");
        check((t.recv1 & 0x10000u) != 0u, "memory card frame flags an error");
    }
}

int main()
{
    testIdlePollAndDisconnectedPort();
    testButtonsReachTheReply();
    testQueriesAndPressureMode();
    testMemoryCardFrameIsAbsent();
    std::printf("SIO2 tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
