// See include/extras/extras_host.h.
#include "extras/extras_host.h"

#include "extras/extras_gate.h"
#include "extras/features/soultree_feature.h"
#include "ps2_runtime.h"
#include "runtime/pad_input.h"

#include <raylib.h>
#include <rlgl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace pc_extras::host
{
    namespace
    {
        // Keys the port or raylib already use: never accepted as the Extras hotkey.
        bool reservedKey(int key)
        {
            return key == KEY_F1 || key == KEY_F2 || key == KEY_F9 || key == KEY_F10 || key == KEY_ESCAPE || key == KEY_ENTER || key == KEY_SPACE || key == KEY_UP ||
                   key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT || key == KEY_BACKSPACE || key == 0;
        }

        int bitCount(unsigned v)
        {
            int n = 0;
            for (; v != 0u; v &= v - 1u)
                ++n;
            return n;
        }

        struct Pending
        {
            long frame;
            std::string action;
        };

        class HostImpl final : public Services
        {
        public:
            enum class Screen { Closed, Menu, Feature, SessionConfirm };

            std::vector<std::unique_ptr<Feature>> features;
            std::vector<Feature *> offered; // available features, in registration order, rebuilt every time the menu opens
            Screen screen = Screen::Closed;
            Feature *active = nullptr;
            int cursor = 0;
            Settings settings = Settings::defaults();
            bool initialized = false, standalone = false, wantHide = false, hidden = false, quit = false;
            std::string exePathStr, bootPathStr, settingsPathStr;
            PS2Runtime *runtime = nullptr;
            long frameNo = 0;
            std::vector<Pending> script;
            bool prevCombo = false;
            unsigned prevPad = 0;
            double nextRepeat = 0.0;
            int heldDir = 0;
            // event session child
            bool sessionMode = false;
            std::string sessionTitle;
            bool (*sessionReady)() = nullptr;

            void hideMainWindow(bool h) override { wantHide = h; }
            const std::string &executablePath() const override { return exePathStr; }
            const std::string &bootPath() const override { return bootPathStr; }
            const std::string &settingsPath() const override { return settingsPathStr; }
            void setSessionBlock(bool on) override { gate::set(gate::Session, on); }

            void init();
            void rebuildOffered();
            void open();
            void close();
            Input poll(unsigned *padNow);
            void frame();
            void shot();
            void drawMenu(Style &style);
            void drawSession(Style &style);
        };

        HostImpl &impl()
        {
            static HostImpl h;
            return h;
        }

        std::string trim(std::string s)
        {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
            size_t i = 0;
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            return s.substr(i);
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }
    }

    int keyFromName(const std::string &name)
    {
        const std::string n = upper(trim(name));
        if (n.size() >= 2 && n[0] == 'F' && std::isdigit(static_cast<unsigned char>(n[1])))
        {
            const int f = std::atoi(n.c_str() + 1);
            if (f >= 1 && f <= 12)
                return KEY_F1 + (f - 1);
            return 0;
        }
        if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z')
            return KEY_A + (n[0] - 'A');
        if (n.size() == 1 && n[0] >= '0' && n[0] <= '9')
            return KEY_ZERO + (n[0] - '0');
        static const struct { const char *name; int key; } named[] = {
            {"TAB", KEY_TAB}, {"HOME", KEY_HOME}, {"END", KEY_END}, {"INSERT", KEY_INSERT}, {"DELETE", KEY_DELETE},
            {"PAGEUP", KEY_PAGE_UP}, {"PAGEDOWN", KEY_PAGE_DOWN}, {"PAUSE", KEY_PAUSE}, {"SCROLLLOCK", KEY_SCROLL_LOCK}, {"GRAVE", KEY_GRAVE}};
        for (const auto &e : named)
            if (n == e.name)
                return e.key;
        return 0;
    }

    Settings Settings::defaults()
    {
        Settings s;
        s.enabled = true;
        s.toggleKey = KEY_F7;
        s.toggleCombo = padinput::kL3 | padinput::kR3;
        return s;
    }

    Settings Settings::parse(const std::string &text, std::string *warnings)
    {
        Settings s = defaults();
        std::istringstream in(text);
        std::string line;
        auto warn = [&](const std::string &w) { if (warnings != nullptr) *warnings += w + "\n"; };
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                warn("ignored: " + line);
                continue;
            }
            const std::string key = upper(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
            if (key == "ENABLED")
                s.enabled = !(value == "0" || upper(value) == "FALSE" || upper(value) == "OFF");
            else if (key == "TOGGLE_KEY")
            {
                const int k = keyFromName(value);
                if (k == 0 || reservedKey(k))
                    warn("toggle_key '" + value + "' is unknown or reserved: keeping F7");
                else
                    s.toggleKey = k;
            }
            else if (key == "TOGGLE_PAD")
            {
                unsigned bits = 0u;
                bool ok = !value.empty();
                std::istringstream parts(value);
                std::string part;
                while (std::getline(parts, part, '+'))
                {
                    std::string lower = trim(part);
                    for (char &c : lower)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    const int bit = padinput::ps2ButtonFromName(lower);
                    if (bit < 0)
                        ok = false;
                    else
                        bits |= 1u << bit;
                }
                // a single face button or Start would collide with the game: at least two buttons are required
                if (!ok || bitCount(bits) < 2)
                    warn("toggle_pad '" + value + "' needs two or more valid buttons: keeping L3+R3");
                else
                    s.toggleCombo = bits;
            }
            else
                warn("unknown setting: " + key);
        }
        return s;
    }

    void HostImpl::init()
    {
        if (initialized)
            return;
        initialized = true;
        registerBuiltInFeatures();
        std::string path;
        if (const char *env = std::getenv("PS2X_EXTRAS_CONFIG"); env != nullptr && *env != '\0')
            path = env;
        else
        {
            std::error_code ec;
            const std::filesystem::path input = std::filesystem::path(padinput::configPath()).parent_path();
            path = (input.empty() ? std::filesystem::path("extras.cfg") : input / "extras.cfg").string();
        }
        settingsPathStr = path;
        std::ifstream f(path, std::ios::binary);
        if (f)
        {
            std::stringstream ss;
            ss << f.rdbuf();
            std::string warnings;
            settings = Settings::parse(ss.str(), &warnings);
            if (!warnings.empty())
                std::fprintf(stderr, "[extras] %s:\n%s", path.c_str(), warnings.c_str()); std::fflush(stderr);
        }
        if (const char *e = std::getenv("PS2X_EXTRAS"); e != nullptr && std::string(e) == "0")
            settings.enabled = false;
        // test-only scripted input: PS2X_EXTRAS_TEST_INPUT="60:toggle;70:down;80:accept" (host frame number : action)
        if (const char *t = std::getenv("PS2X_EXTRAS_TEST_INPUT"))
        {
            std::istringstream in(t);
            std::string item;
            while (std::getline(in, item, ';'))
            {
                const size_t c = item.find(':');
                if (c != std::string::npos)
                    script.push_back({std::atol(item.substr(0, c).c_str()), trim(item.substr(c + 1))});
            }
        }
    }

    void HostImpl::rebuildOffered()
    {
        offered.clear();
        for (auto &f : features)
        {
            std::string why;
            if (f->available(*this, why))
                offered.push_back(f.get());
            else
                std::fprintf(stderr, "[extras] feature '%s' not offered: %s\n", f->id(), why.c_str()); std::fflush(stderr);
        }
    }

    void HostImpl::open()
    {
        rebuildOffered();
        screen = Screen::Menu;
        cursor = 0;
        active = nullptr;
        SetExitKey(KEY_NULL); // Esc is "back" here, it must not close the window
        gate::set(gate::Menu, true);
    }

    void HostImpl::close()
    {
        screen = Screen::Closed;
        active = nullptr;
        gate::set(gate::Menu, false);
        SetExitKey(KEY_ESCAPE);
    }

    Input HostImpl::poll(unsigned *padNow)
    {
        Input in;
        unsigned pad = 0u;
        for (const auto &d : padinput::global().devices())
            pad |= d.pressed;
        *padNow = pad;
        const unsigned edge = pad & ~prevPad;
        float stickX = 0.0f, stickY = 0.0f;
        for (int g = 0; g < 4; ++g)
            if (IsGamepadAvailable(g))
            {
                stickX += GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_X);
                stickY += GetGamepadAxisMovement(g, GAMEPAD_AXIS_LEFT_Y);
            }
        // directions: held state with key-repeat
        const bool up = (pad & padinput::kUp) || IsKeyDown(KEY_UP) || stickY < -0.6f;
        const bool down = (pad & padinput::kDown) || IsKeyDown(KEY_DOWN) || stickY > 0.6f;
        const bool left = (pad & padinput::kLeft) || IsKeyDown(KEY_LEFT) || stickX < -0.6f;
        const bool right = (pad & padinput::kRight) || IsKeyDown(KEY_RIGHT) || stickX > 0.6f;
        const int dir = up ? 1 : down ? 2 : left ? 3 : right ? 4 : 0;
        const double now = GetTime();
        if (dir != 0 && (dir != heldDir || now >= nextRepeat))
        {
            const bool first = dir != heldDir;
            nextRepeat = now + (first ? 0.38 : 0.11);
            in.up = dir == 1;
            in.down = dir == 2;
            in.left = dir == 3;
            in.right = dir == 4;
        }
        heldDir = dir;
        in.accept = (edge & (padinput::kCircle | padinput::kStart)) != 0u || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);
        in.back = (edge & (padinput::kCross | padinput::kTriangle)) != 0u || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_BACKSPACE);
        return in;
    }

    void HostImpl::drawMenu(Style &style)
    {
        style.begin();
        style.dimBackground();
        style.header("Extras", "Features added by the Windows port");
        const int count = static_cast<int>(offered.size());
        style.panel(Rect{64.0f, 176.0f, 640.0f, 440.0f}, false);
        if (count == 0)
            style.text("No extra features are available here.", 96.0f, 420.0f, 24.0f, Tone::Dim);
        for (int i = 0; i < count; ++i)
            style.row(offered[i]->title(), Rect{80.0f, 192.0f + 64.0f * i, 608.0f, 56.0f}, i == cursor);
        style.row(standalone ? "Quit" : "Close", Rect{80.0f, 192.0f + 64.0f * count, 608.0f, 56.0f}, cursor == count);
        style.panel(Rect{728.0f, 176.0f, 488.0f, 440.0f}, false);
        if (cursor < count)
        {
            style.text(offered[cursor]->title(), 752.0f, 200.0f, 30.0f, Tone::Accent);
            style.paragraph(offered[cursor]->summary(), 752.0f, 248.0f, 440.0f, 22.0f, Tone::Dim);
        }
        else
            style.text(standalone ? "Quit PC Extras" : "Return to the game", 752.0f, 200.0f, 30.0f, Tone::Accent);
        style.footer(standalone ? "Up/Down  Select      Enter / (O)  Open      Esc / (X)  Quit" : "Up/Down  Select      Enter / (O)  Open      Esc / (X)  Close");
    }

    void HostImpl::drawSession(Style &style)
    {
        style.begin();
        if (screen == Screen::SessionConfirm)
        {
            style.dimBackground(0.86f);
            style.header("Soul Tree Events", sessionTitle.c_str());
            style.message("Stop this event and return to the selector?", Tone::Warn, 320.0f);
            style.message("Enter / (O)  Stop      Esc / (X)  Keep watching", Tone::Dim, 380.0f);
            return;
        }
        if (sessionReady != nullptr && !sessionReady())
        {
            style.dimBackground(1.0f); // opaque: the game's own boot pictures are not part of the loading screen
            style.header("Soul Tree Events", sessionTitle.c_str());
            style.spinner(Style::kWidth * 0.5f, 330.0f, 36.0f);
            style.message("Loading the event...", Tone::Normal, 400.0f);
            style.footer("Extras hotkey (default F7, or L3+R3)  Stop the event");
        }
    }

    void HostImpl::frame()
    {
        init();
        ++frameNo;
        if (!settings.enabled && !sessionMode)
            return;
        for (auto &f : features)
            f->tick(*this);

        // scripted input (tests) and the real devices
        bool scriptedToggle = false, scriptedShot = false;
        Input in;
        unsigned pad = 0u;
        in = poll(&pad);
        for (const Pending &p : script)
            if (p.frame == frameNo)
            {
                if (p.action == "toggle") scriptedToggle = true;
                else if (p.action == "up") in.up = true;
                else if (p.action == "down") in.down = true;
                else if (p.action == "left") in.left = true;
                else if (p.action == "right") in.right = true;
                else if (p.action == "accept") in.accept = true;
                else if (p.action == "back") in.back = true;
                else if (p.action == "shot") scriptedShot = true;
                else if (p.action == "vb") { std::fprintf(stderr, "[extras] vb=%llu frame=%ld screen=%d\n", g_ps2xGsVblank.load(std::memory_order_relaxed), frameNo, static_cast<int>(screen)); std::fflush(stderr); }
                else if (p.action == "quit") std::_Exit(0);
            }
        const bool comboNow = settings.toggleCombo != 0u && (pad & settings.toggleCombo) == settings.toggleCombo;
        const bool toggle = scriptedToggle || IsKeyPressed(settings.toggleKey) || (comboNow && !prevCombo);
        prevCombo = comboNow;
        prevPad = pad;

        if (sessionMode)
        {
            if (screen == Screen::Closed && toggle)
            {
                screen = Screen::SessionConfirm;
                SetExitKey(KEY_NULL);
            }
            else if (screen == Screen::SessionConfirm)
            {
                if (in.accept)
                {
                    std::fprintf(stderr, "[soul] event stopped by the user\n");
                    std::fflush(nullptr);
                    std::_Exit(43);
                }
                if (in.back || toggle)
                {
                    screen = Screen::Closed;
                    SetExitKey(KEY_ESCAPE);
                }
            }
            Style style;
            drawSession(style);
            if (scriptedShot)
                shot();
            return;
        }

        bool anyBusy = false;
        for (auto &f : features)
            anyBusy = anyBusy || f->busy();

        if (screen == Screen::Closed)
        {
            if (toggle && !anyBusy)
                open();
        }
        else if (toggle && !anyBusy && screen == Screen::Menu)
            close();

        if (screen == Screen::Menu)
        {
            const int count = static_cast<int>(offered.size());
            if (in.up) cursor = (cursor + count) % (count + 1);
            if (in.down) cursor = (cursor + 1) % (count + 1);
            if (in.back)
            {
                if (standalone) quit = true; else close();
            }
            else if (in.accept)
            {
                if (cursor >= count)
                {
                    if (standalone) quit = true; else close();
                }
                else
                {
                    active = offered[cursor];
                    screen = Screen::Feature;
                    active->open(*this);
                }
            }
        }
        else if (screen == Screen::Feature && active != nullptr)
        {
            if (!active->update(*this, in))
            {
                active = nullptr;
                screen = Screen::Menu;
            }
        }

        if (screen != Screen::Closed)
        {
            Style style;
            if (screen == Screen::Menu)
                drawMenu(style);
            else if (active != nullptr)
            {
                style.begin();
                style.dimBackground();
                active->draw(style);
            }
        }
        else
        {
            // closed: features that are running (an event session) may still need to show their state
            for (auto &f : features)
                if (f->busy())
                {
                    Style style;
                    style.begin();
                    style.dimBackground(0.9f);
                    f->draw(style);
                }
        }
        if (scriptedShot)
            shot();
    }

    void HostImpl::shot()
    {
        // test only: what this window shows (its own back buffer, never the desktop) -> extras_shot_<frame>.png in the working directory
        rlDrawRenderBatchActive(); // what was drawn this frame must reach the back buffer before it is read
        Image img = LoadImageFromScreen();
        ExportImage(img, ("extras_shot_" + std::to_string(frameNo) + ".png").c_str());
        UnloadImage(img);
    }

    void registerFeature(std::unique_ptr<Feature> feature) { impl().features.push_back(std::move(feature)); }

    void registerBuiltInFeatures()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        registerFeature(makeSoulTreeFeature()); // listed only when it can really run (see Feature::available)
    }

    void setLaunchContext(const std::string &executablePath, const std::string &bootPath)
    {
        impl().exePathStr = executablePath;
        impl().bootPathStr = bootPath;
    }

    void enterSessionMode(const std::string &title, bool (*ready)())
    {
        HostImpl &h = impl();
        h.sessionMode = true;
        h.sessionTitle = title;
        h.sessionReady = ready;
    }

    void frame(PS2Runtime *runtime)
    {
        HostImpl &h = impl();
        h.runtime = runtime;
        gate::bind(runtime);
        h.frame();
        if (h.hidden)
            WaitTime(0.016); // nothing is visible: do not spin the render loop while an event runs in its own window
        // The window is shown / hidden here, on the render thread.
        if (h.wantHide != h.hidden)
        {
            if (h.wantHide)
                SetWindowState(FLAG_WINDOW_HIDDEN);
            else
            {
                ClearWindowState(FLAG_WINDOW_HIDDEN);
                SetWindowFocused();
            }
            h.hidden = h.wantHide;
            std::fprintf(stderr, "[extras] main window %s\n", h.hidden ? "hidden" : "shown"); std::fflush(stderr);
        }
    }

    void shutdown()
    {
        HostImpl &h = impl();
        for (auto &f : h.features)
            f->shutdown();
        gate::set(gate::Menu, false);
        gate::set(gate::Session, false);
    }

    int runStandalone()
    {
        HostImpl &h = impl();
        h.standalone = true;
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        InitWindow(1280, 720, "PS2-Recomp | PC Extras");
        SetTargetFPS(60);
        SetExitKey(KEY_NULL);
        gate::bind(nullptr);
        h.init();
        h.settings.enabled = true;
        h.open();
        while (!WindowShouldClose() && !h.quit)
        {
            padinput::global().update();
            BeginDrawing();
            ClearBackground(Color{10, 14, 26, 255});
            h.frame();
            EndDrawing();
            if (h.hidden)
                WaitTime(0.016);
            if (h.wantHide != h.hidden)
            {
                if (h.wantHide) SetWindowState(FLAG_WINDOW_HIDDEN);
                else { ClearWindowState(FLAG_WINDOW_HIDDEN); SetWindowFocused(); }
                h.hidden = h.wantHide;
            }
            // closing the standalone menu (Esc at the root) quits; a feature is never left running
            if (h.screen == HostImpl::Screen::Closed)
                break;
        }
        shutdown();
        CloseWindow();
        return 0;
    }
}
