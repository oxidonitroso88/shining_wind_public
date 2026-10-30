// See include/extras/session_config.h.
#include "extras/session_config.h"

#include "ps2_story_text.h"
#include "runtime/gs/hd_pack.h"
#include "runtime/gs/tex_provenance.h"
#include "ps2x/iop/mod_overlay.h"

#include <cstdlib>
#include <filesystem>

namespace pc_extras::session_config
{
    namespace
    {
        std::string absolutePath(const std::string &path)
        {
            std::error_code ec;
            const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
            return ec ? path : absolute.lexically_normal().string();
        }

        const char *const kPresentationVariables[] = {"PS2X_GS_BACKEND", "PS2X_GS_SCALE", "PS2X_GS_OUTPUT", "PS2X_GS_GPU_DOWNSCALE", "PS2X_GS_PROMOTE",
                                                      "PS2X_GS_SSTEX", "PS2X_GS_HD_SSAA", "PS2X_WINDOW_SCALE", "PS2X_ASCII_PITCH", "PS2X_AUDIO",
                                                      "PS2X_AUDIO_LATENCY_MS", "PS2X_AUDIO_BUFFER_MS"};
    }

    std::string modsFolder()
    {
        const std::string folder = ps2x::iop::mods::resolveModFolder();
        return folder.empty() ? std::string("off") : absolutePath(folder);
    }

    std::string storyTable()
    {
        if (!ps2_story_text::loadedPath().empty())
            return ps2_story_text::loadedPath();
        // not installed in this process (stand-alone Extras): the same search the game does
        std::error_code ec;
        if (const char *env = std::getenv("PS2X_STORY_EN"))
            return (std::string(env) == "off" || !std::filesystem::exists(env, ec)) ? std::string("off") : absolutePath(env);
        for (const char *candidate : {"mods/story_en.tsv", "../mods/story_en.tsv", "../../mods/story_en.tsv", "../../../mods/story_en.tsv"})
            if (std::filesystem::exists(candidate, ec))
                return absolutePath(candidate);
        return "off";
    }

    std::string hdPackFolder()
    {
        const std::string path = texprov::hdPackEffectivePath();
        return path.empty() ? std::string("off") : path;
    }

    bool hdPackOn() { return !hdpack::loaded() || hdpack::enabled(); }

    std::vector<Setting> forChild()
    {
        std::vector<Setting> out;
        out.emplace_back("PS2X_MODS", modsFolder());
        out.emplace_back("PS2X_STORY_EN", storyTable());
        out.emplace_back("PS2X_HD_PACK", hdPackFolder());
        out.emplace_back("PS2X_HD_PACK_STATE", hdPackOn() ? "on" : "off");
        for (const char *name : kPresentationVariables)
            if (const char *value = std::getenv(name))
                out.emplace_back(name, value);
        return out;
    }

    std::string describe(const char *who)
    {
        std::string s = std::string("[config] ") + who + ": mods=" + modsFolder() + " story=" + storyTable() + " hdpack=" + hdPackFolder() +
                        " hdpack_state=" + (hdPackOn() ? "on" : "off");
        for (const char *name : kPresentationVariables)
            if (const char *value = std::getenv(name))
                s += std::string(" ") + name + "=" + value;
        return s;
    }
}
