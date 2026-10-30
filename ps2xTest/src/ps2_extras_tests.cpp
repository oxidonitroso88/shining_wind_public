// PC Extras infrastructure: settings/hotkey parsing, the input gate (pad block), the audio mute flag and the pause flag.
#include "MiniTest.h"
#include "extras/extras_gate.h"
#include "extras/extras_host.h"
#include "extras/features/soultree_process.h"
#include "extras/session_config.h"
#include "ps2x/iop/audio_output.h"
#include "runtime/ee_scheduler.h"
#include "runtime/pad_input.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <set>
#include <string>

namespace
{
    using namespace padinput;

    class FakeBackend final : public InputBackend
    {
    public:
        std::set<GpButton> down;
        bool ready() override { return true; }
        bool gamepadPresent(int i) override { return i == 0; }
        std::string gamepadName(int) override { return "Test pad"; }
        bool gamepadButton(int i, GpButton b) override { return i == 0 && down.count(b) > 0; }
        float gamepadAxis(int, GpAxis) override { return 0.0f; }
        bool keyDown(int) override { return false; }
    };
}

void register_ps2_extras_tests()
{
    using pc_extras::host::Settings;
    MiniTest::Case("PcExtras", [](TestCase &tc)
    {
        tc.Run("settings: defaults, hotkey and pad combination from extras.cfg, reserved keys and weak combinations are refused", [](TestCase &t)
        {
            const Settings d = Settings::defaults();
            t.IsTrue(d.enabled, "enabled by default");
            t.Equals(d.toggleKey, pc_extras::host::keyFromName("F7"), "default hotkey F7");
            t.Equals(d.toggleCombo, static_cast<unsigned>(kL3 | kR3), "default pad combination L3+R3");
            std::string w;
            Settings s = Settings::parse("toggle_key=F8\ntoggle_pad=l1+r1+select\nenabled=0\n", &w);
            t.Equals(s.toggleKey, pc_extras::host::keyFromName("F8"), "F8");
            t.Equals(s.toggleCombo, static_cast<unsigned>(kL1 | kR1 | kSelect), "l1+r1+select");
            t.IsFalse(s.enabled, "enabled=0");
            t.IsTrue(w.empty(), "no warnings for valid input");
            w.clear();
            s = Settings::parse("toggle_key=F9\n", &w);
            t.Equals(s.toggleKey, d.toggleKey, "F9 (memory dump) is reserved, the default is kept");
            t.IsFalse(w.empty(), "a warning is reported");
            for (const char *bad : {"toggle_key=F10", "toggle_key=F1", "toggle_key=F2", "toggle_key=ESCAPE", "toggle_key=nonsense"})
                t.Equals(Settings::parse(bad).toggleKey, d.toggleKey, bad);
            w.clear();
            s = Settings::parse("toggle_pad=start\n", &w);
            t.Equals(s.toggleCombo, d.toggleCombo, "a single button would collide with the game: refused");
            s = Settings::parse("toggle_pad=cross+nothing\n", &w);
            t.Equals(s.toggleCombo, d.toggleCombo, "unknown button name: refused");
            t.IsTrue(pc_extras::host::keyFromName("tab") != 0 && pc_extras::host::keyFromName("p") != 0, "names are case-insensitive");
            t.Equals(pc_extras::host::keyFromName("F13"), 0, "unknown key");
        });

        tc.Run("input gate: the game reads a neutral, still connected pad while blocked; the menu still sees the pressed buttons", [](TestCase &t)
        {
            FakeBackend fb;
            PadRouter router(&fb);
            Config cfg;
            router.setConfig(cfg);
            fb.down.insert(GpButton::South);
            router.update();
            PadSample live = router.sample(0);
            t.IsTrue(live.connected, "connected");
            t.IsTrue((live.buttons & kCross) == 0, "cross is down for the game");
            router.setGameInputBlocked(true);
            PadSample blocked = router.sample(0);
            t.IsTrue(blocked.connected, "still connected: the game does not see a disconnection");
            t.Equals(static_cast<unsigned>(blocked.buttons), 0xFFFFu, "no button reaches the game");
            t.Equals(static_cast<unsigned>(blocked.lx), 0x80u, "stick centred");
            bool seen = false;
            for (const auto &d : router.devices())
                seen = seen || (d.pressed & kCross) != 0;
            t.IsTrue(seen, "the device report (what the menu navigates with) still shows cross");
            router.setGameInputBlocked(false);
            t.IsTrue((router.sample(0).buttons & kCross) == 0, "unblocked again");
        });

        tc.Run("gate reasons are independent: closing the menu does not lift an event session's block", [](TestCase &t)
        {
            namespace gate = pc_extras::gate;
            gate::bind(nullptr);
            t.IsFalse(gate::active(), "idle");
            gate::set(gate::Session, true);
            gate::set(gate::Menu, true);
            t.IsTrue(ps2x::iop::audioMuted(), "audio muted");
            t.IsTrue(padinput::global().gameInputBlocked(), "pad blocked");
            gate::set(gate::Menu, false);
            t.IsTrue(gate::active() && ps2x::iop::audioMuted() && padinput::global().gameInputBlocked(), "the session still holds the gate");
            gate::set(gate::Session, false);
            t.IsFalse(gate::active(), "released");
            t.IsFalse(ps2x::iop::audioMuted(), "audio back");
            t.IsFalse(padinput::global().gameInputBlocked(), "pad back");
        });

        tc.Run("event sessions: only presentation settings reach the child; stale session folders are removed, nothing else", [](TestCase &t)
        {
            namespace sp = pc_extras::soultree_process;
            for (const char *no : {"PS2X_AUDIO", "PS2X_GS_SCALE", "PS2X_MC_ROOT", "PS2X_INPUT_CONFIG", "PS2X_TEXMAP", "PS2X_SCRIPT_TRACE", "PS2X_PAD_REPLAY", "PS2X_WINDOW_SHOT",
                                   "PS2X_FAST_FORWARD", "PS2X_SPU2_MIX_WAV"})
                t.IsFalse(sp::environmentPassThrough(no), no); // nothing travels by itself: the session's real settings are built by session_config::forChild()
            t.IsTrue(sp::environmentPassThrough("PS2X_PAD_REPLAY", "PS2X_A,PS2X_PAD_REPLAY"), "explicit test-only list");
            t.IsFalse(sp::environmentPassThrough("PS2X_PAD", "PS2X_A,PS2X_PAD_REPLAY"), "whole names only");

            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path temp = fs::temp_directory_path(ec);
            const fs::path dead = temp / "ps2x-soul-2147483000-1", other = temp / "ps2x-soul-notes", unrelated = temp / "ps2x_extras_test_unrelated";
            fs::create_directories(dead / "card", ec);
            fs::create_directories(other, ec);
            fs::create_directories(unrelated, ec);
            { std::ofstream f(dead / "child.log"); f << "x"; }
            sp::removeStaleFolders();
            t.IsFalse(fs::exists(dead, ec), "folder of a process that does not exist is removed");
            t.IsTrue(fs::exists(other, ec), "a folder without a process number is left alone");
            t.IsTrue(fs::exists(unrelated, ec), "folders with other names are never touched");
            fs::remove_all(other, ec);
            fs::remove_all(unrelated, ec);
        });

        tc.Run("session configuration: absolute resource paths and the presentation switches of the parent reach the child, nothing else", [](TestCase &t)
        {
            namespace sc = pc_extras::session_config;
            namespace fs = std::filesystem;
            std::error_code ec;
            const fs::path mods = fs::temp_directory_path(ec) / "ps2x_extras_cfg_mods";
            fs::create_directories(mods, ec);
            _putenv_s("PS2X_MODS", mods.string().c_str());
            _putenv_s("PS2X_STORY_EN", "off");
            _putenv_s("PS2X_HD_PACK", "off");
            _putenv_s("PS2X_GS_SCALE", "4");
            _putenv_s("PS2X_WINDOW_SCALE", "2");
            _putenv_s("PS2X_MC_ROOT", "C:/must/not/travel");
            _putenv_s("PS2X_TEXMAP", "C:/must/not/travel");
            const auto settings = sc::forChild();
            auto value = [&](const std::string &name) -> std::string {
                for (const auto &s : settings)
                    if (s.first == name)
                        return s.second;
                return "<absent>";
            };
            t.Equals(fs::path(value("PS2X_MODS")).lexically_normal().string(), mods.lexically_normal().string(), "mods folder, absolute");
            t.Equals(value("PS2X_STORY_EN"), std::string("off"), "no story table: explicit off (the child must not search its own temp folder)");
            t.Equals(value("PS2X_HD_PACK"), std::string("off"), "no HD pack: explicit off");
            t.Equals(value("PS2X_HD_PACK_STATE"), std::string("on"), "replacement state of the parent (on when nothing is loaded)");
            t.Equals(value("PS2X_GS_SCALE"), std::string("4"), "internal resolution of the parent");
            t.Equals(value("PS2X_WINDOW_SCALE"), std::string("2"), "window scale of the parent");
            t.Equals(value("PS2X_MC_ROOT"), std::string("<absent>"), "memory card path never travels");
            t.Equals(value("PS2X_TEXMAP"), std::string("<absent>"), "diagnostic outputs never travel");
            t.IsTrue(sc::describe("parent").find("PS2X_GS_SCALE=4") != std::string::npos, "the log line shows the scale");
            for (const char *n : {"PS2X_MODS", "PS2X_STORY_EN", "PS2X_HD_PACK", "PS2X_GS_SCALE", "PS2X_WINDOW_SCALE", "PS2X_MC_ROOT", "PS2X_TEXMAP"})
                _putenv_s(n, "");
            fs::remove_all(mods, ec);
        });
    });
}
