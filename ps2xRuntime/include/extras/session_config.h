#ifndef PC_EXTRAS_SESSION_CONFIG_H
#define PC_EXTRAS_SESSION_CONFIG_H

#include <string>
#include <utility>
#include <vector>

// PC Extras - the configuration an event session inherits from the process that starts it, so that the session looks and sounds like the main game
// (language, fonts, HD textures, internal resolution) although its working directory is a private temporary folder.
//
// The game finds its resources through the working directory (`mods\`, `mods\story_en.tsv`, `hd_packs\`) unless a variable says otherwise, so a child with another
// working directory would silently fall back to the original Japanese game. The parent therefore resolves what it really uses and hands over absolute paths
// (or "off" when it uses nothing), plus the presentation switches it was started with.
namespace pc_extras::session_config
{
    using Setting = std::pair<std::string, std::string>;

    // Effective resource settings of this process (absolute paths, or "off"):
    std::string modsFolder();    // PS2X_MODS / default `mods` of the working directory (resource replacement: system fonts, menu tables)
    std::string storyTable();    // PS2X_STORY_EN / default `mods/story_en.tsv` search (translated dialogue)
    std::string hdPackFolder();  // PS2X_HD_PACK / default `hd_packs` of the working directory (HD and translated textures)
    bool hdPackOn();             // F10 state of the main game (true when no pack is loaded yet)

    // Everything a session must be started with (variable name -> value).
    std::vector<Setting> forChild();

    // One line for the logs, the same format in the parent ("parent") and in the session ("session") so they can be compared.
    std::string describe(const char *who);
}

#endif // PC_EXTRAS_SESSION_CONFIG_H
