// HD texture pack manifest + image loader. See include/runtime/gs/hd_pack.h.
#include "runtime/gs/hd_pack.h"

#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>

namespace hdpack
{
    namespace
    {
        // ---- minimal JSON (objects, arrays, strings, numbers, booleans, null) ------------------------------------------------------------
        struct Json;
        using JsonObject = std::vector<std::pair<std::string, Json>>;
        using JsonArray = std::vector<Json>;
        struct Json
        {
            std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> v = nullptr;
            bool isNumber() const { return std::holds_alternative<double>(v); }
            bool isString() const { return std::holds_alternative<std::string>(v); }
            bool isObject() const { return std::holds_alternative<JsonObject>(v); }
            bool isArray() const { return std::holds_alternative<JsonArray>(v); }
            const Json *get(const std::string &key) const
            {
                if (!isObject())
                    return nullptr;
                for (const auto &kv : std::get<JsonObject>(v))
                    if (kv.first == key)
                        return &kv.second;
                return nullptr;
            }
        };

        struct Parser
        {
            const std::string &s;
            size_t i = 0;
            std::string err;
            explicit Parser(const std::string &text) : s(text) {}
            void ws()
            {
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                    ++i;
            }
            bool fail(const char *m)
            {
                if (err.empty())
                    err = std::string(m) + " at offset " + std::to_string(i);
                return false;
            }
            bool parseString(std::string &out)
            {
                if (i >= s.size() || s[i] != '"')
                    return fail("string expected");
                ++i;
                out.clear();
                while (i < s.size() && s[i] != '"')
                {
                    char c = s[i++];
                    if (c == '\\' && i < s.size())
                    {
                        const char e = s[i++];
                        switch (e)
                        {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'u':
                        {
                            if (i + 4 > s.size())
                                return fail("bad \\u escape");
                            const unsigned cp = static_cast<unsigned>(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                            i += 4;
                            if (cp < 0x80u)
                                out.push_back(static_cast<char>(cp));
                            else if (cp < 0x800u)
                            {
                                out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            else
                            {
                                out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
                            }
                            continue;
                        }
                        default: c = e; break; // \" \\ \/
                        }
                    }
                    out.push_back(c);
                }
                if (i >= s.size())
                    return fail("unterminated string");
                ++i;
                return true;
            }
            bool parseValue(Json &out, int depth = 0)
            {
                if (depth > 32)
                    return fail("nesting too deep");
                ws();
                if (i >= s.size())
                    return fail("value expected");
                const char c = s[i];
                if (c == '{')
                {
                    ++i;
                    JsonObject obj;
                    ws();
                    if (i < s.size() && s[i] == '}')
                    {
                        ++i;
                        out.v = std::move(obj);
                        return true;
                    }
                    for (;;)
                    {
                        ws();
                        std::string key;
                        if (!parseString(key))
                            return false;
                        ws();
                        if (i >= s.size() || s[i] != ':')
                            return fail("':' expected");
                        ++i;
                        Json val;
                        if (!parseValue(val, depth + 1))
                            return false;
                        obj.emplace_back(std::move(key), std::move(val));
                        ws();
                        if (i < s.size() && s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == '}')
                        {
                            ++i;
                            break;
                        }
                        return fail("',' or '}' expected");
                    }
                    out.v = std::move(obj);
                    return true;
                }
                if (c == '[')
                {
                    ++i;
                    JsonArray arr;
                    ws();
                    if (i < s.size() && s[i] == ']')
                    {
                        ++i;
                        out.v = std::move(arr);
                        return true;
                    }
                    for (;;)
                    {
                        Json val;
                        if (!parseValue(val, depth + 1))
                            return false;
                        arr.push_back(std::move(val));
                        ws();
                        if (i < s.size() && s[i] == ',')
                        {
                            ++i;
                            continue;
                        }
                        if (i < s.size() && s[i] == ']')
                        {
                            ++i;
                            break;
                        }
                        return fail("',' or ']' expected");
                    }
                    out.v = std::move(arr);
                    return true;
                }
                if (c == '"')
                {
                    std::string str;
                    if (!parseString(str))
                        return false;
                    out.v = std::move(str);
                    return true;
                }
                if (s.compare(i, 4, "true") == 0)
                {
                    i += 4;
                    out.v = true;
                    return true;
                }
                if (s.compare(i, 5, "false") == 0)
                {
                    i += 5;
                    out.v = false;
                    return true;
                }
                if (s.compare(i, 4, "null") == 0)
                {
                    i += 4;
                    out.v = nullptr;
                    return true;
                }
                char *end = nullptr;
                const double d = std::strtod(s.c_str() + i, &end);
                if (end == s.c_str() + i)
                    return fail("unexpected character");
                i = static_cast<size_t>(end - s.c_str());
                out.v = d;
                return true;
            }
        };

        bool getU64(const Json *j, uint64_t &out)
        {
            if (!j || !j->isNumber())
                return false;
            const double d = std::get<double>(j->v);
            if (d < 0.0 || d > 9007199254740992.0)
                return false;
            out = static_cast<uint64_t>(d);
            return true;
        }

        std::string getStr(const Json *j, const std::string &fallback = std::string())
        {
            return (j && j->isString()) ? std::get<std::string>(j->v) : fallback;
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
            {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (c == '\\')
                    c = '/';
            }
            return s;
        }

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // A region object: {"file"?, "stream_offset"?, "offset", "length", "sha1"?}. `file` defaults to the entry's file.
        bool parseRegion(const Json *j, const std::string &defaultFile, Region &out, std::string &why)
        {
            if (!j || !j->isObject())
            {
                why = "region missing";
                return false;
            }
            out.file = upper(getStr(j->get("file"), defaultFile));
            uint64_t v = 0;
            out.compressed = false;
            if (const Json *so = j->get("stream_offset"); so && !(std::holds_alternative<std::nullptr_t>(so->v)))
            {
                if (!getU64(so, v))
                {
                    why = "bad stream_offset";
                    return false;
                }
                out.compressed = true;
                out.streamOffset = v;
            }
            if (!getU64(j->get("offset"), out.offset) || !getU64(j->get("length"), out.length) || out.length == 0)
            {
                why = "region offset/length missing";
                return false;
            }
            out.sha1 = lower(getStr(j->get("sha1")));
            if (out.file.empty())
            {
                why = "region without file";
                return false;
            }
            return true;
        }

        std::string regionKey(const Region &r)
        {
            return r.file + "|" + (r.compressed ? std::to_string(r.streamOffset) : std::string("-")) + "|" + std::to_string(r.offset) + "|" + std::to_string(r.length);
        }

        // ---- state ---------------------------------------------------------------------------------------------------------------------------
        struct Decoded
        {
            std::vector<uint8_t> rgba;
            bool failed = false;
        };

        struct State
        {
            std::mutex mutex;
            bool loaded = false;
            std::atomic<bool> enabled{false};
            std::vector<std::unique_ptr<Entry>> entries;
            std::unordered_multimap<std::string, Entry *> byPixels;
            std::unordered_multimap<std::string, Entry *> byPixelsSha; // entries that carry pixels.sha1, by that hash (content fallback)
            Stats stats;
            // small LRU of decoded images (the GPU keeps its own copy; this only avoids decoding on repeated cache misses)
            struct Decoded
            {
                const Entry *entry;
                uint64_t variant; // 0 = plain RGBA image, otherwise the hash of the live palette an indexed image was composed with
                std::vector<uint8_t> data;
            };
            std::list<Decoded> lru;
            uint64_t lruBytes = 0;
            std::unordered_map<const Entry *, std::vector<uint8_t>> indexImages; // decoded index maps of indexed entries
            uint64_t indexBytes = 0;
            std::vector<uint8_t> scratch;
            std::unordered_map<const Entry *, bool> failedImages;
        };

        State &st()
        {
            static State s;
            return s;
        }

        constexpr uint64_t kDecodedKeep = 64ull << 20;
    } // namespace

