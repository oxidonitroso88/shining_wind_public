// Two player host input: see include/runtime/pad_input.h and docs/gamepad-two-players-2026-10-17.md.
#include "runtime/pad_input.h"
#include "ps2_session_policy.h"

#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace padinput
{
    namespace
    {
        const char *const kPs2Names[kButtonCount] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                                     "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        const char *const kGpNames[] = {"dpad_up", "dpad_down", "dpad_left", "dpad_right", "south", "east", "west", "north", "l1", "r1", "l2", "r2", "l3", "r3", "back", "start", "guide"};
        static_assert(sizeof(kGpNames) / sizeof(kGpNames[0]) == static_cast<size_t>(GpButton::Count), "gamepad button names");

        uint8_t axisToByte(float v)
        {
            v = std::clamp(v, -1.0f, 1.0f);
            return static_cast<uint8_t>(std::lround((v + 1.0f) * 127.5f));
        }

        float applyDeadzone(float v, float dz)
        {
            return std::fabs(v) < dz ? 0.0f : v;
        }

        std::string trim(const std::string &s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
                ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
                --b;
            return s.substr(a, b - a);
        }

        const char *kindName(SourceKind k)
        {
            switch (k)
            {
            case SourceKind::Keyboard: return "keyboard";
            case SourceKind::Gamepad: return "pad";
            case SourceKind::None: return "none";
            default: return "auto";
            }
        }
    } // namespace

    const char *ps2ButtonName(int bit) { return bit >= 0 && bit < kButtonCount ? kPs2Names[bit] : "?"; }

    int ps2ButtonFromName(const std::string &name)
    {
        for (int i = 0; i < kButtonCount; ++i)
            if (name == kPs2Names[i])
                return i;
        return -1;
    }

    const char *gpButtonName(GpButton b)
    {
        const int i = static_cast<int>(b);
        return i >= 0 && i < static_cast<int>(GpButton::Count) ? kGpNames[i] : "none";
    }

    GpButton gpButtonFromName(const std::string &name)
    {
        for (int i = 0; i < static_cast<int>(GpButton::Count); ++i)
            if (name == kGpNames[i])
                return static_cast<GpButton>(i);
        return GpButton::None;
    }

    // ---- configuration --------------------------------------------------------------------------------------------------------------------------
    Config::Config()
    {
        auto g = [this](int bit, GpButton b) { gamepadMap[bit] = b; };
        for (auto &b : gamepadMap)
            b = GpButton::None;
        g(4, GpButton::DpadUp);
        g(6, GpButton::DpadDown);
        g(7, GpButton::DpadLeft);
        g(5, GpButton::DpadRight);
        g(14, GpButton::South);
        g(13, GpButton::East);
        g(15, GpButton::West);
        g(12, GpButton::North);
        g(10, GpButton::L1);
        g(11, GpButton::R1);
        g(8, GpButton::L2);
        g(9, GpButton::R2);
        g(1, GpButton::L3);
        g(2, GpButton::R3);
        g(0, GpButton::Back);
        g(3, GpButton::Start);
        for (auto &k : keyboardMap)
            k = {0, 0};
        keyboardMap[4] = {KEY_UP, 0};
        keyboardMap[6] = {KEY_DOWN, 0};
        keyboardMap[7] = {KEY_LEFT, 0};
        keyboardMap[5] = {KEY_RIGHT, 0};
        keyboardMap[14] = {KEY_X, KEY_SPACE};
        keyboardMap[13] = {KEY_C, KEY_ESCAPE};
        keyboardMap[15] = {KEY_Z, KEY_KP_0};
        keyboardMap[12] = {KEY_V, KEY_KP_1};
        keyboardMap[10] = {KEY_Q, 0};
        keyboardMap[11] = {KEY_E, 0};
        keyboardMap[8] = {KEY_LEFT_SHIFT, 0};
        keyboardMap[9] = {KEY_RIGHT_SHIFT, 0};
        keyboardMap[3] = {KEY_ENTER, 0};
        keyboardMap[0] = {KEY_TAB, 0};
        // Speed Up: hold F4, or hold the right stick button (R3) of any gamepad (R3 is removed from the pad report while it is the Speed Up button)
        speedUpKey = KEY_F4;
        speedUpPad = {GpButton::R3};
        speedUpToggle = false;
    }

    std::string Config::serialize() const
    {
        std::ostringstream o;
        o << "# ps2x input configuration (written by the port; edit by hand or use the Controllers window, F2)\n";
        o << "version=1\n";
        for (int p = 0; p < kPlayers; ++p)
        {
            o << "player" << (p + 1) << "=" << kindName(player[p].kind);
            if (player[p].kind == SourceKind::Gamepad)
                o << ":" << player[p].device;
            o << "\n";
        }
        o << "deadzone=" << deadzone << "\n";
        for (int b = 0; b < kButtonCount; ++b)
            o << "pad." << kPs2Names[b] << "=" << gpButtonName(gamepadMap[b]) << "\n";
        for (int b = 0; b < kButtonCount; ++b)
            o << "key." << kPs2Names[b] << "=" << keyboardMap[b][0] << "," << keyboardMap[b][1] << "\n";
        o << "speedup.key=" << speedUpKey << "\n";
        o << "speedup.pad=";
        for (size_t i = 0; i < speedUpPad.size(); ++i)
            o << (i ? "+" : "") << gpButtonName(speedUpPad[i]);
        o << "\n";
        o << "speedup.mode=" << (speedUpToggle ? "toggle" : "hold") << "\n";
        return o.str();
    }

    Config Config::parse(const std::string &text, std::vector<std::string> *warnings)
    {
        Config c;
        auto warn = [&](const std::string &m) {
            if (warnings)
                warnings->push_back(m);
        };
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                warn("ignored line: " + line);
                continue;
            }
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "version")
                continue;
            if (key == "player1" || key == "player2")
            {
                PlayerSource src;
                if (value == "auto")
                    src.kind = SourceKind::Auto;
                else if (value == "keyboard")
                    src.kind = SourceKind::Keyboard;
                else if (value == "none")
                    src.kind = SourceKind::None;
                else if (value.rfind("pad:", 0) == 0 && value.size() > 4)
                {
                    src.kind = SourceKind::Gamepad;
                    src.device = value.substr(4);
                }
                else
                {
                    warn("bad " + key + ": " + value);
                    continue;
                }
                c.player[key == "player1" ? 0 : 1] = src;
            }
            else if (key == "deadzone")
            {
                const float v = static_cast<float>(std::atof(value.c_str()));
                if (v >= 0.0f && v < 0.9f)
                    c.deadzone = v;
                else
                    warn("bad deadzone: " + value);
            }
            else if (key.rfind("pad.", 0) == 0)
            {
                const int b = ps2ButtonFromName(key.substr(4));
                const GpButton g = gpButtonFromName(value);
                if (b < 0 || (g == GpButton::None && value != "none"))
                    warn("bad pad mapping: " + line);
                else
                    c.gamepadMap[b] = g;
            }
            else if (key.rfind("key.", 0) == 0)
            {
                const int b = ps2ButtonFromName(key.substr(4));
                int a = 0, d = 0;
                if (b < 0 || std::sscanf(value.c_str(), "%d,%d", &a, &d) < 1)
                    warn("bad key mapping: " + line);
                else
                    c.keyboardMap[b] = {a, d};
            }
            else if (key == "speedup.key")
                c.speedUpKey = std::atoi(value.c_str());
            else if (key == "speedup.pad")
            {
                c.speedUpPad.clear();
                std::istringstream parts(value);
                std::string part;
                bool ok = true;
                while (std::getline(parts, part, '+'))
                {
                    part = trim(part);
                    if (part.empty())
                        continue;
                    const GpButton g = gpButtonFromName(part);
                    if (g == GpButton::None)
                        ok = false;
                    else
                        c.speedUpPad.push_back(g);
                }
                if (!ok)
                {
                    warn("bad speedup.pad: " + value);
                    c.speedUpPad = Config().speedUpPad;
                }
            }
            else if (key == "speedup.mode")
                c.speedUpToggle = value == "toggle";
            else
                warn("unknown key: " + key);
        }
        // the keyboard belongs to one player at most
        if (c.player[0].kind == SourceKind::Keyboard && c.player[1].kind == SourceKind::Keyboard)
        {
            warn("both players use the keyboard: player2 set to auto");
            c.player[1].kind = SourceKind::Auto;
        }
        return c;
    }

    // ---- router -------------------------------------------------------------------------------------------------------------------------------
    struct PadRouter::Impl
    {
        mutable std::mutex mutex;
        std::atomic<bool> gameBlocked{false}; // PC Extras in front: the game reads a neutral pad
        InputBackend *backend = nullptr;
        Config config;

        struct Dev
        {
            bool present = false;
            std::string name;
            std::string identity;
            int player = -1;
            uint16_t prevDown = 0;       // for edge detection of the capture helpers
            bool prevActive = false;
            std::array<bool, static_cast<size_t>(GpButton::Count)> prevButtons{};
        };
        std::array<Dev, kMaxGamepads> dev{};
        int bound[kPlayers] = {-1, -1};
        bool waiting[kPlayers] = {false, false};
        bool everBound[kPlayers] = {false, false};
        std::string lastName[kPlayers];
        PadSample samples[kPlayers];
        uint16_t pressedNow[kMaxGamepads]{};
        bool speedUp = false;
        bool speedUpPrevHeld = false;
        int keyPrev[512]{};
        int capturedKey = 0;
        std::array<std::array<bool, static_cast<size_t>(GpButton::Count)>, kMaxGamepads> pendingPress{};
        bool keyboardEdge[512]{};
        int keyboardOwner = -1;
        std::chrono::steady_clock::time_point lastUpdate{};

        int deviceLimit() const { return backend ? std::min(kMaxGamepads, 4) : 0; }
    };

    PadRouter::PadRouter(InputBackend *backend) : m_impl(std::make_shared<Impl>())
    {
        m_impl->backend = backend;
    }

    void PadRouter::setBackend(InputBackend *backend)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->backend = backend;
        for (auto &d : m_impl->dev)
            d = Impl::Dev{};
        for (int p = 0; p < kPlayers; ++p)
        {
            m_impl->bound[p] = -1;
            m_impl->waiting[p] = false;
            m_impl->everBound[p] = false;
        }
    }

    void PadRouter::setConfig(const Config &config)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        // a changed assignment of a player releases the gamepad that was bound to it (it is bound again by the rules of update())
        for (int p = 0; p < kPlayers; ++p)
        {
            const PlayerSource &was = m_impl->config.player[p];
            const PlayerSource &now = config.player[p];
            if (was.kind != now.kind || was.device != now.device)
            {
                if (m_impl->bound[p] >= 0)
                {
                    m_impl->dev[m_impl->bound[p]].player = -1;
                    m_impl->bound[p] = -1;
                }
                m_impl->waiting[p] = false;
            }
        }
        m_impl->config = config;
    }

    Config PadRouter::config() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->config;
    }

    namespace
    {
        bool anyActivity(InputBackend &b, int index, float dz)
        {
            for (int i = 0; i < static_cast<int>(GpButton::Count); ++i)
                if (b.gamepadButton(index, static_cast<GpButton>(i)))
                    return true;
            for (int a = 0; a < static_cast<int>(GpAxis::Count); ++a)
                if (std::fabs(b.gamepadAxis(index, static_cast<GpAxis>(a))) > std::max(0.5f, dz))
                    return true;
            return false;
        }
    } // namespace

    bool PadRouter::updatedWithin(int milliseconds) const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->lastUpdate != std::chrono::steady_clock::time_point{} &&
               std::chrono::steady_clock::now() - m_impl->lastUpdate < std::chrono::milliseconds(milliseconds);
    }

    void PadRouter::update()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl &s = *m_impl;
        s.lastUpdate = std::chrono::steady_clock::now();
        InputBackend *b = s.backend;
        const Config &cfg = s.config;
        const bool ready = b && b->ready();

        // 1. devices present now, with identities ("<name>#<ordinal among the present ones with that name>")
        std::map<std::string, int> ordinals;
        for (int i = 0; i < kMaxGamepads; ++i)
        {
            Impl::Dev &d = s.dev[i];
            const bool present = ready && i < s.deviceLimit() && b->gamepadPresent(i);
            const std::string name = present ? b->gamepadName(i) : std::string();
            if (present && d.present && d.name != name)
            {
                // a different device appeared at the same host index between two updates: it is a new device
                if (d.player >= 0)
                {
                    s.lastName[d.player] = d.name;
                    s.waiting[d.player] = true;
                    s.bound[d.player] = -1;
                }
                d = Impl::Dev{};
            }
            if (!present && d.present && d.player >= 0)
            {
                // unplugged: the player waits for ITS device
                s.lastName[d.player] = d.name;
                s.waiting[d.player] = true;
                s.bound[d.player] = -1;
            }
            if (!present)
            {
                d = Impl::Dev{};
                continue;
            }
            d.present = true;
            d.name = name;
            d.identity = name + "#" + std::to_string(ordinals[name]++);
        }

        // 2. which players still need a gamepad
        auto wantsPad = [&](int p) { return cfg.player[p].kind == SourceKind::Auto || cfg.player[p].kind == SourceKind::Gamepad; };
        auto unbound = [&]() {
            std::vector<int> v;
            for (int i = 0; i < kMaxGamepads; ++i)
                if (s.dev[i].present && s.dev[i].player < 0)
                    v.push_back(i);
            return v;
        };
        auto bind = [&](int p, int i) {
            s.bound[p] = i;
            s.dev[i].player = p;
            s.waiting[p] = false;
            s.everBound[p] = true;
            s.lastName[p] = s.dev[i].name;
        };
        // explicit gamepad assignments first: exact identity, or the only gamepad with that name; several candidates need a claim (button press)
        for (int p = 0; p < kPlayers; ++p)
        {
            if (cfg.player[p].kind != SourceKind::Gamepad || s.bound[p] >= 0)
                continue;
            const std::string &wanted = cfg.player[p].device;
            const std::string wantedName = wanted.substr(0, wanted.rfind('#'));
            std::vector<int> byName;
            int exact = -1;
            for (int i : unbound())
            {
                if (s.dev[i].identity == wanted)
                    exact = i;
                if (s.dev[i].name == wantedName)
                    byName.push_back(i);
            }
            if (exact >= 0 && byName.size() == 1)
                bind(p, exact);
            else if (!byName.empty())
            {
                // same model several times: the one that is pressed claims the player
                for (int i : byName)
                    if (anyActivity(*b, i, cfg.deadzone))
                    {
                        bind(p, i);
                        break;
                    }
                if (s.bound[p] < 0 && exact >= 0 && !s.waiting[p])
                    bind(p, exact); // first binding at start up: take the exact identity
            }
        }
        // automatic players: first start up / hot plug -> lowest free Auto player; a waiting player only gets its own (same name) gamepad back when
        // that is unambiguous, otherwise a button press on any free gamepad claims it
        for (;;)
        {
            const std::vector<int> free = unbound();
            if (free.empty())
                break;
            bool progressed = false;
            // waiting players first
            for (int p = 0; p < kPlayers && !progressed; ++p)
            {
                if (cfg.player[p].kind != SourceKind::Auto || s.bound[p] >= 0 || !s.waiting[p])
                    continue;
                std::vector<int> same;
                for (int i : free)
                    if (s.dev[i].name == s.lastName[p])
                        same.push_back(i);
                int waitingCount = 0;
                for (int q = 0; q < kPlayers; ++q)
                    waitingCount += (cfg.player[q].kind == SourceKind::Auto && s.bound[q] < 0 && s.waiting[q]) ? 1 : 0;
                if (same.size() == 1 && waitingCount == 1 && free.size() == 1)
                {
                    bind(p, same[0]);
                    progressed = true;
                }
                else
                {
                    for (int i : free)
                        if (anyActivity(*b, i, cfg.deadzone))
                        {
                            bind(p, i);
                            progressed = true;
                            break;
                        }
                }
            }
            if (progressed)
                continue;
            // new gamepads fill the lowest free Auto player that has never had one (waiting players keep waiting for their device)
            for (int p = 0; p < kPlayers && !progressed; ++p)
            {
                if (cfg.player[p].kind != SourceKind::Auto || s.bound[p] >= 0 || s.waiting[p])
                    continue;
                bind(p, free[0]);
                progressed = true;
            }
            if (!progressed)
                break;
        }
        for (int p = 0; p < kPlayers; ++p)
            if (!wantsPad(p) && s.bound[p] >= 0)
            {
                s.dev[s.bound[p]].player = -1;
                s.bound[p] = -1;
            }

        // 3. keyboard owner: an explicit assignment, otherwise the first Auto player (in addition to that player's gamepad)
        s.keyboardOwner = -1;
        for (int p = 0; p < kPlayers; ++p)
            if (cfg.player[p].kind == SourceKind::Keyboard)
                s.keyboardOwner = p;
        if (s.keyboardOwner < 0)
            for (int p = 0; p < kPlayers; ++p)
                if (cfg.player[p].kind == SourceKind::Auto)
                {
                    s.keyboardOwner = p;
                    break;
                }

        // 4. Speed Up (global): keyboard key and/or a gamepad combination on any present gamepad
        bool held = ready && cfg.speedUpKey > 0 && b->keyDown(cfg.speedUpKey);
        bool comboDown[kMaxGamepads] = {};
        if (ready && !cfg.speedUpPad.empty())
            for (int i = 0; i < kMaxGamepads; ++i)
            {
                if (!s.dev[i].present)
                    continue;
                bool all = true;
                for (GpButton g : cfg.speedUpPad)
                    all = all && b->gamepadButton(i, g);
                comboDown[i] = all;
                held = held || all;
            }
        if (cfg.speedUpToggle)
        {
            if (held && !s.speedUpPrevHeld)
                s.speedUp = !s.speedUp;
        }
        else
            s.speedUp = held;
        s.speedUpPrevHeld = held;

        // 5. per player samples
        auto padReport = [&](int i, PadSample &out, bool merge) {
            // buttons of one gamepad
            uint16_t pressed = 0;
            for (int bit = 0; bit < kButtonCount; ++bit)
            {
                const GpButton g = cfg.gamepadMap[bit];
                if (g == GpButton::None)
                    continue;
                bool down = b->gamepadButton(i, g);
                if (down && comboDown[i])
                    for (GpButton sg : cfg.speedUpPad)
                        if (sg == g)
                            down = false; // the Speed Up button never reaches the game
                if (down)
                    pressed |= static_cast<uint16_t>(1u << bit);
            }
            (void)merge;
            out.buttons = static_cast<uint16_t>(out.buttons & ~pressed);
            const float dz = cfg.deadzone;
            out.lx = axisToByte(applyDeadzone(b->gamepadAxis(i, GpAxis::LeftX), dz));
            out.ly = axisToByte(applyDeadzone(b->gamepadAxis(i, GpAxis::LeftY), dz));
            out.rx = axisToByte(applyDeadzone(b->gamepadAxis(i, GpAxis::RightX), dz));
            out.ry = axisToByte(applyDeadzone(b->gamepadAxis(i, GpAxis::RightY), dz));
            return pressed;
        };
        for (int i = 0; i < kMaxGamepads; ++i)
            s.pressedNow[i] = 0;
        for (int p = 0; p < kPlayers; ++p)
        {
            PadSample out;
            const SourceKind kind = cfg.player[p].kind;
            bool connected = false;
            if (ready && s.bound[p] >= 0 && (kind == SourceKind::Auto || kind == SourceKind::Gamepad))
            {
                s.pressedNow[s.bound[p]] = padReport(s.bound[p], out, false);
                connected = true;
            }
            if (s.keyboardOwner == p && (kind == SourceKind::Keyboard || kind == SourceKind::Auto))
            {
                if (ready)
                {
                    for (int bit = 0; bit < kButtonCount; ++bit)
                        for (int key : cfg.keyboardMap[bit])
                            if (key > 0 && b->keyDown(key) && !(key == cfg.speedUpKey))
                                out.buttons = static_cast<uint16_t>(out.buttons & ~(1u << bit));
                    // WASD pushes the left analog stick (Shining Wind walks with the stick and ignores the D-pad in the field)
                    const int kx = (b->keyDown(KEY_D) ? 1 : 0) - (b->keyDown(KEY_A) ? 1 : 0);
                    const int ky = (b->keyDown(KEY_S) ? 1 : 0) - (b->keyDown(KEY_W) ? 1 : 0);
                    if (kx != 0)
                        out.lx = kx > 0 ? 0xFF : 0x00;
                    if (ky != 0)
                        out.ly = ky > 0 ? 0xFF : 0x00;
                }
                // The keyboard is always a connected pad for its player (also headless: it simply reads neutral)
                connected = true;
            }
            out.connected = connected && kind != SourceKind::None;
            s.samples[p] = out;
        }
    }

    PadSample PadRouter::sample(int port) const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (port < 0 || port >= kPlayers)
            return PadSample{};
        if (m_impl->gameBlocked.load(std::memory_order_relaxed))
        {
            PadSample neutral;
            neutral.connected = m_impl->samples[port].connected;
            return neutral;
        }
        return m_impl->samples[port];
    }

    void PadRouter::setGameInputBlocked(bool blocked) { m_impl->gameBlocked.store(blocked, std::memory_order_relaxed); }
    bool PadRouter::gameInputBlocked() const { return m_impl->gameBlocked.load(std::memory_order_relaxed); }

    bool PadRouter::connected(int port) const { return sample(port).connected; }

    bool PadRouter::speedUpActive() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        return m_impl->speedUp;
    }

    PlayerStatus PadRouter::player(int port) const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        PlayerStatus st;
        if (port < 0 || port >= kPlayers)
            return st;
        st.connected = m_impl->samples[port].connected;
        st.waiting = m_impl->waiting[port] && m_impl->bound[port] < 0;
        st.kind = m_impl->config.player[port].kind;
        std::string label;
        if (m_impl->bound[port] >= 0)
            label = "Gamepad: " + m_impl->dev[m_impl->bound[port]].name;
        if (m_impl->keyboardOwner == port && (st.kind == SourceKind::Keyboard || st.kind == SourceKind::Auto))
            label += (label.empty() ? "" : " + ") + std::string("Keyboard");
        st.label = label.empty() ? "-" : label;
        return st;
    }

    std::vector<DeviceStatus> PadRouter::devices() const
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        std::vector<DeviceStatus> out;
        for (int i = 0; i < kMaxGamepads; ++i)
            if (m_impl->dev[i].present)
            {
                DeviceStatus d;
                d.index = i;
                d.name = m_impl->dev[i].name;
                d.identity = m_impl->dev[i].identity;
                d.player = m_impl->dev[i].player;
                d.pressed = m_impl->pressedNow[i];
                out.push_back(d);
            }
        return out;
    }

    void PadRouter::assign(int port, SourceKind kind, const std::string &device)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl &s = *m_impl;
        if (port < 0 || port >= kPlayers)
            return;
        const int other = 1 - port;
        // exclusivity: the keyboard and a given gamepad serve one player only
        if (kind == SourceKind::Keyboard && s.config.player[other].kind == SourceKind::Keyboard)
            s.config.player[other] = PlayerSource{};
        if (kind == SourceKind::Gamepad && s.config.player[other].kind == SourceKind::Gamepad && s.config.player[other].device == device)
            s.config.player[other] = PlayerSource{};
        if (s.bound[other] >= 0 && kind == SourceKind::Gamepad && s.dev[s.bound[other]].identity == device)
        {
            s.dev[s.bound[other]].player = -1;
            s.bound[other] = -1;
            s.waiting[other] = true;
        }
        s.config.player[port].kind = kind;
        s.config.player[port].device = kind == SourceKind::Gamepad ? device : std::string();
        if (s.bound[port] >= 0)
        {
            s.dev[s.bound[port]].player = -1;
            s.bound[port] = -1;
        }
        s.waiting[port] = false;
        s.everBound[port] = kind != SourceKind::Auto ? true : false;
    }

    bool PadRouter::captureGamepadButton(int index, GpButton &out)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl &s = *m_impl;
        if (!s.backend || index < 0 || index >= kMaxGamepads || !s.dev[index].present || !s.backend->ready())
            return false;
        bool found = false;
        for (int g = 0; g < static_cast<int>(GpButton::Count); ++g)
        {
            const bool now = s.backend->gamepadButton(index, static_cast<GpButton>(g));
            if (now && !s.dev[index].prevButtons[g] && !found)
            {
                out = static_cast<GpButton>(g);
                found = true;
            }
            s.dev[index].prevButtons[g] = now;
        }
        return found;
    }

    int PadRouter::captureKey()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        Impl &s = *m_impl;
        if (!s.backend || !s.backend->ready())
            return 0;
        int found = 0;
        for (int k = 1; k < 512; ++k)
        {
            const bool now = s.backend->keyDown(k);
            if (now && !s.keyPrev[k] && !found)
                found = k;
            s.keyPrev[k] = now;
        }
        return found;
    }

    // ---- raylib backend, global router, configuration file -----------------------------------------------------------------------------------
    namespace
    {
        class RaylibBackend final : public InputBackend
        {
        public:
            bool ready() override { return IsWindowReady(); }
            bool gamepadPresent(int index) override { return IsGamepadAvailable(index); }
            std::string gamepadName(int index) override
            {
                const char *n = GetGamepadName(index);
                return n ? n : "Gamepad";
            }
            bool gamepadButton(int index, GpButton b) override
            {
                static const int map[] = {GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
                                          GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_LEFT, GAMEPAD_BUTTON_RIGHT_FACE_UP,
                                          GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1, GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
                                          GAMEPAD_BUTTON_LEFT_THUMB, GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE_RIGHT, GAMEPAD_BUTTON_MIDDLE};
                const int i = static_cast<int>(b);
                return i >= 0 && i < static_cast<int>(GpButton::Count) && IsGamepadButtonDown(index, map[i]);
            }
            float gamepadAxis(int index, GpAxis a) override
            {
                static const int map[] = {GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y, GAMEPAD_AXIS_RIGHT_X, GAMEPAD_AXIS_RIGHT_Y};
                return GetGamepadAxisMovement(index, map[static_cast<int>(a)]);
            }
            bool keyDown(int key) override { return IsKeyDown(key); }
        };

        struct Global
        {
            RaylibBackend raylib;
            InputBackend *backend = &raylib;
            PadRouter router;
            std::string path;
            bool loaded = false;
            Global() : router(&raylib) {}
        };

        Global &g()
        {
            static Global instance;
            return instance;
        }

        void loadConfigInto(Global &gl)
        {
            const char *env = std::getenv("PS2X_INPUT_CONFIG");
            gl.path = (env && env[0]) ? env : "ps2x_input.cfg";
            std::ifstream in(gl.path, std::ios::binary);
            Config cfg;
            if (in)
            {
                std::stringstream buf;
                buf << in.rdbuf();
                std::vector<std::string> warnings;
                cfg = Config::parse(buf.str(), &warnings);
                for (const std::string &w : warnings)
                    std::fprintf(stderr, "[input] %s: %s\n", gl.path.c_str(), w.c_str());
                std::fprintf(stderr, "[input] configuration loaded from %s\n", gl.path.c_str());
            }
            gl.router.setConfig(cfg);
            gl.loaded = true;
        }
    } // namespace

    PadRouter &global()
    {
        Global &gl = g();
        if (!gl.loaded)
            loadConfigInto(gl);
        return gl.router;
    }

    std::string configPath()
    {
        (void)global();
        return g().path;
    }

    bool saveGlobalConfig()
    {
        if (ps2x::session::readOnly())
            return false; // throw-away session: the player's key bindings are read, never written
        Global &gl = g();
        (void)global();
        std::ofstream out(gl.path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << gl.router.config().serialize();
        return static_cast<bool>(out);
    }

    bool reloadGlobalConfig()
    {
        Global &gl = g();
        loadConfigInto(gl);
        return true;
    }

    void setGlobalBackendForTests(InputBackend *backend)
    {
        Global &gl = g();
        (void)global();
        gl.backend = backend ? backend : &gl.raylib;
        gl.router.setBackend(gl.backend);
    }
} // namespace padinput
