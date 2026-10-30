#pragma once

// Host input for the two virtual PS2 pads (ports 0 and 1) and for the port's global functions (Speed Up).
//
// Shining Wind has a two player mode (the mirror world with "2P data", and a human controlled partner in battle: the game asks for a controller in port 2),
// so every physical device is routed to exactly one PS2 port and the two ports never see each other's input:
//   * the router owns a list of host devices (the keyboard and up to kMaxGamepads gamepads),
//   * each player (= PS2 port) has a source: Auto, Keyboard, a specific gamepad (by identity) or None,
//   * `sample(port)` returns the complete pad state of that port only (buttons, both analog sticks, connected flag),
//   * hot plugging never moves a gamepad to another player by itself: a player whose gamepad disappeared waits (its port reports "disconnected") and
//     only gets a gamepad back when it is the same device unambiguously, or when the user presses a button on the new one (claim),
//   * Speed Up is a global action of the port (hold or toggle, keyboard key and/or gamepad button combination); the buttons of a combination are removed from
//     the pad report so the games never see them.
// The router talks to the host through `InputBackend`, so it is fully testable without a window. Design and findings: docs/gamepad-two-players-2026-10-17.md.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace padinput
{
    constexpr int kPlayers = 2;      // PS2 ports 0 and 1
    constexpr int kMaxGamepads = 8;  // host devices considered

    // PS2 buttons as bits of the (active low) 16 bit report: data[2] | data[3] << 8
    enum PadBit : uint16_t
    {
        kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3, kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
        kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11, kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15
    };
    constexpr int kButtonCount = 16;

    // Logical host gamepad buttons (a "standard" gamepad layout; the backend translates its own numbering)
    enum class GpButton : int
    {
        DpadUp, DpadDown, DpadLeft, DpadRight, South, East, West, North, L1, R1, L2, R2, L3, R3, Back, Start, Guide, Count, None = -1
    };
    enum class GpAxis : int { LeftX, LeftY, RightX, RightY, Count };

    const char *ps2ButtonName(int bit);          // "cross" ...
    int ps2ButtonFromName(const std::string &name); // bit index or -1
    const char *gpButtonName(GpButton b);        // "south" ...
    GpButton gpButtonFromName(const std::string &name);

    // What the router needs from the host.
    class InputBackend
    {
    public:
        virtual ~InputBackend() = default;
        virtual bool ready() = 0;                                  // false: no window / no input available (everything reads as neutral)
        virtual bool gamepadPresent(int index) = 0;
        virtual std::string gamepadName(int index) = 0;
        virtual bool gamepadButton(int index, GpButton button) = 0;
        virtual float gamepadAxis(int index, GpAxis axis) = 0;     // -1..1, y down is positive
        virtual bool keyDown(int key) = 0;                         // raylib key codes
    };

    struct PadSample
    {
        uint16_t buttons = 0xFFFF; // active low
        uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
        bool connected = false;
    };

    enum class SourceKind : int { Auto, Keyboard, Gamepad, None };

    struct PlayerSource
    {
        SourceKind kind = SourceKind::Auto;
        std::string device; // Gamepad: identity "<name>#<ordinal>"
    };

    struct Config
    {
        PlayerSource player[kPlayers];
        float deadzone = 0.18f;
        // gamepad button for every PS2 button (shared by all gamepads) and keyboard keys (raylib key codes; 0 = unused)
        std::array<GpButton, kButtonCount> gamepadMap{};
        std::array<std::array<int, 2>, kButtonCount> keyboardMap{};
        // Speed Up: a keyboard key and/or a gamepad combination (all buttons down); `toggle`: a press switches it, otherwise it works while held
        int speedUpKey = 0;
        std::vector<GpButton> speedUpPad;
        bool speedUpToggle = false;
        Config();       // defaults
        std::string serialize() const;
        // Parses text written by serialize() (unknown / invalid lines are ignored and reported in `warnings`); starts from the defaults.
        static Config parse(const std::string &text, std::vector<std::string> *warnings = nullptr);
    };

    struct DeviceStatus
    {
        int index = -1;
        std::string name;
        std::string identity;      // "<name>#<ordinal>"
        int player = -1;           // bound player or -1
        uint16_t pressed = 0;      // PS2 buttons currently down on this device (bit = 1 pressed), for the settings UI
    };

    struct PlayerStatus
    {
        bool connected = false;
        bool waiting = false;       // its gamepad went away and has not come back / been claimed
        std::string label;          // "Keyboard", "Gamepad: <name>", "-"
        SourceKind kind = SourceKind::Auto;
    };

    class PadRouter
    {
    public:
        explicit PadRouter(InputBackend *backend = nullptr);
        void setBackend(InputBackend *backend);
        void setConfig(const Config &config);
        Config config() const;

        // Poll devices once (host/render thread, once per frame): hot plug, claims, per-player samples, Speed Up.
        void update();
        bool updatedWithin(int milliseconds) const; // true if update() ran that recently (the pad stubs call update() themselves when the render loop does not)

        PadSample sample(int port) const;          // any thread
        bool connected(int port) const;
        bool speedUpActive() const;
        // PC Extras: while blocked, sample() reports a neutral pad (still connected as before) so the game sees no input; devices() keeps reporting what is
        // pressed, which is what the Extras menu navigates with.
        void setGameInputBlocked(bool blocked);
        bool gameInputBlocked() const;
        PlayerStatus player(int port) const;
        std::vector<DeviceStatus> devices() const;
        // Assigns a gamepad (by identity) / the keyboard / nothing / auto to a player. Gamepad identities and the keyboard are exclusive to one player.
        void assign(int port, SourceKind kind, const std::string &device = std::string());
        // For binding UIs: the first gamepad button / keyboard key that went down since the last call, if any.
        bool captureGamepadButton(int index, GpButton &out);
        int captureKey();

    private:
        struct Impl;
        std::shared_ptr<Impl> m_impl;
    };

    // Process-wide router used by the pad stubs. Backed by raylib (window + gamepads); configured from `ps2x_input.cfg` (or PS2X_INPUT_CONFIG).
    PadRouter &global();
    // Loads the configuration file (once) and returns its path; saves with saveGlobalConfig().
    std::string configPath();
    bool saveGlobalConfig();
    bool reloadGlobalConfig();
    // Replaces the global router's backend (tests / headless); nullptr restores raylib.
    void setGlobalBackendForTests(InputBackend *backend);
}