    bool parseManifest(const std::string &text, const std::string &baseDir, std::vector<Entry> &out, std::string &error, uint64_t *skipped)
    {
        Parser p(text);
        Json root;
        if (!p.parseValue(root))
        {
            error = "manifest.json: " + p.err;
            return false;
        }
        if (!root.isObject() || getStr(root.get("format")) != "ps2x-hd-pack")
        {
            error = "manifest.json: not a ps2x-hd-pack manifest";
            return false;
        }
        uint64_t version = 0;
        if (!getU64(root.get("version"), version) || version != 1)
        {
            error = "manifest.json: unsupported version";
            return false;
        }
        const Json *textures = root.get("textures");
        if (!textures || !textures->isArray())
        {
            error = "manifest.json: 'textures' array missing";
            return false;
        }
        uint64_t skippedCount = 0;
        uint64_t index = 0;
        for (const Json &t : std::get<JsonArray>(textures->v))
        {
            ++index;
            Entry e;
            std::string why;
            e.id = getStr(t.get("id"), "#" + std::to_string(index));
            const std::string file = upper(getStr(t.get("file")));
            const Json *px = t.get("pixels");
            if (!t.isObject() || file.empty() || !px)
                why = "file/pixels missing";
            if (why.empty())
            {
                // pixels.stream_offset (or the entry level one) selects the LZSS block
                Json merged;
                JsonObject o = px->isObject() ? std::get<JsonObject>(px->v) : JsonObject{};
                if (!px->get("stream_offset"))
                    if (const Json *so = t.get("stream_offset"))
                        o.emplace_back("stream_offset", *so);
                merged.v = std::move(o);
                if (!parseRegion(&merged, file, e.pixels, why))
                    why = "pixels: " + why;
            }
            if (why.empty())
                if (const Json *pal = t.get("palette"); pal && !std::holds_alternative<std::nullptr_t>(pal->v))
                {
                    e.hasPalette = true;
                    // a palette is identified by the SHA-1 of its bytes (wherever they came from); the region (file/offset/length) is informative
                    if (pal->isObject() && pal->get("sha1") && !pal->get("offset"))
                    {
                        e.palette.file = upper(getStr(pal->get("file"), file));
                        e.palette.sha1 = lower(getStr(pal->get("sha1")));
                        getU64(pal->get("length"), e.palette.length);
                    }
                    else if (!parseRegion(pal, file, e.palette, why))
                        why = "palette: " + why;
                }
            uint64_t v = 0;
            if (why.empty())
            {
                const Json *orig = t.get("original");
                const Json *rep = t.get("replacement");
                if (!orig || !rep)
                    why = "original/replacement missing";
                else
                {
                    uint64_t w = 0, h = 0, rw = 0, rh = 0, psm = 0;
                    if (!getU64(orig->get("width"), w) || !getU64(orig->get("height"), h) || !getU64(orig->get("psm"), psm) || !w || !h)
                        why = "original width/height/psm missing";
                    else if (!getU64(rep->get("width"), rw) || !getU64(rep->get("height"), rh) || !rw || !rh)
                        why = "replacement width/height missing";
                    else
                    {
                        e.origWidth = static_cast<uint32_t>(w);
                        e.origHeight = static_cast<uint32_t>(h);
                        e.psm = static_cast<uint32_t>(psm);
                        e.width = static_cast<uint32_t>(rw);
                        e.height = static_cast<uint32_t>(rh);
                        if (rw % w || rh % h)
                            why = "replacement size is not an integer multiple of the original (" + std::to_string(rw) + "x" + std::to_string(rh) + " vs " + std::to_string(w) + "x" + std::to_string(h) + ")";
                        else if (rw > 16384 || rh > 16384)
                            why = "replacement too large";
                        else
                        {
                            e.scaleX = static_cast<uint32_t>(rw / w);
                            e.scaleY = static_cast<uint32_t>(rh / h);
                        }
                    }
                    const std::string path = getStr(rep->get("path"));
                    if (why.empty() && path.empty())
                        why = "replacement path missing";
                    if (why.empty())
                    {
                        std::filesystem::path pp = std::filesystem::path(baseDir) / std::filesystem::u8path(path);
                        e.imagePath = pp.lexically_normal().string();
                        // paths must stay inside the pack
                        const std::string rel = std::filesystem::relative(pp.lexically_normal(), std::filesystem::path(baseDir).lexically_normal()).string();
                        if (rel.empty() || rel.rfind("..", 0) == 0)
                            why = "replacement path leaves the pack folder";
                    }
                    e.mode = lower(getStr(rep->get("mode"), "rgba"));
                    if (why.empty() && e.mode != "rgba" && e.mode != "indexed")
                        why = "mode '" + e.mode + "' is not supported (rgba or indexed)";
                    e.indexed = e.mode == "indexed";
                    // a palette entry in rgba mode is matched by the content of the palette: its SHA-1 is required
                    if (why.empty() && !e.indexed && e.hasPalette && e.palette.sha1.empty())
                        why = "rgba entry with a palette needs palette.sha1";
                    if (why.empty() && e.indexed && e.psm != 0x13 && e.psm != 0x14)
                        why = "indexed mode needs original.psm 19 (PSMT8) or 20 (PSMT4)";
                    const std::string filter = lower(getStr(rep->get("filter"), "linear"));
                    e.linear = filter != "nearest";
                }
            }
            (void)v;
            if (!why.empty())
            {
                ++skippedCount;
                std::fprintf(stderr, "[hdpack] entry %s skipped: %s\n", e.id.c_str(), why.c_str());
                continue;
            }
            out.push_back(std::move(e));
        }
        if (skipped)
            *skipped = skippedCount;
        return true;
    }

