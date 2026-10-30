#include "MiniTest.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "Stubs/Pad.h"
#include "runtime/pad_input.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    using namespace padinput;

    constexpr uint32_t kDataAddr = 0x1000;

    // Scriptable host: gamepads by host index, keyboard keys.
    class FakeBackend final : public InputBackend
    {
    public:
        struct Pad
        {
            bool present = false;
            std::string name;
            std::set<GpButton> down;
            float axis[4] = {0, 0, 0, 0};
        };
        Pad pads[4];
        std::set<int> keys;
        bool windowReady = true;

        void plug(int i, const std::string &name) { pads[i] = Pad{}; pads[i].present = true; pads[i].name = name; }
        void unplug(int i) { pads[i] = Pad{}; }
        void press(int i, GpButton b) { pads[i].down.insert(b); }
        void release(int i, GpButton b) { pads[i].down.erase(b); }

        bool ready() override { return windowReady; }
        bool gamepadPresent(int i) override { return i >= 0 && i < 4 && pads[i].present; }
        std::string gamepadName(int i) override { return pads[i].name; }
        bool gamepadButton(int i, GpButton b) override { return i >= 0 && i < 4 && pads[i].down.count(b) > 0; }
        float gamepadAxis(int i, GpAxis a) override { return pads[i].axis[static_cast<int>(a)]; }
        bool keyDown(int key) override { return keys.count(key) > 0; }
    };

    constexpr int kKeyX = 88, kKeyF4 = 293, kKeyEnter = 257, kKeyD = 68;

    bool down(const PadSample &s, uint16_t mask) { return (s.buttons & mask) == 0; }

    void setReg(R5900Context &ctx, int reg, uint32_t value) { ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value)); }

    void openPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port)
    {
        setReg(ctx, 4, port);
        setReg(ctx, 5, 0);
        setReg(ctx, 6, kDataAddr + 0x200u + port * 0x100u);
        ps2_stubs::scePadPortOpen(rdram.data(), &ctx, nullptr);
    }

    uint32_t readPort(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port, uint8_t *out)
    {
        setReg(ctx, 4, port);
        setReg(ctx, 5, 0);
        setReg(ctx, 6, kDataAddr + port * 0x40u);
        ps2_stubs::scePadRead(rdram.data(), &ctx, nullptr);
        std::memcpy(out, rdram.data() + kDataAddr + port * 0x40u, 32);
        return getRegU32(&ctx, 2);
    }

    uint32_t padState(R5900Context &ctx, std::vector<uint8_t> &rdram, uint32_t port)
    {
        setReg(ctx, 4, port);
        setReg(ctx, 5, 0);
        ps2_stubs::scePadGetState(rdram.data(), &ctx, nullptr);
        return getRegU32(&ctx, 2);
    }

    uint16_t buttonsOf(const uint8_t *d) { return static_cast<uint16_t>(d[2] | (d[3] << 8)); }

    // Fresh router state for the process-wide instance used by the pad stubs
    void useBackend(FakeBackend &fb, const Config &cfg)
    {
        setGlobalBackendForTests(&fb);
        global().setConfig(cfg);
    }
}

