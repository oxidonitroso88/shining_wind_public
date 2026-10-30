#ifndef PS2_STORY_TEXT_H
#define PS2_STORY_TEXT_H

#include <string>

class PS2Runtime;

namespace ps2_story_text
{
    // Story/script text substitution (docs/translation-story-bss-2026-10-17.md). The script VM copies every message line of a script's data pool into a buffer
    // with strcpy (0x197818, called from the message native at 0x1E4EF0) before handing it to the window; this hook replaces the copied bytes by the English
    // line when the Japanese line is in the translation table, so the English may be any length. Table: PS2X_STORY_EN=<file> (default mods/story_en.tsv),
    // one entry per line: hex(Shift-JIS Japanese bytes) TAB hex(Shift-JIS English bytes). PS2X_STORY_EN=off disables it.
    void installFromEnvironment(PS2Runtime &runtime);
    // Absolute path of the table that is loaded ("" when the substitution is off).
    const std::string &loadedPath();
}

#endif // PS2_STORY_TEXT_H