    bool load(const std::string &dir, std::string &error)
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.loaded = false;
        s.entries.clear();
        s.byPixels.clear();
        s.byPixelsSha.clear();
        s.lru.clear();
        s.lruBytes = 0;
        s.indexImages.clear();
        s.indexBytes = 0;
        s.failedImages.clear();
        // `dir` is either one pack (a folder with manifest.json) or a folder of packs (every sub folder with a manifest.json, loaded in name order: when
        // two packs replace the same texture the later one wins, so "00-base", "10-ui-fixes" work as a load order).
        namespace fs = std::filesystem;
        std::vector<fs::path> packs;
        std::error_code ec;
        const fs::path base = fs::path(dir);
        if (fs::exists(base / "manifest.json", ec))
            packs.push_back(base);
        else if (fs::is_directory(base, ec))
        {
            std::vector<fs::path> subs;
            for (const auto &it : fs::directory_iterator(base, ec))
                if (it.is_directory(ec) && fs::exists(it.path() / "manifest.json", ec))
                    subs.push_back(it.path());
            std::sort(subs.begin(), subs.end());
            packs = std::move(subs);
        }
        if (packs.empty())
        {
            error = "no manifest.json in " + dir + " (nor in its sub folders)";
            return false;
        }
        s.stats = Stats{};
        uint64_t key = 1;
        std::string firstError;
        for (const fs::path &pack : packs)
        {
            std::ifstream in(pack / "manifest.json", std::ios::binary);
            std::stringstream buffer;
            buffer << in.rdbuf();
            std::vector<Entry> parsed;
            uint64_t skipped = 0;
            std::string packError;
            if (!in || !parseManifest(buffer.str(), pack.string(), parsed, packError, &skipped))
            {
                std::fprintf(stderr, "[hdpack] pack %s ignored: %s\n", pack.string().c_str(), packError.empty() ? "cannot read manifest.json" : packError.c_str());
                if (firstError.empty())
                    firstError = packError;
                continue;
            }
            for (Entry &e : parsed)
            {
                auto entry = std::make_unique<Entry>(std::move(e));
                entry->key = key++;
                s.byPixels.emplace(regionKey(entry->pixels), entry.get());
                if (!entry->pixels.sha1.empty())
                    s.byPixelsSha.emplace(entry->pixels.sha1, entry.get());
                s.entries.push_back(std::move(entry));
            }
            s.stats.manifestEntries += parsed.size() + skipped;
            s.stats.entriesSkipped += skipped;
            std::fprintf(stderr, "[hdpack] pack %s: %llu entries (%llu skipped)\n", pack.string().c_str(), static_cast<unsigned long long>(parsed.size()),
                         static_cast<unsigned long long>(skipped));
        }
        s.loaded = !s.entries.empty();
        s.enabled.store(s.loaded);
        if (!s.loaded)
            error = firstError.empty() ? "no pack has usable entries" : firstError;
        return s.loaded;
    }

