#pragma once

// HD texture packs (PS2X_HD_PACK=<folder>): manifest + PNG replacements keyed by the stable texture identity of tex_provenance.h
// (logical file + resource region), not by VRAM addresses or content hashes alone. Contract: docs/hd-texture-identity-contract-2026-10-16.md.
// This module only knows the manifest and the images; deciding which texture the GS is sampling is texprov::findReplacement, and substituting the
// image is the GPU backend (docs/hd-stage3-parallel-gs-scope-2026-10-16.md). Without a pack nothing here runs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hdpack
{
    // A byte region of a game resource, as the contract defines it: `file` (+ `stream_offset` of the LZSS block when compressed) + `offset`/`length`
    // inside the decompressed stream (or inside the file when not compressed). `sha1` checks the bytes the GS actually received.
    struct Region
    {
        std::string file;               // logical path, upper case ("ST/TITLE.RAX")
        bool compressed = false;        // true: the region is inside the LZSS block at streamOffset
        uint64_t streamOffset = 0;
        uint64_t offset = 0;
        uint64_t length = 0;
        std::string sha1;               // lower case hex, may be empty (no check)
    };

    struct Entry
    {
        std::string id;
        Region pixels;
        bool hasPalette = false;
        Region palette;
        uint32_t origWidth = 0, origHeight = 0, psm = 0; // the rectangle the GS receives
        std::string imagePath;          // absolute path of the PNG
        uint32_t width = 0, height = 0; // declared PNG size
        uint32_t scaleX = 0, scaleY = 0; // width / origWidth, height / origHeight (integers)
        bool linear = true;
        std::string mode = "rgba";
        bool indexed = false;           // mode == "indexed"
        uint64_t key = 0;               // stable identifier of the image (entry index based), used by the GPU cache
    };

    struct Stats
    {
        uint64_t manifestEntries = 0;
        uint64_t entriesSkipped = 0;    // manifest entries that cannot be used (unsupported mode, bad dimensions, missing PNG)
        uint64_t imagesDecoded = 0;
        uint64_t decodedBytes = 0;
    };

    // Loads `<dir>/manifest.json`. Returns false (with `error`) when there is no usable pack: the game then runs with its original textures.
    bool load(const std::string &dir, std::string &error);
    bool loaded();

    // Runtime switch (hot key / API): replacements apply only while enabled. Starts enabled when a pack is loaded.
    bool enabled();
    void setEnabled(bool on);

    // Cheap pre-check: does any entry use exactly this pixel region (file, stream offset, offset, length), whatever its hashes?
    bool knowsPixels(const Region &pixels);

    // The entry for exactly this pixel region. An rgba entry with a palette also needs the SHA-1 of the palette bytes the texture is drawn with (palettes
    // are matched by content, wherever they came from); an indexed entry accepts any palette. `requirePixelsSha`: the entry must carry pixels.sha1 (buffers
    // the game modified after loading are matched by the content that was uploaded). Null when the pack has none.
    const Entry *find(const Region &pixels, const std::string *paletteSha1, bool requirePixelsSha = false);

    // Fallback for an upload whose source region cannot be established (the game wrote a file over a buffer that held something else, so the bytes come from
    // more than one origin): the entry whose pixels.sha1 is exactly `pixelsSha1`, made for the same rectangle (`length` bytes, width x height, psm), with the same
    // palette rules as find(). The latest pack wins, as in find(). Never used when the region is known. `differs` (optional) is set when another matching entry
    // draws a different picture (sameImage): the caller should then refuse.
    const Entry *findByContent(const std::string &pixelsSha1, uint64_t length, uint32_t width, uint32_t height, uint32_t psm, const std::string *paletteSha1,
                                 bool *differs = nullptr);

    // True when both entries draw the same picture: the same file, or two files with identical bytes (a pack may repeat one image under several names, one per
    // location of the same resource) and the same mode, size and filter.
    bool sameImage(const Entry &a, const Entry &b);

    // Decoded RGBA8 of the entry's PNG with alpha already in GS range (0x80 = opaque), width * height * 4 bytes, top-left origin. Null when the image is
    // missing or its size differs from the manifest (the entry is then disabled). The pointer is valid until the next call.
    const uint8_t *pixels(const Entry &entry);

    // Indexed entries: the entry's PNG is an 8-bit grayscale index map (value = palette index of the original texture); this composes the RGBA image with the
    // live palette: `paletteEntries` (16 or 256) CT32 words indexed by the texture's palette index, i.e. already resolved through the CSM1 layout and CSA
    // (R,G,B,A bytes with the GS alpha range). `paletteHash` identifies the palette contents (cache key).
    const uint8_t *pixelsIndexed(const Entry &entry, const uint8_t *paletteCt32, uint32_t paletteEntries, uint64_t paletteHash);

    Stats stats();

    // Parser exposed for tests: manifest text -> entries (paths resolved against `baseDir`).
    bool parseManifest(const std::string &text, const std::string &baseDir, std::vector<Entry> &out, std::string &error, uint64_t *skipped = nullptr);
}
