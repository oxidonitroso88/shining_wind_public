// HD texture pack manifest and matching (runtime/gs/hd_pack.h): the contract of docs/hd-texture-identity-contract-2026-10-16.md.
#include "MiniTest.h"
#include "runtime/gs/hd_pack.h"
#include "runtime/gs/gs_block_span.h"

#include <raylib.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    const char *kTitleEntry = R"({
      "id": "ST/TITLE.RAX@0xafcc+0x10:pixels",
      "file": "ST/TITLE.RAX",
      "pixels": {"stream_offset": 45004, "offset": 16, "length": 8064, "sha1": "6101"},
      "palette": {"stream_offset": 45004, "offset": 8080, "length": 1024, "sha1": "edb9"},
      "original": {"width": 336, "height": 24, "psm": 19},
      "replacement": {"path": "textures/a.png", "width": 672, "height": 48, "mode": "rgba", "filter": "linear"}
    })";

    std::string manifest(const std::string &entries)
    {
        return std::string(R"({"format":"ps2x-hd-pack","version":1,"textures":[)") + entries + "]}";
    }
}

void register_ps2_hd_pack_tests()
{
    MiniTest::Case("HdPack", [](TestCase &tc)
    {
        tc.Run("a valid entry is parsed with its identity, regions and scale", [](TestCase &t)
        {
            std::vector<hdpack::Entry> entries;
            std::string error;
            uint64_t skipped = 99;
            t.IsTrue(hdpack::parseManifest(manifest(kTitleEntry), "packdir", entries, error, &skipped), "manifest parses: " + error);
            t.Equals(entries.size(), size_t(1), "one entry");
            t.Equals(skipped, uint64_t(0), "none skipped");
            const hdpack::Entry &e = entries[0];
            t.Equals(e.pixels.file, std::string("ST/TITLE.RAX"), "file");
            t.IsTrue(e.pixels.compressed, "the pixels are inside an LZSS block");
            t.Equals(e.pixels.streamOffset, uint64_t(45004), "stream offset");
            t.Equals(e.pixels.offset, uint64_t(16), "offset");
            t.Equals(e.pixels.length, uint64_t(8064), "length");
            t.IsTrue(e.hasPalette && e.palette.file == "ST/TITLE.RAX" && e.palette.offset == 8080, "palette region defaults to the entry's file");
            t.Equals(e.scaleX, 2u, "integer horizontal scale");
            t.Equals(e.scaleY, 2u, "integer vertical scale");
            t.IsTrue(e.linear, "linear filtering");
        });

        tc.Run("an uncompressed palette (file bytes) has no stream offset; entry level stream_offset applies to the pixels", [](TestCase &t)
        {
            const std::string entry = R"({"file":"st/chara/player/006a_ev1.bra","stream_offset":135684,
              "pixels":{"offset":16,"length":2880},"palette":{"offset":24,"length":1024,"sha1":"ab"},
              "original":{"width":40,"height":72,"psm":19},"replacement":{"path":"t/x.png","width":160,"height":288,"filter":"nearest"}})";
            std::vector<hdpack::Entry> entries;
            std::string error;
            t.IsTrue(hdpack::parseManifest(manifest(entry), "packdir", entries, error), "parses");
            t.Equals(entries.size(), size_t(1), "one entry");
            t.Equals(entries[0].pixels.file, std::string("ST/CHARA/PLAYER/006A_EV1.BRA"), "file names are normalised to upper case");
            t.IsTrue(entries[0].pixels.compressed && entries[0].pixels.streamOffset == 135684, "entry level stream_offset reaches the pixels");
            t.IsFalse(entries[0].palette.compressed, "the palette is read straight from the file");
            t.Equals(entries[0].scaleX, 4u, "x4");
            t.IsFalse(entries[0].linear, "nearest filter requested");
        });

        tc.Run("unusable entries are skipped, never applied", [](TestCase &t)
        {
            const std::string bad[] = {
                R"({"file":"ST/A.RAX","pixels":{"offset":0,"length":4},"original":{"width":4,"height":4,"psm":19},"replacement":{"path":"a.png","width":6,"height":8}})", // 1.5x
                R"({"file":"ST/A.RAX","pixels":{"offset":0,"length":4},"original":{"width":4,"height":4,"psm":19},"replacement":{"path":"../evil.png","width":8,"height":8}})", // leaves the pack
                R"({"file":"ST/A.RAX","pixels":{"offset":0,"length":4},"original":{"width":4,"height":4,"psm":19},"replacement":{"path":"a.png","width":8,"height":8,"mode":"cmyk"}})", // unknown mode
                R"({"file":"ST/A.RAX","pixels":{"offset":0},"original":{"width":4,"height":4,"psm":19},"replacement":{"path":"a.png","width":8,"height":8}})", // no length
                R"({"pixels":{"offset":0,"length":4},"original":{"width":4,"height":4,"psm":19},"replacement":{"path":"a.png","width":8,"height":8}})", // no file
            };
            for (const std::string &entry : bad)
            {
                std::vector<hdpack::Entry> entries;
                std::string error;
                uint64_t skipped = 0;
                t.IsTrue(hdpack::parseManifest(manifest(entry), "packdir", entries, error, &skipped), "the manifest itself parses");
                t.Equals(entries.size(), size_t(0), "bad entry is not usable: " + entry.substr(0, 80));
                t.Equals(skipped, uint64_t(1), "and is counted as skipped");
            }
            std::vector<hdpack::Entry> entries;
            std::string error;
            t.IsFalse(hdpack::parseManifest(R"({"format":"other","version":1,"textures":[]})", "p", entries, error), "foreign format rejected");
            t.IsFalse(hdpack::parseManifest(R"({"format":"ps2x-hd-pack","version":2,"textures":[]})", "p", entries, error), "unknown version rejected");
            t.IsFalse(hdpack::parseManifest("{not json", "p", entries, error), "broken json rejected");
        });

        tc.Run("find() needs the exact region (and palette) and the same hashes; pixels() converts alpha and checks the size", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "ps2x_hd_pack_test";
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir / "textures", ec);
            // 672x48 RGBA test image: opaque white left half, transparent right half
            Image img = GenImageColor(672, 48, Color{255, 255, 255, 255});
            ImageDrawRectangle(&img, 336, 0, 336, 48, Color{0, 0, 0, 0});
            ExportImage(img, (dir / "textures" / "a.png").string().c_str());
            UnloadImage(img);
            {
                std::ofstream out(dir / "manifest.json");
                out << manifest(kTitleEntry);
            }
            std::string error;
            t.IsTrue(hdpack::load(dir.string(), error), "pack loads: " + error);
            t.IsTrue(hdpack::loaded() && hdpack::enabled(), "loaded and enabled");

            hdpack::Region px;
            px.file = "ST/TITLE.RAX";
            px.compressed = true;
            px.streamOffset = 45004;
            px.offset = 16;
            px.length = 8064;
            px.sha1 = "6101";
            const std::string pal = "edb9";
            const hdpack::Entry *e = hdpack::find(px, &pal);
            t.IsTrue(e != nullptr, "exact identity found");
            t.IsTrue(hdpack::find(px, nullptr) == nullptr, "an indexed entry needs its palette: without one the original is used");
            const std::string wrongPal = "0000";
            t.IsTrue(hdpack::find(px, &wrongPal) == nullptr, "a palette that changed (different bytes) is not replaced");
            hdpack::Region wrongPx = px;
            wrongPx.sha1 = "ffff";
            t.IsTrue(hdpack::find(wrongPx, &pal) == nullptr, "different pixel bytes: not replaced");
            hdpack::Region otherStream = px;
            otherStream.streamOffset = 45005;
            t.IsTrue(hdpack::find(otherStream, &pal) == nullptr, "another stream: not replaced");
            hdpack::Region uncompressed = px;
            uncompressed.compressed = false;
            t.IsTrue(hdpack::find(uncompressed, &pal) == nullptr, "compressed and plain regions are different identities");

            const uint8_t *rgba = e ? hdpack::pixels(*e) : nullptr;
            t.IsTrue(rgba != nullptr, "image decoded");
            if (rgba)
            {
                t.Equals(int(rgba[3]), 128, "opaque PNG alpha becomes the GS opaque value 0x80");
                t.Equals(int(rgba[(336 * 4) + 3]), 0, "transparent stays 0");
                t.Equals(int(rgba[0]), 255, "colour untouched");
            }

            hdpack::setEnabled(false);
            t.IsFalse(hdpack::enabled(), "can be switched off at run time");
            hdpack::setEnabled(true);

            // an image whose size differs from the manifest disables its entry
            Image small = GenImageColor(100, 20, Color{1, 2, 3, 255});
            ExportImage(small, (dir / "textures" / "a.png").string().c_str());
            UnloadImage(small);
            std::string error2;
            t.IsTrue(hdpack::load(dir.string(), error2), "reload");
            const hdpack::Entry *again = hdpack::find(px, &pal);
            t.IsTrue(again != nullptr && hdpack::pixels(*again) == nullptr, "wrong PNG size: no pixels (original kept)");
            t.IsTrue(hdpack::find(px, &pal) == nullptr, "...and the entry stops matching");
            fs::remove_all(dir, ec);
        });

        tc.Run("indexed entries: grayscale index map + live palette -> RGBA; palettes are matched by content", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path dir = fs::temp_directory_path() / "ps2x_hd_pack_test_indexed";
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir / "textures", ec);
            // 4x2 index map (a 2x1 texture at x2): indices 3 and 7
            Image gray = GenImageColor(4, 2, Color{3, 3, 3, 255});
            ImageFormat(&gray, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
            static_cast<uint8_t *>(gray.data)[2] = 7;
            static_cast<uint8_t *>(gray.data)[3] = 7;
            static_cast<uint8_t *>(gray.data)[6] = 7;
            static_cast<uint8_t *>(gray.data)[7] = 7;
            ExportImage(gray, (dir / "textures" / "i.png").string().c_str());
            UnloadImage(gray);
            Image color = GenImageColor(4, 2, Color{9, 9, 9, 255});
            ExportImage(color, (dir / "textures" / "c.png").string().c_str());
            UnloadImage(color);
            const std::string indexedEntry = R"({"file":"ST/A.RAX","pixels":{"stream_offset":100,"offset":16,"length":1,"sha1":"aa"},
              "original":{"width":2,"height":1,"psm":20},"replacement":{"path":"textures/i.png","width":4,"height":2,"mode":"indexed"}})";
            // an rgba entry whose palette is given by content only (no region)
            const std::string rgbaEntry = R"({"file":"ST/B.RAX","pixels":{"offset":0,"length":1},"palette":{"sha1":"BEEF","length":64},
              "original":{"width":2,"height":1,"psm":20},"replacement":{"path":"textures/c.png","width":4,"height":2}})";
            {
                std::ofstream out(dir / "manifest.json");
                out << manifest(indexedEntry + "," + rgbaEntry);
            }
            std::string error;
            t.IsTrue(hdpack::load(dir.string(), error), "pack loads: " + error);
            hdpack::Region px;
            px.file = "ST/A.RAX";
            px.compressed = true;
            px.streamOffset = 100;
            px.offset = 16;
            px.length = 1;
            px.sha1 = "aa";
            const std::string anyPalette = "whatever";
            const hdpack::Entry *e = hdpack::find(px, &anyPalette);
            t.IsTrue(e != nullptr && e->indexed, "an indexed entry accepts any palette");
            t.IsTrue(e && hdpack::find(px, nullptr) == e, "...even none at lookup time");
            t.IsTrue(e && hdpack::pixels(*e) == nullptr, "an indexed entry has no palette-free image");
            // 16 entry palette: entry 3 = opaque red, entry 7 = half transparent blue (GS alpha range 0..0x80)
            std::vector<uint8_t> palette(16u * 4u, 0u);
            palette[3 * 4 + 0] = 255; palette[3 * 4 + 3] = 0x80;
            palette[7 * 4 + 2] = 255; palette[7 * 4 + 3] = 0x40;
            const uint8_t *rgba = e ? hdpack::pixelsIndexed(*e, palette.data(), 16u, 111u) : nullptr;
            t.IsTrue(rgba != nullptr, "composed with the live palette");
            if (rgba)
            {
                t.Equals(int(rgba[0]), 255, "texel 0 is palette entry 3 (red)");
                t.Equals(int(rgba[3]), 0x80, "its GS alpha is kept as is");
                t.Equals(int(rgba[2 * 4 + 2]), 255, "texel 2 is palette entry 7 (blue)");
                t.Equals(int(rgba[2 * 4 + 3]), 0x40, "alpha of a translucent palette entry kept");
            }
            // the palette changes (fade / recolour): the same index map gives other colours
            palette[3 * 4 + 0] = 10; palette[3 * 4 + 1] = 200;
            const uint8_t *recoloured = e ? hdpack::pixelsIndexed(*e, palette.data(), 16u, 222u) : nullptr;
            t.IsTrue(recoloured != nullptr && recoloured[0] == 10 && recoloured[1] == 200, "a new palette produces a new image from the same index map");
            // a palette-less / out of range request is refused
            t.IsTrue(e && hdpack::pixelsIndexed(*e, nullptr, 16u, 1u) == nullptr, "no palette, no image");

            hdpack::Region pb;
            pb.file = "ST/B.RAX";
            pb.offset = 0;
            pb.length = 1;
            const std::string beef = "beef"; // manifest sha1s are compared case-insensitively
            t.IsTrue(hdpack::find(pb, &beef) != nullptr, "rgba entry with a palette given by content matches that palette from any origin");
            const std::string other = "dead";
            t.IsTrue(hdpack::find(pb, &other) == nullptr, "...and no other");
            fs::remove_all(dir, ec);
        });

        tc.Run("a folder of packs loads in name order (the last pack wins) and modified buffers need the content hash", [](TestCase &t)
        {
            namespace fs = std::filesystem;
            const fs::path root = fs::temp_directory_path() / "ps2x_hd_pack_test_multi";
            std::error_code ec;
            fs::remove_all(root, ec);
            auto writePack = [&](const char *name, int w, const std::string &entry) {
                const fs::path dir = root / name;
                fs::create_directories(dir / "textures", ec);
                Image img = GenImageColor(w, 2, Color{200, 100, 50, 255});
                ExportImage(img, (dir / "textures" / "a.png").string().c_str());
                UnloadImage(img);
                std::ofstream out(dir / "manifest.json");
                out << manifest(entry);
            };
            const std::string withSha = R"({"file":"ST/F.RAX","pixels":{"offset":0,"length":2,"sha1":"cafe"},"original":{"width":2,"height":1,"psm":19},"replacement":{"path":"textures/a.png","width":4,"height":2,"mode":"rgba"}})";
            const std::string noSha = R"({"file":"ST/F.RAX","pixels":{"offset":0,"length":2},"original":{"width":2,"height":1,"psm":0},"replacement":{"path":"textures/a.png","width":4,"height":2,"mode":"rgba"}})";
            writePack("10-late", 4, withSha);
            writePack("00-early", 6 /* size mismatch: would be disabled if it won */, withSha);
            writePack("broken", 4, "{not an entry}");
            {
                std::ofstream bad(root / "broken" / "manifest.json");
                bad << "{garbage";
            }
            std::string error;
            t.IsTrue(hdpack::load(root.string(), error), "a folder of packs loads: " + error);
            hdpack::Region px;
            px.file = "ST/F.RAX";
            px.offset = 0;
            px.length = 2;
            px.sha1 = "cafe";
            const hdpack::Entry *e = hdpack::find(px, nullptr);
            t.IsTrue(e != nullptr && e->width == 4, "the later pack (10-late) wins over the earlier one for the same identity");
            hdpack::Region other = px;
            other.sha1 = "beef";
            t.IsTrue(hdpack::find(other, nullptr) == nullptr, "a different uploaded content does not match");
            const hdpack::Entry *byContent = hdpack::findByContent("cafe", 2, 2, 1, 19, nullptr);
            t.IsTrue(byContent != nullptr && byContent->width == 4, "content fallback: the same bytes and rectangle find the entry of the latest pack");
            t.IsTrue(hdpack::findByContent("beef", 2, 2, 1, 19, nullptr) == nullptr, "content fallback: another hash does not match");
            t.IsTrue(hdpack::findByContent("cafe", 3, 2, 1, 19, nullptr) == nullptr && hdpack::findByContent("cafe", 2, 4, 1, 19, nullptr) == nullptr &&
                         hdpack::findByContent("cafe", 2, 2, 1, 20, nullptr) == nullptr,
                     "content fallback: a different length, width or format does not match");
            const std::string somePalette = "abcd";
            t.IsTrue(hdpack::findByContent("cafe", 2, 2, 1, 19, &somePalette) == nullptr, "content fallback: an rgba entry without palette does not match a texture that has one");
            fs::remove_all(root, ec);
            // the same bytes under two entries: identical image files (a copy under another name) are one picture, a different image is a conflict
            auto writeColorPack = [&](const char *name, Color c) {
                const fs::path dir = root / name;
                fs::create_directories(dir / "textures", ec);
                Image img = GenImageColor(4, 2, c);
                ExportImage(img, (dir / "textures" / "a.png").string().c_str());
                UnloadImage(img);
                std::ofstream out(dir / "manifest.json");
                out << manifest(withSha);
            };
            writeColorPack("10-a", Color{200, 100, 50, 255});
            writeColorPack("20-copy", Color{200, 100, 50, 255});
            t.IsTrue(hdpack::load(root.string(), error), "reload with two identical images");
            bool differs = true;
            t.IsTrue(hdpack::findByContent("cafe", 2, 2, 1, 19, nullptr, &differs) != nullptr && !differs, "content fallback: copies of the same image do not differ");
            writeColorPack("30-other", Color{10, 20, 30, 255});
            t.IsTrue(hdpack::load(root.string(), error), "reload with a different image");
            const hdpack::Entry *last = hdpack::findByContent("cafe", 2, 2, 1, 19, nullptr, &differs);
            t.IsTrue(last != nullptr && differs, "content fallback: a different image for the same bytes is reported");
            fs::remove_all(root, ec);
            writePack("only", 4, noSha);
            t.IsTrue(hdpack::load(root.string(), error), "reload");
            t.IsTrue(hdpack::find(px, nullptr, false) != nullptr, "an entry without sha1 matches a plain buffer");
            t.IsTrue(hdpack::find(px, nullptr, true) == nullptr, "...but never a buffer the game modified after loading (content hash mandatory)");
            fs::remove_all(root, ec);
        });
    });

    MiniTest::Case("TexProvenanceBlockSpan", [](TestCase &tc)
    {
        tc.Run("a palette uploaded in the same page as its texture does not overlap the texture (menu title headers)", [](TestCase &t)
        {
            uint32_t tl = 0, th = 0, pl = 0, ph = 0;
            texprov::blockSpan(13856, 2, 0x13, 0, 0, 112, 18, tl, th); // PSMT8 pixels of STATUS_TITLE, 112x18 uploaded
            texprov::blockSpan(13884, 1, 0x00, 0, 0, 16, 16, pl, ph);  // its 16x16 CT32 palette, 28 blocks further in the same page
            t.IsTrue(tl == 13856 && th == 13879, "the texture covers blocks 13856..13878 (got " + std::to_string(tl) + ".." + std::to_string(th) + ")");
            t.IsTrue(pl == 13884 && ph == 13888, "the palette covers blocks 13884..13887 (got " + std::to_string(pl) + ".." + std::to_string(ph) + ")");
            t.IsTrue(th <= pl, "the two ranges do not overlap, so uploading the palette keeps the texture tracked");
        });
        tc.Run("regions that really share blocks still overlap", [](TestCase &t)
        {
            uint32_t al = 0, ah = 0, bl = 0, bh = 0;
            texprov::blockSpan(100, 1, 0x00, 0, 0, 8, 8, al, ah);
            texprov::blockSpan(100, 1, 0x00, 0, 0, 16, 8, bl, bh);
            t.IsTrue(al == 100 && ah == 101 && bl == 100 && bh == 102, "one block and two blocks");
            t.IsTrue(bl < ah && al < bh, "overlap test used by the ledger");
        });
        tc.Run("whole buffers and pages", [](TestCase &t)
        {
            uint32_t lo = 0, hi = 0;
            texprov::blockSpan(0, 10, 0x00, 0, 0, 640, 448, lo, hi); // 640x448 CT32: 10 x 14 pages of 32 blocks
            t.IsTrue(lo == 0 && hi == 4480, "640x448 CT32 frame buffer = 4480 blocks");
            texprov::blockSpan(64, 1, 0x32, 0, 0, 64, 64, lo, hi); // one Z16 page
            t.IsTrue(lo == 64 && hi == 96, "one 64x64 Z16 page = 32 blocks");
            texprov::blockSpan(0, 16, 0x14, 0, 0, 1024, 1024, lo, hi); // too big to walk block by block: whole pages
            t.IsTrue(lo == 0 && hi == 64 * 32, "1024x1024 PSMT4 = 8 x 8 pages");
            texprov::blockSpan(0, 2, 0x13, 64, 32, 16, 16, lo, hi); // PSMT8 block (4, 2) of page 0 is block number 24
            t.IsTrue(lo == 24 && hi == 25, "a one block sub rectangle (got " + std::to_string(lo) + ".." + std::to_string(hi) + ")");
        });
        tc.Run("an unknown storage mode falls back to a conservative whole page range", [](TestCase &t)
        {
            uint32_t lo = 0, hi = 0;
            texprov::blockSpan(10, 1, 0x7F, 0, 0, 8, 8, lo, hi);
            t.IsTrue(lo == 10 && hi >= 11, "never smaller than the data");
        });
    });
}