    bool loaded() { return st().loaded; }
    bool enabled() { return st().loaded && st().enabled.load(); }
    void setEnabled(bool on) { st().enabled.store(on); }

    bool knowsPixels(const Region &pixels)
    {
        State &s = st();
        if (!s.loaded)
            return false;
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.byPixels.count(regionKey(pixels)) != 0;
    }

    const Entry *find(const Region &pixels, const std::string *paletteSha1, bool requirePixelsSha)
    {
        State &s = st();
        if (!s.loaded)
            return nullptr;
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto range = s.byPixels.equal_range(regionKey(pixels));
        const Entry *match = nullptr;
        for (auto it = range.first; it != range.second; ++it) // equal keys keep insertion order: the last matching entry (the latest pack) wins
        {
            const Entry &e = *it->second;
            if (s.failedImages.count(&e))
                continue;
            if (e.pixels.sha1.empty() ? requirePixelsSha : e.pixels.sha1 != pixels.sha1)
                continue;
            if (e.indexed)
            {
                match = &e; // the live palette is applied at run time: any palette fits
                continue;
            }
            if (e.hasPalette != (paletteSha1 != nullptr))
                continue;
            if (paletteSha1 && e.palette.sha1 != *paletteSha1)
                continue;
            match = &e;
        }
        return match;
    }

