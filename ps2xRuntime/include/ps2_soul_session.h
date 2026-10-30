#ifndef PS2_SOUL_SESSION_H
#define PS2_SOUL_SESSION_H

#include <cstdint>
#include <string>
#include <string_view>

class PS2Runtime;

// PC Extras - "Soul Tree Events" viewer, session side (docs/soultree-session-isolation-2026-10-24.md, docs/soultree-pcextras-modules-2026-10-24.md).
//
// A session is a throw-away process that plays ONE heart-door event with the game's own script interpreter. It is started by the parent
// (the selector) with `--soul-event=<character>:<door>` and:
//   * boots straight into a new-game state (no title, no pad input, no save needed) on town40_01, which has no prologue event;
//   * skips the piece puzzle (no Soul Pieces are spent, nothing is unlocked);
//   * ends (exit code kExitEventFinished) the instant the event's own content is over, before the game's closing tail (SetCharInfo, ChangeMap);
//   * can never write to the player's data: every write to the memory card / host files is refused by construction (see ps2_session_policy.h).
// Nothing here is active in a normal run: without the argument the module installs nothing.
namespace pc_extras::soul_session
{
    constexpr int kExitEventFinished = 42; // the event played to its end (the parent shows the selector again)
    constexpr int kExitStoppedByUser = 43; // the user stopped the event from the session window
    constexpr int kExitUnsupported = 2;    // not the executable the viewer was written for / hooks could not be installed

    struct Character
    {
        const char *id;        // internal name = the game's own file-name romanisation (pac_souldoor_<id>01)
        const char *display;   // name shown by the selector
        uint32_t firstLabel;   // script label of door 1 (door n = firstLabel + 2 * (n - 1))
        uint32_t doorVariable; // global script variable that holds (door - 1) for this character
        int doors;             // number of doors that really exist
    };

    const Character *characters(size_t &count); // the 13 characters of the 55 events, in the game's id order
    const Character *findCharacter(std::string_view id);
    std::string doorTitle(int door);            // "First Door of the Heart" ... "Fifth Door of the Heart"

    // "seena:5" -> request (false when the character or door does not exist)
    struct Request
    {
        uint32_t label = 0, variable = 0, value = 0;
    };
    bool parseRequest(std::string_view text, Request &out);

    // Called by main() for the `--soul-event=` argument; installIfRequested() is called from the game overrides once the ELF is loaded.
    void setRequest(const Request &request);
    bool requested();
    bool eventStarted(); // the event has been handed to the game's script manager (session child)
    void installIfRequested(PS2Runtime &runtime);
}

#endif // PS2_SOUL_SESSION_H
