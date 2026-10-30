#ifndef PC_EXTRAS_HOST_H
#define PC_EXTRAS_HOST_H

#include "extras/extras_style.h"

#include <memory>
#include <string>

class PS2Runtime;

// PC Extras - registry of features and navigation. A feature is a self-contained screen ("Soul Tree Events", later "Music Player", "Gallery", ...);
// the host lists only the features that are really available, draws the menu, owns the hotkey and passes edge-triggered input to the active feature.
namespace pc_extras
{
    // One frame of navigation input (edges: true only on the frame the action starts, with key-repeat for the directions).
    struct Input
    {
        bool up = false, down = false, left = false, right = false;
        bool accept = false, back = false;
    };

    // What a feature can ask of the host.
    class Services
    {
    public:
        virtual ~Services() = default;
        virtual void hideMainWindow(bool hidden) = 0;      // hide / restore the player's window (an event session has its own)
        virtual const std::string &executablePath() const = 0;
        virtual const std::string &bootPath() const = 0;   // the game's ELF the main process was started with ("" when unknown)
        virtual const std::string &settingsPath() const = 0; // extras.cfg in use (may not exist)
        virtual void setSessionBlock(bool on) = 0;         // gate::Session: game input blocked, audio muted, emulation paused
    };

    class Feature
    {
    public:
        virtual ~Feature() = default;
        virtual const char *id() const = 0;       // stable identifier
        virtual const char *title() const = 0;    // menu entry
        virtual const char *summary() const = 0;  // one line under the menu
        // false: the feature is not offered at all (the menu never shows options that cannot work). `why` explains it for the log.
        virtual bool available(Services &services, std::string &why) const = 0;
        virtual void open(Services &services) = 0;
        // Every frame while the feature is the active screen. Return false to go back to the host menu.
        virtual bool update(Services &services, const Input &input) = 0;
        virtual void draw(Style &style) = 0;
        // Every frame, also while another screen is active or Extras is closed (e.g. polling a child process). Must be cheap.
        virtual void tick(Services &services) { (void)services; }
        // true: Extras must stay open (an event is running); the menu hotkey is ignored.
        virtual bool busy() const { return false; }
        // Leaving for good (window closed): stop anything the feature started.
        virtual void shutdown() {}
    };

    namespace host
    {
        void registerFeature(std::unique_ptr<Feature> feature);
        void registerBuiltInFeatures(); // idempotent
        void setLaunchContext(const std::string &executablePath, const std::string &bootPath);

        // Main process: called once per frame from the render loop after the game picture was drawn (render thread).
        void frame(PS2Runtime *runtime);
        void shutdown();

        // `--extras`: PC Extras in its own window, no emulation. Returns the process exit code.
        int runStandalone();

        // Event session child (--soul-event): the host shows `title` with a spinner until `ready()`; the hotkey asks to stop the event.
        void enterSessionMode(const std::string &title, bool (*ready)());

        // Hotkey / pad combination settings (extras.cfg next to ps2x_input.cfg); exposed for tests.
        struct Settings
        {
            bool enabled = true;
            int toggleKey = 0;          // raylib key code (default F7)
            unsigned toggleCombo = 0;   // PS2 button bits (padinput::PadBit), all down on one gamepad (default L3+R3)
            static Settings parse(const std::string &text, std::string *warnings = nullptr);
            static Settings defaults();
        };
        int keyFromName(const std::string &name); // "F7", "TAB", "P" ... -> raylib code, 0 when unknown
    }
}

#endif // PC_EXTRAS_HOST_H