    const Entry *findByContent(const std::string &pixelsSha1, uint64_t length, uint32_t width, uint32_t height, uint32_t psm, const std::string *paletteSha1, bool *differs)
    {
        State &s = st();
        if (differs)
            *differs = false;
        if (!s.loaded || pixelsSha1.empty())
            return nullptr;
        std::lock_guard<std::mutex> lock(s.mutex);
        const auto range = s.byPixelsSha.equal_range(pixelsSha1);
        const Entry *match = nullptr;
        for (auto it = range.first; it != range.second; ++it) // insertion order: the latest pack wins
        {
            const Entry &e = *it->second;
            if (s.failedImages.count(&e) || e.pixels.length != length || e.origWidth != width || e.origHeight != height || e.psm != psm)
                continue;
            if (!e.indexed)
            {
                if (e.hasPalette != (paletteSha1 != nullptr))
                    continue;
                if (paletteSha1 && e.palette.sha1 != *paletteSha1)
                    continue;
            }
            if (match && differs && !sameImage(*match, e))
                *differs = true;
            match = &e;
        }
        return match;
    }

    bool sameImage(const Entry &a, const Entry &b)
    {
        if (&a == &b || a.imagePath == b.imagePath)
            return true;
        if (a.indexed != b.indexed || a.width != b.width || a.height != b.height || a.mode != b.mode || a.linear != b.linear)
            return false;
        static std::mutex cacheMutex;
        static std::unordered_map<std::string, uint64_t> cache; // file path -> hash of the file bytes (0 = unreadable)
        const auto fileHash = [](const std::string &path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return uint64_t(0);
            uint64_t h = 1469598103934665603ull;
            char buf[65536];
            while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
                for (std::streamsize i = 0; i < in.gcount(); ++i)
                    h = (h ^ static_cast<uint8_t>(buf[i])) * 1099511628211ull;
            return h ? h : uint64_t(1);
        };
        std::lock_guard<std::mutex> lock(cacheMutex);
        uint64_t h[2];
        const std::string *paths[2] = {&a.imagePath, &b.imagePath};
        for (int i = 0; i < 2; ++i)
        {
            auto it = cache.find(*paths[i]);
            if (it == cache.end())
                it = cache.emplace(*paths[i], fileHash(*paths[i])).first;
            h[i] = it->second;
        }
        return h[0] != 0 && h[0] == h[1];
    }

    namespace
    {
        // Looks a decoded variant (entry, palette hash) up in the LRU; moves it to the front.
        const uint8_t *lruFind(State &s, const Entry *entry, uint64_t variant)
        {
            for (auto it = s.lru.begin(); it != s.lru.end(); ++it)
                if (it->entry == entry && it->variant == variant)
                {
                    s.lru.splice(s.lru.begin(), s.lru, it);
                    return s.lru.front().data.data();
                }
            return nullptr;
        }

        const uint8_t *lruInsert(State &s, const Entry *entry, uint64_t variant, std::vector<uint8_t> &&data)
        {
            s.stats.imagesDecoded++;
            s.stats.decodedBytes += data.size();
            s.lruBytes += data.size();
            s.lru.push_front({entry, variant, std::move(data)});
            while (s.lruBytes > kDecodedKeep && s.lru.size() > 1)
            {
                s.lruBytes -= s.lru.back().data.size();
                s.lru.pop_back();
            }
            return s.lru.front().data.data();
        }
    } // namespace

