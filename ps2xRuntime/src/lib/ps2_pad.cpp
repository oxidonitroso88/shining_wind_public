#include "runtime/ps2_pad.h"
#include "runtime/pad_input.h"
#include "ps2_host_backend.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    // PS2X_PAD_SCRIPT="start@40-41.5,cross@45-46": presses the named buttons between the given host seconds counted from
    // the first pad poll. Test aid for reaching later screens (menus) inside a bounded observation.
    // Analog sticks can be scripted too: lleft/lright/lup/ldown (left stick) and rleft/rright/rup/rdown (right stick) push the
    // stick fully in that direction (byte 0x00 or 0xFF in the DualShock report: 4=RX 5=RY 6=LX 7=LY).
    struct ScriptedPress
    {
        uint16_t mask;
        double from;
        double to;
        int stickByte = -1;
        uint8_t stickValue = 0x80;
    };

    // Diagnostic inputs belong to one port: PS2X_PAD_SCRIPT / PS2X_PAD_FILE are the pad of player 1 (port 0, as before), PS2X_PAD2_SCRIPT / PS2X_PAD2_FILE
    // the pad of player 2 (port 1). Nothing scripted for a port is ever sent to the other one.
    const char *scriptEnvName(int port) { return port == 0 ? "PS2X_PAD_SCRIPT" : "PS2X_PAD2_SCRIPT"; }
    const char *fileEnvName(int port) { return port == 0 ? "PS2X_PAD_FILE" : "PS2X_PAD2_FILE"; }

    std::vector<ScriptedPress> parseScript(const char *env)
    {
        {
            std::vector<ScriptedPress> v;
            if (!env)
                return v;
            struct Name
            {
                const char *name;
                uint16_t mask;
            };
            static const Name names[] = {{"left", PAD_LEFT},     {"down", PAD_DOWN},     {"right", PAD_RIGHT}, {"up", PAD_UP},         {"start", PAD_START},
                                         {"select", PAD_SELECT}, {"square", PAD_SQUARE}, {"cross", PAD_CROSS}, {"circle", PAD_CIRCLE}, {"triangle", PAD_TRIANGLE},
                                         {"r1", PAD_R1},         {"l1", PAD_L1},         {"r2", PAD_R2},       {"l2", PAD_L2}};
            const std::string spec(env);
            size_t at = 0;
            while (at < spec.size())
            {
                size_t end = spec.find(',', at);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string item = spec.substr(at, end - at);
                at = end + 1;
                const size_t atSign = item.find('@');
                const size_t dash = item.find('-', atSign == std::string::npos ? 0 : atSign);
                if (atSign == std::string::npos || dash == std::string::npos)
                    continue;
                const std::string name = item.substr(0, atSign);
                const double from = std::atof(item.substr(atSign + 1, dash - atSign - 1).c_str());
                const double to = std::atof(item.substr(dash + 1).c_str());
                for (const Name &n : names)
                    if (name == n.name)
                        v.push_back({n.mask, from, to});
                struct StickName
                {
                    const char *name;
                    int byte;
                    uint8_t value;
                };
                static const StickName sticks[] = {{"lleft", 6, 0x00}, {"lright", 6, 0xFF}, {"lup", 7, 0x00}, {"ldown", 7, 0xFF},
                                                   {"rleft", 4, 0x00}, {"rright", 4, 0xFF}, {"rup", 5, 0x00}, {"rdown", 5, 0xFF}};
                for (const StickName &s : sticks)
                    if (name == s.name)
                        v.push_back({0u, from, to, s.byte, s.value});
            }
            return v;
        }
    }

    const std::vector<ScriptedPress> &scriptedPresses(int port)
    {
        static const std::vector<ScriptedPress> presses[2] = {parseScript(std::getenv(scriptEnvName(0))), parseScript(std::getenv(scriptEnvName(1)))};
        return presses[port == 1 ? 1 : 0];
    }
}

// Declared by the pad stubs (Kernel/Stubs/Pad.cpp) with `extern`: ps2_pad.h is included by the generated runner and must not change.
bool ps2x_padPortConnected(int port)
{
    if (port < 0 || port > 1)
        return false;
    if (!scriptedPresses(port).empty() || std::getenv(fileEnvName(port)) != nullptr)
        return true; // a scripted / file driven pad (diagnostics, replays) is a connected pad
    padinput::PadRouter &router = padinput::global();
    if (!router.updatedWithin(250))
        router.update();
    return router.connected(port);
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    // This port's own host input only (gamepad / keyboard of the player assigned to it): see pad_input.h
    uint16_t btns = 0xFFFFu;
    if (port == 0 || port == 1)
    {
        padinput::PadRouter &router = padinput::global();
        if (!router.updatedWithin(250))
            router.update();
        const padinput::PadSample sample = router.sample(port);
        btns = sample.buttons;
        data[4] = sample.rx;
        data[5] = sample.ry;
        data[6] = sample.lx;
        data[7] = sample.ly;
    }
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // PS2X_PAD_FILE / PS2X_PAD2_FILE=<path>: live remote control for diagnostics of that port. The file holds the buttons held right now, separated by
    // whitespace (same names as PS2X_PAD_SCRIPT: cross circle ... lleft lright lup ldown rleft ...). Re-read every ~30 ms.
    if (const char *padFile = (port == 0 || port == 1) ? std::getenv(fileEnvName(port)) : nullptr)
    {
        static std::string held[2];
        static auto lastRead = std::array<std::chrono::steady_clock::time_point, 2>{};
        const auto nowTp = std::chrono::steady_clock::now();
        if (nowTp - lastRead[port] > std::chrono::milliseconds(30))
        {
            lastRead[port] = nowTp;
            std::ifstream in(padFile);
            held[port].assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        struct Held
        {
            const char *name;
            uint16_t mask;
            int stickByte;
            uint8_t stickValue;
        };
        static const Held table[] = {{"left", PAD_LEFT, -1, 0}, {"down", PAD_DOWN, -1, 0}, {"right", PAD_RIGHT, -1, 0}, {"up", PAD_UP, -1, 0},
                                     {"start", PAD_START, -1, 0}, {"select", PAD_SELECT, -1, 0}, {"square", PAD_SQUARE, -1, 0}, {"cross", PAD_CROSS, -1, 0},
                                     {"circle", PAD_CIRCLE, -1, 0}, {"triangle", PAD_TRIANGLE, -1, 0}, {"r1", PAD_R1, -1, 0}, {"l1", PAD_L1, -1, 0},
                                     {"r2", PAD_R2, -1, 0}, {"l2", PAD_L2, -1, 0}, {"lleft", 0, 6, 0x00}, {"lright", 0, 6, 0xFF}, {"lup", 0, 7, 0x00},
                                     {"ldown", 0, 7, 0xFF}, {"rleft", 0, 4, 0x00}, {"rright", 0, 4, 0xFF}, {"rup", 0, 5, 0x00}, {"rdown", 0, 5, 0xFF}};
        std::istringstream tokens(held[port]);
        std::string word;
        while (tokens >> word)
            for (const Held &h : table)
                if (word == h.name)
                {
                    clearBit(h.mask);
                    if (h.stickByte >= 0)
                        data[h.stickByte] = h.stickValue;
                }
    }

    const std::vector<ScriptedPress> &script = scriptedPresses(port);
    if (!script.empty())
    {
        static const auto started = std::chrono::steady_clock::now();
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        for (const ScriptedPress &press : script)
            if (now >= press.from && now < press.to)
            {
                clearBit(press.mask);
                if (press.stickByte >= 0)
                    data[press.stickByte] = press.stickValue;
            }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
