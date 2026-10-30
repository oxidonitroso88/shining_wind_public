// See include/ps2_story_text.h.
#include "ps2_story_text.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

namespace
{
    constexpr uint32_t kStrcpy = 0x197818u;
    constexpr uint32_t kMessageNativeLo = 0x1E4EF0u, kMessageNativeHi = 0x1E5050u; // the "message line" native
    PS2Runtime::RecompiledFunction g_originalStrcpy = nullptr;
    std::unordered_map<std::string, std::string> g_table;
    std::string g_loadedPath; // absolute path of the table in use ("" when none)
    long g_replaced = 0, g_seen = 0;

    int hexVal(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool unhex(const std::string &s, size_t from, size_t to, std::string &out)
    {
        out.clear();
        if (((to - from) & 1u) != 0)
            return false;
        for (size_t i = from; i + 1 < to + 0; i += 2)
        {
            const int a = hexVal(s[i]), b = hexVal(s[i + 1]);
            if (a < 0 || b < 0)
                return false;
            out.push_back(static_cast<char>(a * 16 + b));
        }
        return true;
    }

    void strcpyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
        if (src >= 0x100000u && src < 0x2000000u - 520u && dst >= 0x100000u && dst < 0x2000000u - 520u)
        {
            const uint8_t c = rdram[src];
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC))
            {
                size_t n = 0;
                while (n < 400u && rdram[src + n] != 0)
                    ++n;
                if (n < 400u)
                {
                    ++g_seen;
                    const auto it = g_table.find(std::string(reinterpret_cast<const char *>(rdram + src), n));
                    if (it != g_table.end())
                    {
                        const bool message = ra >= kMessageNativeLo && ra < kMessageNativeHi;
                        // message lines go to a 0x20C-byte stack buffer; other callers (string variables of the VM: 64-byte slots) get short text only
                        if (it->second.size() <= (message ? 500u : 63u))
                        {
                            std::memcpy(rdram + dst, it->second.c_str(), it->second.size() + 1u);
                            ++g_replaced;
                            SET_GPR_U32(ctx, 2, dst);
                            ctx->pc = ra;
                            return;
                        }
                    }
                }
            }
        }
        ctx->pc = kStrcpy;
        g_originalStrcpy(rdram, ctx, runtime);
    }
}

namespace ps2_story_text
{
    const std::string &loadedPath() { return g_loadedPath; }

    void installFromEnvironment(PS2Runtime &runtime)
    {
        std::string path;
        if (const char *env = std::getenv("PS2X_STORY_EN"))
        {
            if (std::string(env) == "off")
                return;
            path = env;
        }
        else
        {
            for (const char *cand : {"mods/story_en.tsv", "../mods/story_en.tsv", "../../mods/story_en.tsv", "../../../mods/story_en.tsv"})
            {
                if (std::filesystem::exists(cand))
                {
                    path = cand;
                    break;
                }
            }
        }
        if (path.empty() || !std::filesystem::exists(path))
            return;
        std::ifstream in(path, std::ios::binary);
        std::string line, jp, en;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t tab = line.find('\t');
            if (tab == std::string::npos)
                continue;
            if (unhex(line, 0, tab, jp) && unhex(line, tab + 1, line.size(), en) && !jp.empty())
                g_table[jp] = en;
        }
        g_originalStrcpy = runtime.hasFunction(kStrcpy) ? runtime.lookupFunction(kStrcpy) : nullptr;
        if (g_originalStrcpy == nullptr || g_table.empty())
        {
            std::cerr << "[story] not installed (table " << g_table.size() << ", strcpy " << (g_originalStrcpy != nullptr) << ")" << std::endl;
            return;
        }
        {
            std::error_code ec;
            const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
            g_loadedPath = ec ? path : absolute.lexically_normal().string();
        }
        if (runtime.replaceFunction(kStrcpy, &strcpyHook))
            std::cout << "[story] " << g_table.size() << " English lines loaded from " << path << std::endl;
    }
}