    const uint8_t *pixels(const Entry &entry)
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (entry.indexed)
            return nullptr; // needs a palette: pixelsIndexed
        if (const uint8_t *hit = lruFind(s, &entry, 0))
            return hit;
        if (s.failedImages.count(&entry))
            return nullptr;
        Image img = LoadImage(entry.imagePath.c_str());
        if (!img.data)
        {
            std::fprintf(stderr, "[hdpack] %s: cannot read %s\n", entry.id.c_str(), entry.imagePath.c_str());
            s.failedImages[&entry] = true;
            return nullptr;
        }
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (static_cast<uint32_t>(img.width) != entry.width || static_cast<uint32_t>(img.height) != entry.height)
        {
            std::fprintf(stderr, "[hdpack] %s: %s is %dx%d, the manifest says %ux%u (original texture kept)\n", entry.id.c_str(), entry.imagePath.c_str(), img.width,
                         img.height, entry.width, entry.height);
            UnloadImage(img);
            s.failedImages[&entry] = true;
            return nullptr;
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(entry.width) * entry.height * 4u);
        std::memcpy(rgba.data(), img.data, rgba.size());
        UnloadImage(img);
        // PNG alpha 255 = opaque; the GS works with 0x80 = opaque
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = static_cast<uint8_t>((static_cast<unsigned>(rgba[i]) * 128u + 127u) / 255u);
        return lruInsert(s, &entry, 0, std::move(rgba));
    }

    const uint8_t *pixelsIndexed(const Entry &entry, const uint8_t *paletteCt32, uint32_t paletteEntries, uint64_t paletteHash)
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!entry.indexed || !paletteCt32 || !paletteEntries)
            return nullptr;
        const uint64_t variant = paletteHash | 1ull; // never 0 (0 = the plain RGBA variant)
        if (const uint8_t *hit = lruFind(s, &entry, variant))
            return hit;
        if (s.failedImages.count(&entry))
            return nullptr;
        auto idx = s.indexImages.find(&entry);
        if (idx == s.indexImages.end())
        {
            Image img = LoadImage(entry.imagePath.c_str());
            if (!img.data)
            {
                std::fprintf(stderr, "[hdpack] %s: cannot read %s\n", entry.id.c_str(), entry.imagePath.c_str());
                s.failedImages[&entry] = true;
                return nullptr;
            }
            const bool gray = img.format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;
            const bool grayAlpha = img.format == PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
            if ((!gray && !grayAlpha) || static_cast<uint32_t>(img.width) != entry.width || static_cast<uint32_t>(img.height) != entry.height)
            {
                std::fprintf(stderr, "[hdpack] %s: %s must be an 8-bit grayscale PNG of %ux%u whose value is the palette index (it is %dx%d, format %d); original texture kept\n",
                             entry.id.c_str(), entry.imagePath.c_str(), entry.width, entry.height, img.width, img.height, img.format);
                UnloadImage(img);
                s.failedImages[&entry] = true;
                return nullptr;
            }
            std::vector<uint8_t> indices(static_cast<size_t>(entry.width) * entry.height);
            const uint8_t *src = static_cast<const uint8_t *>(img.data);
            for (size_t i = 0; i < indices.size(); ++i)
                indices[i] = src[i * (gray ? 1 : 2)];
            UnloadImage(img);
            if (s.indexBytes + indices.size() > kDecodedKeep)
            {
                s.indexImages.clear();
                s.indexBytes = 0;
            }
            s.indexBytes += indices.size();
            idx = s.indexImages.emplace(&entry, std::move(indices)).first;
        }
        const bool fourBit = entry.psm == 0x14;
        std::vector<uint8_t> rgba(idx->second.size() * 4u);
        for (size_t i = 0; i < idx->second.size(); ++i)
        {
            uint32_t index = idx->second[i];
            if (fourBit)
                index &= 15u;
            if (index >= paletteEntries)
                index = 0;
            std::memcpy(&rgba[i * 4], paletteCt32 + static_cast<size_t>(index) * 4u, 4u); // R, G, B, A (alpha already in GS range)
        }
        return lruInsert(s, &entry, variant, std::move(rgba));
    }

    Stats stats()
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.stats;
    }
}
