// See include/extras/features/soultree_feature.h.
#include "extras/features/soultree_feature.h"

#include "extras/features/soultree_process.h"
#include "extras/session_config.h"
#include "ps2_soul_session.h"
#include "runtime/pad_input.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace pc_extras
{
    namespace
    {
        namespace ss = pc_extras::soul_session;

        class SoulTreeFeature final : public Feature
        {
        public:
            const char *id() const override { return "soultree"; }
            const char *title() const override { return "Soul Tree Events"; }
            const char *summary() const override
            {
                return "Replay any heart-door event, with its original voices, music and illustrations. Nothing is unlocked and your saved game is never touched.";
            }

            bool available(Services &services, std::string &why) const override
            {
#if !defined(_WIN32)
                why = "event sessions need Windows";
                return false;
#else
                std::error_code ec;
                if (services.bootPath().empty() || !std::filesystem::exists(services.bootPath(), ec))
                {
                    why = "the game's executable file (ELF) is not known: start PC Extras with the game's path";
                    return false;
                }
                if (services.executablePath().empty() || !std::filesystem::exists(services.executablePath(), ec))
                {
                    why = "the port's executable path is unknown";
                    return false;
                }
                return true;
#endif
            }

            void open(Services &) override
            {
                // leftovers of an event whose player process was killed (never touches a running session's folder)
                if (const int removed = soultree_process::removeStaleFolders(); removed > 0)
                    std::fprintf(stderr, "[extras] soul tree: removed %d stale session folder(s)\n", removed);
                character_ = std::min(character_, characterCount() - 1);
                focusDoors_ = false;
                phase_ = Phase::Select;
                message_.clear();
            }

            bool busy() const override { return phase_ == Phase::Starting || phase_ == Phase::Running; }

            void shutdown() override
            {
                if (child_)
                    child_->terminate();
                child_.reset();
            }

            void tick(Services &services) override
            {
                if (!child_)
                    return;
                int code = -1;
                if (child_->exited(code))
                {
                    finish(services, code);
                    return;
                }
                if (phase_ == Phase::Starting && child_->windowVisible())
                {
                    // the event has its own window now: the player's window steps aside (the game behind it stays paused and silent)
                    services.hideMainWindow(true);
                    phase_ = Phase::Running;
                }
            }

            bool update(Services &services, const Input &in) override
            {
                if (phase_ == Phase::Starting && in.back && child_)
                    child_->terminate(); // cancel while loading; tick() sees the exit
                if (phase_ == Phase::Starting || phase_ == Phase::Running)
                    return true; // the session owns the screen; it ends through tick()
                const ss::Character *chars = characters();
                const int count = characterCount();
                if (!focusDoors_)
                {
                    if (in.up) { character_ = (character_ + count - 1) % count; door_ = 0; }
                    if (in.down) { character_ = (character_ + 1) % count; door_ = 0; }
                    if (in.accept || in.right) { focusDoors_ = true; door_ = 0; message_.clear(); }
                    if (in.back) return false;
                }
                else
                {
                    const int doors = chars[character_].doors;
                    if (in.up) door_ = (door_ + doors - 1) % doors;
                    if (in.down) door_ = (door_ + 1) % doors;
                    if (in.left || in.back) { focusDoors_ = false; message_.clear(); }
                    else if (in.accept)
                        launch(services, chars[character_], door_ + 1);
                }
                return true;
            }

            void draw(Style &style) override
            {
                const ss::Character *chars = characters();
                const int count = characterCount();
                style.header("Soul Tree Events", "Heart doors: replay the events of each character");
                style.panel(Rect{64.0f, 176.0f, 430.0f, 462.0f}, false);
                for (int i = 0; i < count; ++i)
                    style.row(chars[i].display, Rect{76.0f, 184.0f + 34.5f * i, 406.0f, 34.0f}, i == character_ && !focusDoors_, true, std::string(),
                              i == character_ && focusDoors_);
                style.panel(Rect{514.0f, 176.0f, 702.0f, 462.0f}, false);
                const ss::Character &c = chars[character_];
                style.text(c.display, 540.0f, 194.0f, 36.0f, Tone::Accent);
                style.text(c.doors == 1 ? "1 heart door" : std::to_string(c.doors) + " heart doors", 540.0f, 240.0f, 22.0f, Tone::Dim);
                for (int d = 0; d < c.doors; ++d)
                    style.row(ss::doorTitle(d + 1), Rect{540.0f, 286.0f + 58.0f * d, 440.0f, 52.0f}, focusDoors_ && d == door_);
                // original artwork (portraits, backdrops) goes here once an ImageProvider is registered; without one nothing is drawn
                style.image(std::string("portrait:") + c.id, Rect{1000.0f, 200.0f, 196.0f, 300.0f});
                if (phase_ == Phase::Starting)
                {
                    style.panel(Rect{300.0f, 280.0f, 680.0f, 160.0f}, true);
                    style.spinner(380.0f, 360.0f, 30.0f);
                    style.text("Starting the event...", 440.0f, 322.0f, 30.0f);
                    style.text(current_, 440.0f, 366.0f, 22.0f, Tone::Dim);
                    style.text("Esc / (X)  Cancel", 440.0f, 400.0f, 20.0f, Tone::Dim);
                }
                else if (phase_ == Phase::Running)
                {
                    style.panel(Rect{300.0f, 280.0f, 680.0f, 160.0f}, true);
                    style.text("The event is playing in its own window.", 330.0f, 322.0f, 28.0f);
                    style.text("Stop it with the Extras hotkey inside that window, or close it.", 330.0f, 366.0f, 22.0f, Tone::Dim);
                }
                if (!message_.empty() && phase_ == Phase::Select)
                    style.text(message_, 540.0f, 586.0f, 24.0f, messageTone_);
                style.footer(focusDoors_ ? "Up/Down  Door      Enter / (O)  Play the event      Esc / (X)  Characters"
                                         : "Up/Down  Character      Enter / (O) / Right  Doors      Esc / (X)  Back");
            }

        private:
            enum class Phase { Select, Starting, Running };

            static const ss::Character *characters()
            {
                size_t n = 0;
                return ss::characters(n);
            }
            static int characterCount()
            {
                size_t n = 0;
                ss::characters(n);
                return static_cast<int>(n);
            }

            void launch(Services &services, const ss::Character &c, int door)
            {
                std::string error;
                std::string inputCfg;
                try { inputCfg = padinput::configPath(); } catch (...) {}
                const auto settings = session_config::forChild();
                std::fprintf(stderr, "%s\n", session_config::describe("parent").c_str()); std::fflush(stderr);
                child_ = soultree_process::start(services.executablePath(), services.bootPath(), c.id, door, inputCfg, services.settingsPath(), settings, error);
                if (!child_)
                {
                    message_ = "Could not start the event: " + error;
                    messageTone_ = Tone::Bad;
                    std::fprintf(stderr, "[extras] soul tree: %s\n", error.c_str()); std::fflush(stderr);
                    return;
                }
                current_ = std::string(c.display) + " - " + ss::doorTitle(door);
                std::fprintf(stderr, "[extras] soul tree: event started: %s:%d\n", c.id, door); std::fflush(stderr);
                services.setSessionBlock(true);
                phase_ = Phase::Starting;
            }

            void finish(Services &services, int code)
            {
                std::fprintf(stderr, "[extras] soul tree: event process ended with code %d\n", code); std::fflush(stderr);
                if (child_)
                    for (const std::string &line : child_->diagnosticLines())
                        std::fprintf(stderr, "[extras] child %s\n", line.c_str());
                std::fflush(stderr);
                if (code != ss::kExitEventFinished && code != ss::kExitStoppedByUser && code != 0)
                {
                    const std::string tail = child_ ? child_->logTail() : std::string();
                    if (!tail.empty())
                        std::fprintf(stderr, "[extras] soul tree: child log (tail):\n%s\n", tail.c_str()); std::fflush(stderr);
                }
                child_.reset(); // removes the private temporary folder
                services.setSessionBlock(false);
                services.hideMainWindow(false);
                phase_ = Phase::Select;
                if (code == ss::kExitEventFinished)
                {
                    message_ = "Finished: " + current_;
                    messageTone_ = Tone::Good;
                }
                else if (code == ss::kExitStoppedByUser || code == 0)
                {
                    message_ = "Stopped: " + current_;
                    messageTone_ = Tone::Warn;
                }
                else if (code == ss::kExitUnsupported)
                {
                    message_ = "This executable is not supported by the event viewer.";
                    messageTone_ = Tone::Bad;
                }
                else
                {
                    message_ = "The event ended unexpectedly (code " + std::to_string(code) + ").";
                    messageTone_ = Tone::Bad;
                }
            }

            Phase phase_ = Phase::Select;
            int character_ = 0, door_ = 0;
            bool focusDoors_ = false;
            std::string message_, current_;
            Tone messageTone_ = Tone::Dim;
            std::unique_ptr<soultree_process::Session> child_;
        };
    }

    std::unique_ptr<Feature> makeSoulTreeFeature() { return std::make_unique<SoulTreeFeature>(); }
}