void register_pad_two_players_tests()
{
    MiniTest::Case("PadTwoPlayers", [](TestCase &tc)
                   {
        tc.Run("two gamepads reach only their own port", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            fb.plug(0, "Pad A");
            fb.plug(1, "Pad B");
            fb.press(0, GpButton::South);          // player 1: cross
            fb.press(1, GpButton::East);           // player 2: circle
            fb.pads[0].axis[0] = 1.0f;             // P1 left stick right
            fb.pads[1].axis[1] = -1.0f;            // P2 left stick up
            r.update();
            const PadSample p1 = r.sample(0), p2 = r.sample(1);
            t.IsTrue(p1.connected && p2.connected, "both ports connected");
            t.IsTrue(down(p1, kCross) && !down(p1, kCircle), "P1 has only cross");
            t.IsTrue(down(p2, kCircle) && !down(p2, kCross), "P2 has only circle");
            t.Equals(p1.lx, static_cast<uint8_t>(0xFF), "P1 lx");
            t.Equals(p1.ly, static_cast<uint8_t>(0x80), "P1 ly untouched by P2 stick");
            t.Equals(p2.ly, static_cast<uint8_t>(0x00), "P2 ly");
            t.Equals(p2.lx, static_cast<uint8_t>(0x80), "P2 lx untouched by P1 stick");
        });

        tc.Run("simultaneous presses of every button stay in their port", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            fb.plug(0, "Pad A");
            fb.plug(1, "Pad B");
            r.update();
            for (int bit = 0; bit < kButtonCount; ++bit)
            {
                const GpButton g = cfg.gamepadMap[bit];
                if (g == GpButton::R3)
                    continue; // R3 is the default Speed Up button
                const int other = (bit + 5) % kButtonCount;
                const GpButton go = cfg.gamepadMap[other];
                if (go == GpButton::R3 || other == bit)
                    continue;
                fb.press(0, g);
                fb.press(1, go);
                r.update();
                const PadSample p1 = r.sample(0), p2 = r.sample(1);
                t.Equals(static_cast<uint16_t>(~p1.buttons), static_cast<uint16_t>(1u << bit), std::string("P1 only ") + ps2ButtonName(bit));
                t.Equals(static_cast<uint16_t>(~p2.buttons), static_cast<uint16_t>(1u << other), std::string("P2 only ") + ps2ButtonName(other));
                fb.release(0, g);
                fb.release(1, go);
            }
        });

        tc.Run("keyboard on player 2 with one gamepad on player 1", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.player[1].kind = SourceKind::Keyboard;
            r.setConfig(cfg);
            fb.plug(0, "Pad A");
            fb.press(0, GpButton::North);
            fb.keys.insert(kKeyX); // keyboard cross
            r.update();
            t.IsTrue(down(r.sample(0), kTriangle) && !down(r.sample(0), kCross), "gamepad is player 1 only");
            t.IsTrue(down(r.sample(1), kCross) && !down(r.sample(1), kTriangle), "keyboard is player 2 only");
            fb.keys.erase(kKeyX);
            fb.keys.insert(kKeyD);
            r.update();
            t.Equals(r.sample(1).lx, static_cast<uint8_t>(0xFF), "keyboard D pushes player 2 left stick");
            t.Equals(r.sample(0).lx, static_cast<uint8_t>(0x80), "player 1 stick untouched");
        });

        tc.Run("single gamepad plays alone and port 2 stays empty", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.player[0].kind = SourceKind::Gamepad;
            cfg.player[1].kind = SourceKind::None;
            r.setConfig(cfg);
            fb.plug(0, "Solo");
            cfg.player[0].device = "Solo#0";
            r.setConfig(cfg);
            fb.press(0, GpButton::South);
            r.update();
            t.IsTrue(r.connected(0), "port 1 connected");
            t.IsTrue(!r.connected(1), "port 2 not connected");
            t.IsTrue(down(r.sample(0), kCross), "cross reaches player 1");
        });

        tc.Run("auto: gamepads follow connection order, keyboard goes with player 1", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            fb.plug(2, "Late");   // host index does not matter, connection order does
            r.update();
            fb.plug(0, "Early");
            r.update();
            t.IsTrue(r.player(0).label.find("Late") != std::string::npos, "first seen gamepad is player 1");
            t.IsTrue(r.player(1).label.find("Early") != std::string::npos, "second one is player 2");
        });

        tc.Run("hot plug: a lost gamepad leaves its player waiting, nobody is swapped", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.player[0].kind = SourceKind::Gamepad;
            cfg.player[1].kind = SourceKind::Gamepad;
            fb.plug(0, "Alpha");
            fb.plug(1, "Beta");
            cfg.player[0].device = "Alpha#0";
            cfg.player[1].device = "Beta#0";
            r.setConfig(cfg);
            r.update();
            t.IsTrue(r.connected(0) && r.connected(1), "both connected");
            fb.unplug(0);
            r.update();
            t.IsTrue(!r.connected(0), "player 1 lost its pad");
            t.IsTrue(r.connected(1), "player 2 unaffected");
            fb.press(1, GpButton::South);
            r.update();
            t.IsTrue(down(r.sample(1), kCross) && r.sample(0).buttons == 0xFFFF, "player 2 keeps playing, player 1 reads neutral");
            // different pad appears at the freed index: it must not become player 1
            fb.plug(0, "Gamma");
            r.update();
            t.IsTrue(!r.connected(0), "unknown gamepad is not silently given to player 1");
            // the original returns on another host index
            fb.unplug(0);
            fb.plug(3, "Alpha");
            r.update();
            t.IsTrue(r.connected(0), "Alpha is player 1 again");
            t.IsTrue(r.player(0).label.find("Alpha") != std::string::npos, "label shows Alpha");
        });

        tc.Run("hot plug in auto mode: unplugging player 1 does not promote player 2", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            fb.plug(0, "One");
            fb.plug(1, "Two");
            r.update();
            fb.unplug(0);
            r.update();
            t.IsTrue(r.player(0).waiting, "player 1 waits");
            t.IsTrue(r.player(1).label.find("Two") != std::string::npos, "Two stays player 2");
            fb.press(1, GpButton::South);
            r.update();
            t.IsTrue(!down(r.sample(0), kCross) && down(r.sample(1), kCross), "Two's presses stay on port 2");
            // a new pad claims the waiting player only by pressing a button
            fb.plug(0, "Three");
            r.update();
            t.IsTrue(r.player(0).label.find("Three") == std::string::npos, "not claimed without a press (keyboard still serves player 1)");
            fb.press(0, GpButton::Start);
            r.update();
            t.IsTrue(r.player(0).label.find("Three") != std::string::npos && !r.player(0).waiting, "claimed by pressing a button");
        });

        tc.Run("same gamepad model twice keeps identities distinct", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            fb.plug(0, "Twin");
            fb.plug(1, "Twin");
            r.update();
            const std::vector<DeviceStatus> devs = r.devices();
            t.Equals(static_cast<uint32_t>(devs.size()), 2u, "two devices");
            t.IsTrue(devs[0].identity == "Twin#0" && devs[1].identity == "Twin#1", "identities are numbered");
            r.assign(0, SourceKind::Gamepad, "Twin#1");
            r.assign(1, SourceKind::Gamepad, "Twin#0");
            fb.press(1, GpButton::South);
            r.update();
            t.IsTrue(down(r.sample(0), kCross) && !down(r.sample(1), kCross), "swap by assignment works");
            t.IsTrue(r.config().player[0].device == "Twin#1", "config records the choice");
        });

        tc.Run("assigning a gamepad or the keyboard to a player removes it from the other", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            fb.plug(0, "Solo");
            r.assign(0, SourceKind::Keyboard);
            r.assign(1, SourceKind::Keyboard);
            t.IsTrue(r.config().player[0].kind == SourceKind::Auto && r.config().player[1].kind == SourceKind::Keyboard, "keyboard moved to player 2");
            r.assign(0, SourceKind::Gamepad, "Solo#0");
            r.assign(1, SourceKind::Gamepad, "Solo#0");
            t.IsTrue(r.config().player[0].kind == SourceKind::Auto && r.config().player[1].device == "Solo#0", "gamepad moved to player 2");
        });

        tc.Run("speed up is global: not forwarded, combo hidden from the game", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.speedUpPad = {GpButton::L3, GpButton::R3};
            cfg.speedUpKey = kKeyF4;
            r.setConfig(cfg);
            fb.plug(0, "A");
            fb.plug(1, "B");
            r.update();
            fb.press(1, GpButton::R3);              // only one of the two: no speed up, and R3 is a normal button
            r.update();
            t.IsTrue(!r.speedUpActive(), "single button is not the combination");
            t.IsTrue(down(r.sample(1), kR3), "R3 reaches player 2 when the combo is not complete");
            fb.press(1, GpButton::L3);              // player 2 completes the combination
            fb.press(0, GpButton::South);
            r.update();
            t.IsTrue(r.speedUpActive(), "combination on pad 2 enables speed up");
            t.IsTrue(!down(r.sample(1), kL3) && !down(r.sample(1), kR3), "combo buttons are hidden from player 2");
            t.IsTrue(down(r.sample(0), kCross) && r.sample(0).buttons == static_cast<uint16_t>(~kCross), "player 1 is not affected");
            fb.release(1, GpButton::L3);
            fb.release(1, GpButton::R3);
            r.update();
            t.IsTrue(!r.speedUpActive(), "released");
            fb.keys.insert(kKeyF4);
            r.update();
            t.IsTrue(r.speedUpActive(), "keyboard key works too");
            t.IsTrue(r.sample(0).buttons == static_cast<uint16_t>(~kCross) || r.sample(0).buttons == 0xFFFF, "F4 is not a game button");
        });

        tc.Run("speed up toggle mode", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.speedUpToggle = true;
            r.setConfig(cfg);
            fb.plug(0, "A");
            fb.press(0, GpButton::R3);
            r.update();
            t.IsTrue(r.speedUpActive(), "pressed: on");
            r.update();
            t.IsTrue(r.speedUpActive(), "still held: stays on");
            fb.release(0, GpButton::R3);
            r.update();
            t.IsTrue(r.speedUpActive(), "released: stays on");
            fb.press(0, GpButton::R3);
            r.update();
            t.IsTrue(!r.speedUpActive(), "second press: off");
        });

        tc.Run("configuration round trip and tolerant parsing", [](TestCase &t)
               {
            Config c;
            c.player[0].kind = SourceKind::Gamepad;
            c.player[0].device = "Wireless Controller#1";
            c.player[1].kind = SourceKind::Keyboard;
            c.gamepadMap[14] = GpButton::West;
            c.keyboardMap[3] = {kKeyEnter, 32};
            c.speedUpPad = {GpButton::L1, GpButton::R1};
            c.speedUpToggle = true;
            c.deadzone = 0.25f;
            const Config back = Config::parse(c.serialize());
            t.IsTrue(back.player[0].kind == SourceKind::Gamepad && back.player[0].device == "Wireless Controller#1", "player 1 gamepad");
            t.IsTrue(back.player[1].kind == SourceKind::Keyboard, "player 2 keyboard");
            t.IsTrue(back.gamepadMap[14] == GpButton::West, "gamepad remap");
            t.IsTrue(back.keyboardMap[3][0] == kKeyEnter && back.keyboardMap[3][1] == 32, "keyboard remap");
            t.IsTrue(back.speedUpPad.size() == 2 && back.speedUpPad[1] == GpButton::R1 && back.speedUpToggle, "speed up");
            t.IsTrue(back.deadzone > 0.24f && back.deadzone < 0.26f, "deadzone");
            std::vector<std::string> warnings;
            const Config bad = Config::parse("player1=keyboard\nplayer2=keyboard\nbogus=1\npad.cross=nonsense\n", &warnings);
            t.IsTrue(bad.player[0].kind == SourceKind::Keyboard && bad.player[1].kind != SourceKind::Keyboard, "keyboard cannot serve both players");
            t.IsTrue(!warnings.empty(), "problems reported");
        });

        tc.Run("remapped gamepad button reaches only its player", [](TestCase &t)
               {
            FakeBackend fb;
            PadRouter r(&fb);
            Config cfg;
            cfg.gamepadMap[14] = GpButton::West; // cross on the host's West button
            cfg.gamepadMap[15] = GpButton::South;
            r.setConfig(cfg);
            fb.plug(0, "A");
            fb.plug(1, "B");
            fb.press(0, GpButton::West);
            r.update();
            t.IsTrue(down(r.sample(0), kCross) && !down(r.sample(1), kCross), "remap applies, other port quiet");
        });

        tc.Run("virtual PS2 ports: scePadRead delivers each player's own state", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            FakeBackend fb;
            Config cfg;
            cfg.speedUpKey = 0;
            useBackend(fb, cfg);
            fb.plug(0, "Pad A");
            fb.plug(1, "Pad B");
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPort(ctx, rdram, 0);
            openPort(ctx, rdram, 1);
            fb.press(0, GpButton::South);
            fb.press(1, GpButton::North);
            fb.pads[0].axis[2] = 1.0f; // P1 right stick X
            fb.pads[1].axis[3] = 1.0f; // P2 right stick Y
            global().update();
            uint8_t d0[32], d1[32];
            t.Equals(readPort(ctx, rdram, 0, d0), 1u, "port 0 read ok");
            t.Equals(readPort(ctx, rdram, 1, d1), 1u, "port 1 read ok");
            t.Equals(static_cast<uint16_t>(~buttonsOf(d0)), static_cast<uint16_t>(kCross), "port 0 gets only cross");
            t.Equals(static_cast<uint16_t>(~buttonsOf(d1)), static_cast<uint16_t>(kTriangle), "port 1 gets only triangle");
            t.Equals(d0[4], static_cast<uint8_t>(0xFF), "port 0 rx");
            t.Equals(d0[5], static_cast<uint8_t>(0x80), "port 0 ry untouched");
            t.Equals(d1[4], static_cast<uint8_t>(0x80), "port 1 rx untouched");
            t.Equals(d1[5], static_cast<uint8_t>(0xFF), "port 1 ry");
            ps2_stubs::scePadPortClose(rdram.data(), &ctx, nullptr);
            setGlobalBackendForTests(nullptr);
        });

        tc.Run("virtual PS2 ports: empty port 2 reports disconnected, hot plug renegotiates", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            FakeBackend fb;
            Config cfg;
            cfg.player[0].kind = SourceKind::Gamepad;
            cfg.player[1].kind = SourceKind::Gamepad;
            useBackend(fb, cfg);
            fb.plug(0, "Pad A");
            cfg.player[0].device = "Pad A#0";
            cfg.player[1].device = "Pad B#0";
            global().setConfig(cfg);
            global().update();
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPort(ctx, rdram, 0);
            openPort(ctx, rdram, 1);
            t.Equals(padState(ctx, rdram, 0), 6u, "port 0 STABLE");
            t.Equals(padState(ctx, rdram, 1), 0u, "port 1 DISCONNECTED while nobody is plugged");
            uint8_t d[32];
            t.Equals(readPort(ctx, rdram, 1, d), 0u, "scePadRead on the empty port fails");
            fb.plug(1, "Pad B");
            fb.press(1, GpButton::South);
            global().update();
            const uint32_t st = padState(ctx, rdram, 1);
            t.IsTrue(st == 5u || st == 6u, "port 1 comes back (EXECCMD or STABLE)");
            for (int i = 0; i < 4; ++i)
                padState(ctx, rdram, 1);
            t.Equals(readPort(ctx, rdram, 1, d), 1u, "read works after the pad appears");
            t.IsTrue((buttonsOf(d) & kCross) == 0, "its own press is there");
            fb.unplug(1);
            global().update();
            t.Equals(padState(ctx, rdram, 1), 0u, "unplugged again");
            t.Equals(padState(ctx, rdram, 0), 6u, "player 1 unaffected");
            setGlobalBackendForTests(nullptr);
        });

        tc.Run("per-port overrides drive one port only", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0);
            R5900Context ctx;
            ps2_stubs::scePadInit(rdram.data(), &ctx, nullptr);
            openPort(ctx, rdram, 0);
            openPort(ctx, rdram, 1);
            ps2_stubs::setPadOverrideStateForPort(1, static_cast<uint16_t>(0xFFFFu & ~kCircle), 0x80, 0x80, 0x80, 0x80);
            uint8_t d0[32], d1[32];
            readPort(ctx, rdram, 0, d0);
            t.Equals(readPort(ctx, rdram, 1, d1), 1u, "port 1 override read");
            t.IsTrue((buttonsOf(d1) & kCircle) == 0, "port 1 sees its override");
            t.IsTrue((buttonsOf(d0) & kCircle) != 0, "port 0 does not");
            ps2_stubs::clearPadOverrideStateForPort(1);
        });
    });
}
