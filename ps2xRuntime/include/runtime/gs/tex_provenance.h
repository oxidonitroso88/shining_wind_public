#pragma once

// Texture provenance tracking (PS2X_TEXMAP=<folder>): follows a resource from the disc, through the guest's decompression and copies, to the texture a
// GS draw samples (and the palette it uses), so replacement packs (HD textures, translations) can be keyed by "file + entry" instead of by hashes.
// Design and manifest contract: docs/hd-texture-identity-contract-2026-10-16.md. Everything is off (no cost, no behaviour change) unless PS2X_TEXMAP is set.
//
// Links of the chain (each one is recorded where the data is already known):
//   disc read   IOP CDVDFSV read: sectors -> file + offset, written to EE address X          (ps2x/iop/disc_observer.h)
//   decompress  the game's LZSS routine 0x221FF0(dst, src): RAM range <- stream at file offset  (installHooks)
//   DMA upload  GIF chain payloads: which RAM range fed every IMAGE tag (before the arbiter copies the packet)
//   GS upload   BITBLTBUF/TRXPOS/TRXREG + IMAGE data -> VRAM region (the "ledger"); fingerprints tie IMAGE chunks to their RAM origin
//   draw        TEX0 (tbp0, cbp, ...) -> ledger entry of the pixels and of the palette
// An identity is only reported as unique when every link resolved without conflict and the bytes still match what was registered; otherwise it is
// flagged ambiguous and a replacement system must keep the original texture.

#include "runtime/gs/gs_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class PS2Runtime;

namespace hdpack
{
    struct Entry;
}

namespace texprov
{
    // The pack folder in use as an absolute path ("" when none): PS2X_HD_PACK, or the default `hd_packs` of the working directory.
    std::string hdPackEffectivePath();
    bool enabled();            // tracking is on: PS2X_TEXMAP (writes the report) and/or PS2X_HD_PACK (identities only, nothing is written)
    bool replacementActive();  // an HD pack is loaded and enabled (see hd_pack.h)

    // The texture the GS is about to sample, in the terms of TEX0 (the GPU backend fills it from its descriptor).
    struct ReplacementQuery
    {
        uint32_t tbp0, tbw, psm, tw, th; // pixels
        uint32_t cbp, cpsm, csm, csa;    // palette (only used for indexed formats)
    };
    // What the GPU backend needs to bind a replacement.
    struct ReplacementResult
    {
        const hdpack::Entry *entry = nullptr;
        uint64_t key = 0;               // identifies the image (the entry, plus the live palette for indexed entries)
        std::vector<uint8_t> palette;   // indexed entries: the live palette (16 or 256 CT32 words indexed by the texture's palette index)
        uint32_t paletteEntries = 0;
        uint64_t paletteHash = 0;
    };
    // Finds the pack entry that replaces this texture. Only succeeds when every check of the contract holds (same upload geometry, pixels of a known resource
    // region -- or of several resources holding identical bytes whose pack entries agree --, palette matching the manifest for rgba entries, a readable live
    // palette for indexed ones); otherwise the original texture must be used. `why` (optional) receives the outcome.
    bool findReplacement(const ReplacementQuery &query, ReplacementResult &out, const char **why = nullptr);
    // Drops the memoised decisions (they are otherwise valid until the ledger changes).
    void forgetReplacementDecisions();
    // Counters of findReplacement outcomes ("applied", "ambiguous identity", ...), one line each, for logs.
    std::vector<std::pair<std::string, uint64_t>> replacementReport();

    // Called once with the guest RAM (applyShiningWind): installs the disc read observer and the LZSS hook.
    void install(PS2Runtime &runtime, uint8_t *rdram, size_t rdramSize);

    struct ChainSpan
    {
        uint32_t chainOffset; // offset inside the flattened chain buffer
        uint32_t srcAddr;     // EE address the DMA read it from
        uint32_t bytes;
    };
    // PS2Memory: the flattened GIF chain and where each part came from (key = chain buffer address, valid until submitted).
    void registerChainSpans(const uint8_t *chainData, std::vector<ChainSpan> spans);
    // PS2Memory::submitGifPacket, before the packet is copied by the GIF arbiter.
    void noteGifPacket(const uint8_t *data, uint32_t sizeBytes);

    // GS frontend (called with the GS state mutex held).
    void onTransferBegin(const GSBitBltBuf &bitbltbuf, const GSTrxPos &trxpos, const GSTrxReg &trxreg, uint32_t direction);
    void onImageChunk(const uint8_t *data, uint32_t sizeBytes);
    void onNativeUpload(const GSBitBltBuf &bitbltbuf, const GSTrxPos &trxpos, const GSTrxReg &trxreg, uint32_t direction, const uint8_t *data, uint32_t sizeBytes);
    void onDraw(const GSDrawState &state);
}
