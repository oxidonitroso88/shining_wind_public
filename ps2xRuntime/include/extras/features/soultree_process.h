#ifndef PC_EXTRAS_SOULTREE_PROCESS_H
#define PC_EXTRAS_SOULTREE_PROCESS_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

// PC Extras - Soul Tree Events: the isolated event session as an operating-system process. No raylib here (the Windows headers and raylib clash).
//
// One child process per event: `<exe> "<elf>" --soul-event=<character>:<door>`. Its working directory, memory card folder and a copy of the player's
// input configuration live in a private temporary folder that is deleted when the session ends; only a short whitelist of the parent's PS2X_* settings
// (picture / audio / HD pack / translation, all read-only) is passed on. The child belongs to a job object that kills it when the parent goes away,
// so an event never outlives PC Extras.
namespace pc_extras::soultree_process
{
    class Session
    {
    public:
        virtual ~Session() = default;
        virtual bool exited(int &exitCode) = 0;   // true once the child has ended (exitCode valid); never blocks
        virtual bool windowVisible() = 0;         // the child's own window is on screen
        virtual void terminate() = 0;             // kill now (no-op when it already ended)
        virtual std::string logTail() = 0;        // last lines of the child's stderr (for error reports)
        // The child's configuration / diagnostic lines ("[config]", "[gs]", "[story]", "[hdpack] pack", "[soul]"), to compare with the parent's.
        virtual std::vector<std::string> diagnosticLines() = 0;
    };

    // `inputConfigPath`/`extrasConfigPath`: the player's files, only read (a copy of the first goes to the child; the second is passed by path).
    // `settings`: the PS2X_* variables the session is started with (see session_config::forChild); no other PS2X_* of the parent reaches it.
    std::unique_ptr<Session> start(const std::string &exePath, const std::string &elfPath, const std::string &character, int door,
                                   const std::string &inputConfigPath, const std::string &extrasConfigPath,
                                   const std::vector<std::pair<std::string, std::string>> &settings, std::string &error);

    // Removes the private folders of sessions whose parent process no longer exists (it was killed before it could clean up). Only folders named
    // "ps2x-soul-<pid>-<n>" directly under the temporary directory are ever touched. Returns how many were removed.
    int removeStaleFolders();

    // Pure helpers (tested): which environment variables of the parent are passed to the child.
    // true only for the names listed in `extra` (PS2X_EXTRAS_CHILD_PASS, a test-only switch the player never sets: it lets automated tests hand the child a
    // scripted pad). Every other PS2X_* variable of the parent is dropped; the session's real settings travel in `settings`.
    bool environmentPassThrough(const std::string &name, const std::string &extra = std::string());
}

#endif // PC_EXTRAS_SOULTREE_PROCESS_H
