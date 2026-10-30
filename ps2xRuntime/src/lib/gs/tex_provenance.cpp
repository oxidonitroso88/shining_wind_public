// Texture provenance tracking (PS2X_TEXMAP=<folder>). See include/runtime/gs/tex_provenance.h and docs/hd-texture-identity-contract-2026-10-16.md.
#include "runtime/gs/tex_provenance.h"
#include "runtime/gs/hd_pack.h"
#include "runtime/gs/gs_block_span.h"

#include "ps2_runtime.h"
#include "ps2x/iop/disc_observer.h"

#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace texprov
{
    namespace
    {
        constexpr uint32_t kLzssEntry = 0x00221FF0u; // the game's LZSS decompressor: a0 = destination, a1 = source stream ("LZSS" header)
        constexpr uint32_t kLzssMagic = 0x53535A4Cu;
        constexpr uint32_t kBlockHash = 256u;

        // Cost accounting of the tracking hooks (PS2X_HD_STATS=1 prints it): where the time of the provenance bookkeeping goes.
        enum ProfSlot { PfGifPacket, PfLookupRange, PfContent, PfFingerprint, PfFinalize, PfLzss, PfDiscRead, PfDraw, PfFind, PfImageChunk, PfCount };
        const char *const kProfNames[PfCount] = {"noteGifPacket", "  lookupRange", "  findByContent", "  fingerprint", "finalizeTransfer", "noteLzss", "noteDiscRead", "onDraw", "findReplacement", "onImageChunk"};
        struct ProfCounter
        {
            std::atomic<uint64_t> ns{0}, calls{0};
        } g_prof[PfCount];
        const bool g_profOn = std::getenv("PS2X_HD_STATS") != nullptr;
        struct ProfScope
        {
            int slot;
            std::chrono::steady_clock::time_point t0;
            explicit ProfScope(int s) : slot(s)
            {
                if (g_profOn)
                    t0 = std::chrono::steady_clock::now();
            }
            ~ProfScope()
            {
                if (!g_profOn)
                    return;
                g_prof[slot].ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                ++g_prof[slot].calls;
            }
        };

        // 64 bit content hash (8 bytes per step: the fingerprints and segment checks run over every uploaded byte). Not persisted anywhere.
        uint64_t fnv64(const uint8_t *p, size_t n)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) * 0xFF51AFD7ED558CCDull);
            size_t i = 0;
            for (; i + 8 <= n; i += 8)
            {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0x9FB21C651E98DF25ull;
                h ^= h >> 29;
            }
            uint64_t tail = 0;
            if (i < n)
            {
                std::memcpy(&tail, p + i, n - i);
                h = (h ^ tail) * 0x9FB21C651E98DF25ull;
            }
            h ^= h >> 32;
            h *= 0xD6E8FEB86659FD93ull;
            return h ^ (h >> 32);
        }

        // Minimal SHA-1 (hashes in the manifests are SHA-1 so Python's hashlib can check them).
        std::string sha1Hex(const uint8_t *data, size_t len)
        {
            uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
            auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
            std::vector<uint8_t> msg(data, data + len);
            msg.push_back(0x80u);
            while (msg.size() % 64 != 56)
                msg.push_back(0u);
            const uint64_t bits = static_cast<uint64_t>(len) * 8u;
            for (int i = 7; i >= 0; --i)
                msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));
            for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
            {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = (uint32_t(msg[chunk + i * 4]) << 24) | (uint32_t(msg[chunk + i * 4 + 1]) << 16) | (uint32_t(msg[chunk + i * 4 + 2]) << 8) | uint32_t(msg[chunk + i * 4 + 3]);
                for (int i = 16; i < 80; ++i)
                    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i)
                {
                    uint32_t f, k;
                    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
                    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                    else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                    const uint32_t t = rol(a, 5) + f + e + k + w[i];
                    e = d; d = c; c = rol(b, 30); b = a; a = t;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
            }
            char out[41];
            for (int i = 0; i < 5; ++i)
                std::snprintf(out + i * 8, 9, "%08x", h[i]);
            return std::string(out, 40);
        }

        std::string jsonEscape(const std::string &s)
        {
            std::string out;
            for (const char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    out.push_back('\\');
                    out.push_back(c);
                }
                else if (static_cast<unsigned char>(c) < 0x20u)
                    out.push_back('?');
                else
                    out.push_back(c);
            }
            return out;
        }

        // ---- origins and the RAM -> origin map ------------------------------------------------------------------------------------------------
        struct Origin
        {
            enum class Kind { File, Lzss } kind = Kind::File;
            std::string path;            // File: the file; Lzss: the file holding the compressed stream ("" when unknown)
            uint64_t streamOffset = 0;   // Lzss: offset of the "LZSS" header inside `path`
            uint32_t compressed = 0, raw = 0;
            std::string rawSha1;         // Lzss: SHA-1 of the whole decompressed stream
            int id = 0;
            // copy of the bytes (file span / decompressed stream) for content based re-attribution: bytes [base, base + data.size()) in origin coordinates
            std::shared_ptr<std::vector<uint8_t>> data;
            uint64_t base = 0;
            std::vector<uint64_t> anchors; // hashes this origin put in the content index (so they can be removed when it is evicted)
        };
        using OriginPtr = std::shared_ptr<Origin>;

        struct Seg
        {
            uint32_t end = 0;
            OriginPtr origin;
            uint64_t off = 0; // offset inside the origin's coordinates of the first byte of this segment
            std::vector<uint64_t> blockHashes;
        };

        struct Piece
        {
            OriginPtr origin; // null = nothing is known about these bytes
            uint64_t off = 0;
            uint32_t bytes = 0;
            bool verified = false;
            bool byContent = false; // re-attributed by content (the address based tag was stale)
        };

        struct ChunkInfo
        {
            std::vector<Piece> pieces;
            bool ambiguous = false;
            uint32_t bytes = 0;
            std::vector<std::vector<Piece>> alternates; // other origins that hold exactly the same bytes (when ambiguous), at most 6
        };

        struct ChunkRes
        {
            bool found = false;
            ChunkInfo info;
        };

        struct Ledger
        {
            uint32_t id = 0;
            uint32_t dbp = 0, dbw = 0, dpsm = 0, x = 0, y = 0, w = 0, h = 0;
            uint32_t blockLo = 0, blockHi = 0;
            std::vector<uint8_t> data;
            mutable std::string sha1Cache; // SHA-1 of `data`, computed on demand (see ledgerSha)
            std::vector<ChunkRes> chunks;
            unsigned long long vblank = 0;
        };

        // SHA-1 of an upload, computed on demand. The same bytes are uploaded again and again (sprite frames cycling through the same VRAM slots), so the SHA-1 is
        // remembered by a fast content hash of the bytes.
        const std::string &ledgerSha(const Ledger &e)
        {
            if (e.sha1Cache.empty())
            {
                static std::unordered_map<uint64_t, std::string> byContent;
                const uint64_t key = fnv64(e.data.data(), e.data.size()) ^ (static_cast<uint64_t>(e.data.size()) << 40);
                const auto it = byContent.find(key);
                if (it != byContent.end())
                    e.sha1Cache = it->second;
                else
                {
                    e.sha1Cache = sha1Hex(e.data.data(), e.data.size());
                    if (byContent.size() > 30000)
                        byContent.clear();
                    byContent[key] = e.sha1Cache;
                }
            }
            return e.sha1Cache;
        }

        struct Xfer
        {
            bool active = false;
            GSBitBltBuf bb{};
            GSTrxPos pos{};
            GSTrxReg reg{};
            uint64_t expected = 0;
            std::vector<uint8_t> data;
            std::vector<ChunkRes> chunks;
        };

        struct State
        {
            std::mutex mutex;
            bool enabled = false;
            bool writeOutput = false; // PS2X_TEXMAP: write the report / PNG / decompression files (PS2X_HD_PACK alone only tracks identities)
            bool trace = false;
            std::map<std::string, uint64_t> replacementCounters;
            // Version of the ledger entries based at a block address: bumped whenever an entry with that base is recorded or removed. A replacement decision
            // only depends on the entries at the texture base and at the palette base, so it stays valid while those two versions do not change.
            std::unordered_map<uint32_t, uint64_t> dbpVersion;
            uint64_t versionOf(uint32_t dbp) const
            {
                const auto it = dbpVersion.find(dbp);
                return it == dbpVersion.end() ? 0u : it->second;
            }
            struct ReplacementMemo
            {
                uint32_t tbp0 = 0, cbp = 0;
                uint64_t tbpVersion = 0, cbpVersion = 0;
                bool found = false;
                const char *reason = "";
                ReplacementResult result;
            };
            std::unordered_map<uint64_t, ReplacementMemo> replacementMemo;
            std::filesystem::path dir;
            std::ofstream texturesOut, eventsOut;
            uint8_t *rdram = nullptr;
            size_t rdramSize = 0;
            std::map<uint32_t, Seg> segs; // key = start (EE physical address)
            std::unordered_map<const uint8_t *, std::vector<ChainSpan>> chainSpans;
            std::unordered_map<uint64_t, ChunkInfo> fingerprints;
            std::vector<Ledger> ledger;
            Xfer xfer;
            uint32_t nextOrigin = 1, nextLedger = 1, nextUsage = 1;
            std::set<std::string> usageKeys;
            std::set<std::string> savedDecomp;
            std::unordered_map<int, OriginPtr> originsById; // origins whose bytes are kept (FIFO, bounded)
            std::deque<int> originOrder;
            uint64_t keptBytes = 0;
            std::unordered_multimap<uint64_t, std::pair<int, uint32_t>> contentIndex; // hash of 64 bytes at 16 byte steps -> (origin id, offset in origin coordinates)
            uint64_t lastFrameKey = ~0ull;
            uint32_t uploads = 0, draws = 0;
        };

        State &st()
        {
            static State s;
            return s;
        }

        PS2Runtime::RecompiledFunction g_originalLzss = nullptr;

        uint32_t phys(uint32_t addr) { return addr & 0x1FFFFFFFu; }

        // ---- segment map --------------------------------------------------------------------------------------------------------------------
        void insertSeg(State &s, uint32_t start, uint32_t length, const OriginPtr &origin, uint64_t off)
        {
            if (!length || start >= s.rdramSize || start + static_cast<uint64_t>(length) > s.rdramSize)
                return;
            const uint32_t end = start + length;
            // trim / split everything that overlaps [start, end)
            auto it = s.segs.lower_bound(start);
            if (it != s.segs.begin())
                --it;
            while (it != s.segs.end() && it->first < end)
            {
                Seg old = it->second;
                const uint32_t oldStart = it->first;
                if (old.end <= start)
                {
                    ++it;
                    continue;
                }
                it = s.segs.erase(it);
                if (oldStart < start)
                {
                    Seg left = old;
                    left.end = start;
                    left.blockHashes.clear();
                    for (uint32_t a = oldStart; a < start; a += kBlockHash)
                        left.blockHashes.push_back(fnv64(s.rdram + a, std::min(kBlockHash, start - a)));
                    s.segs[oldStart] = std::move(left);
                }
                if (old.end > end)
                {
                    Seg right = old;
                    right.off = old.off + (end - oldStart);
                    right.blockHashes.clear();
                    for (uint32_t a = end; a < old.end; a += kBlockHash)
                        right.blockHashes.push_back(fnv64(s.rdram + a, std::min(kBlockHash, old.end - a)));
                    s.segs[end] = std::move(right);
                }
            }
            Seg seg;
            seg.end = end;
            seg.origin = origin;
            seg.off = off;
            for (uint32_t a = start; a < end; a += kBlockHash)
                seg.blockHashes.push_back(fnv64(s.rdram + a, std::min(kBlockHash, end - a)));
            s.segs[start] = std::move(seg);
        }

        // Pieces covering [addr, addr + len); bytes nobody registered are returned with a null origin.
        std::vector<Piece> lookupRange(State &s, uint32_t addr, uint32_t len)
        {
            std::vector<Piece> out;
            if (!s.rdram || addr >= s.rdramSize)
                return out;
            len = static_cast<uint32_t>(std::min<uint64_t>(len, s.rdramSize - addr));
            uint32_t pos = addr;
            const uint32_t end = addr + len;
            while (pos < end)
            {
                auto it = s.segs.upper_bound(pos);
                const Seg *seg = nullptr;
                uint32_t segStart = 0;
                if (it != s.segs.begin())
                {
                    auto prev = std::prev(it);
                    if (prev->second.end > pos)
                    {
                        seg = &prev->second;
                        segStart = prev->first;
                    }
                }
                if (!seg)
                {
                    const uint32_t next = (it != s.segs.end()) ? std::min(end, it->first) : end;
                    Piece gap;
                    gap.bytes = next - pos;
                    out.push_back(gap);
                    pos = next;
                    continue;
                }
                const uint32_t to = std::min(end, seg->end);
                Piece piece;
                piece.origin = seg->origin;
                piece.off = seg->off + (pos - segStart);
                piece.bytes = to - pos;
                piece.verified = true;
                if (piece.origin && piece.origin->data && piece.off >= piece.origin->base &&
                    piece.off - piece.origin->base + piece.bytes <= piece.origin->data->size())
                {
                    // exact check against the kept copy of the source bytes
                    piece.verified = std::memcmp(s.rdram + pos, piece.origin->data->data() + (piece.off - piece.origin->base), piece.bytes) == 0;
                    out.push_back(piece);
                    pos = to;
                    continue;
                }
                // no kept copy: every 256 byte block that overlaps the piece must still hash as registered
                const uint32_t firstBlock = (pos - segStart) / kBlockHash, lastBlock = (to - 1 - segStart) / kBlockHash;
                for (uint32_t b = firstBlock; b <= lastBlock && b < seg->blockHashes.size(); ++b)
                {
                    const uint32_t blockStart = segStart + b * kBlockHash;
                    const uint32_t blockLen = std::min(kBlockHash, seg->end - blockStart);
                    if (fnv64(s.rdram + blockStart, blockLen) != seg->blockHashes[b])
                    {
                        piece.verified = false;
                        break;
                    }
                }
                out.push_back(piece);
                pos = to;
            }
            return out;
        }

        // Identity of a piece independent of which read/decompression produced the Origin object.
        std::string pieceSignature(const OriginPtr &origin, uint64_t off)
        {
            if (!origin)
                return "?";
            return std::string(origin->kind == Origin::Kind::File ? "F|" : "L|") + origin->path + "|" + std::to_string(origin->streamOffset) + "|" + std::to_string(off);
        }

        // Content based provenance: remember the bytes of the latest files / decompressed streams (bounded) and index them with content defined anchors
        // (a rolling hash over 16 bytes, one position in 32 on average), so data that the guest copied or moved can be traced back to its source whatever the
        // alignment of the copy.
        constexpr uint64_t kKeepLimit = 48ull << 20;
        constexpr uint32_t kWindow = 16u;
        constexpr uint64_t kPrime = 1099511628211ull;

        uint64_t primePower() // kPrime^(kWindow - 1)
        {
            static const uint64_t value = []() { uint64_t v = 1; for (uint32_t i = 1; i < kWindow; ++i) v *= kPrime; return v; }();
            return value;
        }

        bool isAnchor(uint64_t hash) { return ((hash * 0x9E3779B97F4A7C15ull) >> 59) == 0u; }

        // Calls fn(position, hash) for every anchor of bytes[0, length).
        template <typename Fn>
        void forEachAnchor(const uint8_t *bytes, size_t length, Fn &&fn)
        {
            if (length < kWindow)
                return;
            uint64_t h = 0;
            for (uint32_t i = 0; i < kWindow; ++i)
                h = h * kPrime + bytes[i];
            const uint64_t top = primePower();
            for (size_t k = 0;; ++k)
            {
                if (isAnchor(h))
                    if (!fn(k, h))
                        return;
                if (k + kWindow >= length)
                    break;
                h = (h - bytes[k] * top) * kPrime + bytes[k + kWindow];
            }
        }

        void indexOrigin(State &s, const OriginPtr &origin)
        {
            forEachAnchor(origin->data->data(), origin->data->size(), [&](size_t k, uint64_t h) {
                if (s.contentIndex.count(h) < 8u)
                {
                    s.contentIndex.emplace(h, std::make_pair(origin->id, static_cast<uint32_t>(origin->base + k)));
                    origin->anchors.push_back(h);
                }
                return true;
            });
        }

        void keepOrigin(State &s, const OriginPtr &origin, const uint8_t *bytes, size_t length, uint64_t base)
        {
            if (length < 64u || length > kKeepLimit)
                return;
            origin->data = std::make_shared<std::vector<uint8_t>>(bytes, bytes + length);
            origin->base = base;
            s.originsById[origin->id] = origin;
            s.originOrder.push_back(origin->id);
            s.keptBytes += length;
            indexOrigin(s, origin);
            while (s.keptBytes > kKeepLimit && !s.originOrder.empty())
            {
                const int victim = s.originOrder.front();
                s.originOrder.pop_front();
                auto it = s.originsById.find(victim);
                if (it != s.originsById.end())
                {
                    s.keptBytes -= it->second->data ? it->second->data->size() : 0u;
                    // take the victim's anchors out of the index (incrementally: rebuilding the whole index for every eviction is far too slow)
                    for (const uint64_t h : it->second->anchors)
                    {
                        const auto range = s.contentIndex.equal_range(h);
                        for (auto e = range.first; e != range.second; ++e)
                            if (e->second.first == victim)
                            {
                                s.contentIndex.erase(e);
                                break;
                            }
                    }
                    it->second->anchors.clear();
                    it->second->anchors.shrink_to_fit();
                    it->second->data.reset();
                    s.originsById.erase(it);
                }
            }
        }

        // Where do these exact bytes live in the kept origins? Returns every distinct (origin, offset) that matches completely.
        std::vector<std::pair<OriginPtr, uint64_t>> findByContent(State &s, const uint8_t *bytes, uint32_t length)
        {
            std::vector<std::pair<OriginPtr, uint64_t>> found;
            std::set<std::string> seen;
            if (length < 64u)
                return found;
            uint32_t tested = 0;
            forEachAnchor(bytes, length, [&](size_t k, uint64_t h) {
                const auto range = s.contentIndex.equal_range(h);
                for (auto it = range.first; it != range.second; ++it)
                {
                    const auto origin = s.originsById.find(it->second.first);
                    if (origin == s.originsById.end() || !origin->second->data || it->second.second < k)
                        continue;
                    const auto &data = *origin->second->data;
                    const uint64_t start = it->second.second - k; // where the piece would begin in origin coordinates
                    if (start < origin->second->base)
                        continue;
                    const uint64_t at = start - origin->second->base;
                    if (at + length <= data.size() && std::memcmp(data.data() + at, bytes, length) == 0)
                        if (seen.insert(pieceSignature(origin->second, start)).second)
                            found.emplace_back(origin->second, start);
                }
                return ++tested < 64u && seen.size() < 2u;
            });
            return found;
        }

        // ---- output ------------------------------------------------------------------------------------------------------------------------
        void ensureOutput(State &s)
        {
            if (!s.writeOutput || s.texturesOut.is_open())
                return;
            std::error_code ec;
            std::filesystem::create_directories(s.dir / "png", ec);
            std::filesystem::create_directories(s.dir / "decomp", ec);
            s.texturesOut.open(s.dir / "textures.jsonl", std::ios::out | std::ios::trunc);
            if (s.trace)
                s.eventsOut.open(s.dir / "events.jsonl", std::ios::out | std::ios::trunc);
        }

        void event(State &s, const std::string &line)
        {
            if (s.trace && s.eventsOut.is_open())
            {
                s.eventsOut << line << '\n';
                s.eventsOut.flush();
            }
        }

        std::string pieceJson(const Piece &p)
        {
            std::ostringstream o;
            if (!p.origin)
            {
                o << "{\"stage\":\"unknown\",\"bytes\":" << p.bytes << "}";
                return o.str();
            }
            if (p.origin->kind == Origin::Kind::File)
            {
                o << "{\"stage\":\"file\",\"file\":\"" << jsonEscape(p.origin->path) << "\",\"offset\":" << p.off << ",\"bytes\":" << p.bytes
                  << ",\"verified\":" << (p.verified ? "true" : "false") << (p.byContent ? ",\"by_content\":true" : "") << "}";
                return o.str();
            }
            o << "{\"stage\":\"lzss\",\"stream_file\":\"" << jsonEscape(p.origin->path) << "\",\"stream_offset\":" << p.origin->streamOffset
              << ",\"compressed\":" << p.origin->compressed << ",\"raw\":" << p.origin->raw << ",\"raw_sha1\":\"" << p.origin->rawSha1
              << "\",\"raw_offset\":" << p.off << ",\"bytes\":" << p.bytes << ",\"verified\":" << (p.verified ? "true" : "false") << (p.byContent ? ",\"by_content\":true" : "") << "}";
            return o.str();
        }

        std::string piecesJson(const std::vector<Piece> &pieces)
        {
            std::string out = "[";
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                if (i)
                    out += ",";
                out += pieceJson(pieces[i]);
            }
            return out + "]";
        }

        // ---- GS geometry helpers -----------------------------------------------------------------------------------------------------------
        uint32_t bitsPerPixel(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: case 0x30: return 32;
            case 0x01: case 0x31: return 24;
            case 0x02: case 0x0A: case 0x32: case 0x3A: return 16;
            case 0x13: return 8;
            case 0x14: case 0x1B: case 0x24: case 0x2C: return 4;
            default: return 32;
            }
        }

        void invalidate(State &s, uint32_t lo, uint32_t hi)
        {
            s.ledger.erase(std::remove_if(s.ledger.begin(), s.ledger.end(),
                                          [&](const Ledger &e) {
                                              const bool hit = e.blockLo < hi && lo < e.blockHi;
                                              if (hit)
                                                  ++s.dbpVersion[e.dbp];
                                              return hit;
                                          }),
                           s.ledger.end());
        }

        // ---- decoding to PNG ----------------------------------------------------------------------------------------------------------------
        uint32_t clutEntry(const Ledger &clut, uint32_t index, uint32_t cpsm, bool sixteen)
        {
            const uint32_t entries = static_cast<uint32_t>(clut.data.size() / (cpsm == 0 ? 4u : 2u));
            uint32_t pos = index;
            if (entries >= 256u)
                pos = (index & 0xE7u) | ((index & 0x10u) >> 1) | ((index & 0x08u) << 1); // CSM1 swizzle of 256 entry palettes
            (void)sixteen;
            if (pos >= entries)
                return 0u;
            if (cpsm == 0u || cpsm == 1u)
            {
                const uint8_t *p = clut.data.data() + pos * 4u;
                const uint32_t a = std::min<uint32_t>(255u, uint32_t(p[3]) * 255u / 128u);
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (a << 24);
            }
            const uint8_t *p = clut.data.data() + pos * 2u;
            const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
            const uint32_t r = (v & 31u) << 3, g = ((v >> 5) & 31u) << 3, b = ((v >> 10) & 31u) << 3, a = (v & 0x8000u) ? 255u : 0u;
            return r | (g << 8) | (b << 16) | (a << 24);
        }

        bool decodeToPng(const Ledger &tex, const Ledger *clut, uint32_t texPsm, uint32_t cpsm, uint32_t csa, const std::string &path)
        {
            const uint32_t w = tex.w, h = tex.h;
            if (!w || !h || w > 2048 || h > 2048)
                return false;
            std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u, 0u);
            const uint32_t bits = bitsPerPixel(tex.dpsm);
            if (texPsm != tex.dpsm)
                return false; // uploaded in another format (swizzle trick): the host byte order is not the sampled order
            if (bits == 32 || bits == 24 || bits == 16)
            {
                const uint32_t bytes = bits / 8;
                if (tex.data.size() < static_cast<size_t>(w) * h * bytes)
                    return false;
                for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                {
                    const uint8_t *p = tex.data.data() + i * bytes;
                    uint32_t c;
                    if (bits == 32)
                        c = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (std::min<uint32_t>(255u, uint32_t(p[3]) * 255u / 128u) << 24);
                    else if (bits == 24)
                        c = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | 0xFF000000u;
                    else
                    {
                        const uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
                        c = ((v & 31u) << 3) | (((v >> 5) & 31u) << 11) | (((v >> 10) & 31u) << 19) | ((v & 0x8000u) ? 0xFF000000u : 0u);
                    }
                    std::memcpy(&rgba[i * 4], &c, 4);
                }
            }
            else
            {
                if (!clut)
                    return false;
                const bool four = bits == 4;
                const size_t need = four ? (static_cast<size_t>(w) * h + 1) / 2 : static_cast<size_t>(w) * h;
                if (tex.data.size() < need)
                    return false;
                for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
                {
                    uint32_t index = four ? ((tex.data[i / 2] >> ((i & 1) ? 4 : 0)) & 0xFu) : tex.data[i];
                    if (four)
                        index += csa * 16u;
                    const uint32_t c = clutEntry(*clut, index, cpsm, false);
                    std::memcpy(&rgba[i * 4], &c, 4);
                }
            }
            Image image{};
            image.data = rgba.data();
            image.width = static_cast<int>(w);
            image.height = static_cast<int>(h);
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            return ExportImage(image, path.c_str());
        }

        // ---- identity of a ledger entry ---------------------------------------------------------------------------------------------------
        struct Identity
        {
            std::string status = "none"; // unique | equivalent | ambiguous | none
            std::string reason;
            std::vector<Piece> pieces;
            // equivalent: the very same bytes come from several resources (each candidate is one contiguous region, `pieces` is the first one)
            std::vector<std::vector<Piece>> candidates;
        };

        Identity identify(const Ledger &e)
        {
            Identity id;
            bool ambiguous = false, missing = false, stale = false;
            uint64_t covered = 0;
            for (const ChunkRes &c : e.chunks)
            {
                if (!c.found)
                {
                    missing = true;
                    continue;
                }
                if (c.info.ambiguous)
                    ambiguous = true;
                for (const Piece &p0 : c.info.pieces)
                {
                    // the GIF payload is rounded up to whole quadwords: bytes past the end of the transfer are padding that belongs to whatever follows in memory
                    if (covered >= e.data.size())
                        break;
                    Piece p = p0;
                    p.bytes = static_cast<uint32_t>(std::min<uint64_t>(p.bytes, e.data.size() - covered));
                    covered += p.bytes;
                    if (!p.origin)
                        missing = true;
                    else if (!p.verified)
                        stale = true;
                    id.pieces.push_back(p);
                }
            }
            if (id.pieces.empty())
            {
                id.status = "none";
                id.reason = "no origin recorded for the uploaded data";
                return id;
            }
            id.status = (ambiguous || missing || stale) ? "ambiguous" : "unique";
            if (stale && !ambiguous && !missing)
            {
                // The game rewrote part of the buffer after loading it (e.g. the font atlas of SYSTEM.RAX gets its kanji glyphs written in at load time). The upload
                // is still tied to one resource region and, being content addressed (SHA-1 of the uploaded bytes), a replacement made for exactly this content is safe.
                id.status = "modified";
                id.reason = "the buffer was modified after loading: identified by its source region and by the content actually uploaded";
                return id;
            }
            if (id.status == "ambiguous" && ambiguous && !missing && !stale && e.chunks.size() == 1u && e.chunks[0].found && !e.chunks[0].info.alternates.empty())
            {
                // identical bytes held by several resources: still a well defined image, the pack decides whether the candidates agree
                bool allVerified = true;
                for (const Piece &p : id.pieces)
                    allVerified = allVerified && p.origin && p.verified;
                if (allVerified)
                {
                    id.status = "equivalent";
                    id.candidates.push_back(id.pieces);
                    for (const auto &alt : e.chunks[0].info.alternates)
                        id.candidates.push_back(alt);
                    for (auto &candidate : id.candidates) // same padding trim as above
                    {
                        uint64_t used = 0;
                        for (Piece &p : candidate)
                        {
                            p.bytes = static_cast<uint32_t>(std::min<uint64_t>(p.bytes, e.data.size() > used ? e.data.size() - used : 0u));
                            used += p.bytes;
                        }
                        while (!candidate.empty() && candidate.back().bytes == 0u)
                            candidate.pop_back();
                    }
                    id.reason = "identical bytes come from several resources (equivalent origins)";
                    return id;
                }
            }
            if (ambiguous)
                id.reason = "identical bytes were uploaded from more than one origin";
            else if (missing)
                id.reason = "part of the uploaded bytes has no known origin";
            else if (stale)
                id.reason = "the source buffer changed after it was registered";
            return id;
        }

        // For an upload whose bytes no longer match their source (the game modified the buffer after loading it): which bytes differ from the source, as the rows
        // of the uploaded rectangle that contain changes. Diagnostic only (docs/hd-system-rax-2026-10-17.md).
        std::string diffJson(const Ledger &e, const Identity &id)
        {
            if (id.pieces.empty() || id.status == "unique" || id.status == "equivalent")
                return std::string();
            const uint64_t rowBytes = std::max<uint64_t>(1u, (static_cast<uint64_t>(e.w) * bitsPerPixel(e.dpsm) + 7u) / 8u);
            uint64_t pos = 0, changed = 0, runs = 0;
            bool inRun = false;
            std::vector<std::pair<uint64_t, uint64_t>> rowRuns; // [first row, last row] with changes
            uint64_t minCol = ~0ull, maxCol = 0;
            for (const Piece &p : id.pieces)
            {
                if (!p.origin || !p.origin->data || p.off < p.origin->base || p.off - p.origin->base + p.bytes > p.origin->data->size() || pos + p.bytes > e.data.size())
                    return std::string();
                const uint8_t *src = p.origin->data->data() + (p.off - p.origin->base);
                for (uint32_t i = 0; i < p.bytes; ++i)
                {
                    if (e.data[pos + i] != src[i])
                    {
                        ++changed;
                        if (!inRun)
                            ++runs;
                        inRun = true;
                        const uint64_t row = (pos + i) / rowBytes, col = (pos + i) % rowBytes;
                        minCol = std::min(minCol, col);
                        maxCol = std::max(maxCol, col);
                        if (rowRuns.empty() || row > rowRuns.back().second + 1)
                            rowRuns.emplace_back(row, row);
                        else
                            rowRuns.back().second = std::max(rowRuns.back().second, row);
                    }
                    else
                        inRun = false;
                }
                pos += p.bytes;
            }
            std::ostringstream o;
            o << ",\"diff\":{\"changed\":" << changed << ",\"total\":" << pos << ",\"runs\":" << runs << ",\"row_bytes\":" << rowBytes << ",\"cols\":[" << (changed ? minCol : 0)
              << "," << maxCol << "],\"row_runs\":[";
            for (size_t k = 0; k < rowRuns.size() && k < 48; ++k)
                o << (k ? "," : "") << "[" << rowRuns[k].first << "," << rowRuns[k].second << "]";
            o << "]}";
            return o.str();
        }

        std::string ledgerJson(const Ledger &e, const Identity &id)
        {
            std::ostringstream o;
            o << "{\"identity\":\"" << id.status << "\",\"reason\":\"" << id.reason << "\",\"pieces\":" << piecesJson(id.pieces) << ",\"candidates\":[";
            for (size_t k = 0; k < id.candidates.size(); ++k)
                o << (k ? "," : "") << piecesJson(id.candidates[k]);
            o << "],\"sha1\":\"" << ledgerSha(e)
              << "\",\"upload\":{\"dbp\":" << e.dbp << ",\"dpsm\":" << e.dpsm << ",\"dbw\":" << e.dbw << ",\"x\":" << e.x << ",\"y\":" << e.y << ",\"w\":" << e.w
              << ",\"h\":" << e.h << ",\"bytes\":" << e.data.size() << ",\"vblank\":" << e.vblank << "}" << diffJson(e, id) << "}";
            return o.str();
        }

        void finalizeTransfer(State &s)
        {
            ProfScope prof(PfFinalize);
            Xfer &x = s.xfer;
            x.active = false;
            Ledger e;
            e.id = s.nextLedger++;
            e.dbp = x.bb.dbp;
            e.dbw = x.bb.dbw;
            e.dpsm = x.bb.dpsm;
            e.x = x.pos.dsax;
            e.y = x.pos.dsay;
            e.w = x.reg.rrw;
            e.h = x.reg.rrh;
            blockSpan(e.dbp, e.dbw, e.dpsm, e.x, e.y, e.w, e.h, e.blockLo, e.blockHi);
            e.data = std::move(x.data);
            if (e.data.size() > x.expected)
                e.data.resize(static_cast<size_t>(x.expected));
            e.chunks = std::move(x.chunks);
            e.vblank = g_ps2xGsVblank.load(std::memory_order_relaxed);
            invalidate(s, e.blockLo, e.blockHi);
            if (const char *dumpSha = std::getenv("PS2X_TEXMAP_DUMPSHA"); dumpSha && ledgerSha(e).compare(0, std::strlen(dumpSha), dumpSha) == 0)
            {
                std::ofstream up(s.dir / ("sha_" + ledgerSha(e) + "_" + std::to_string(e.id) + ".bin"), std::ios::binary);
                up.write(reinterpret_cast<const char *>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
            }
            if (std::getenv("PS2X_TEXMAP_DUMPUP") && (e.w * e.h >= 4096u || (e.w == 16u && e.h == 16u)) && s.uploads < 4000u)
            {
                std::ofstream up(s.dir / ("upload_" + std::to_string(e.id) + "_" + std::to_string(e.dpsm) + "_" + std::to_string(e.w) + "x" + std::to_string(e.h) + ".bin"), std::ios::binary);
                up.write(reinterpret_cast<const char *>(e.data.data()), static_cast<std::streamsize>(e.data.size()));
            }
            ++s.uploads;
            if (s.trace)
            {
                const Identity id = identify(e);
                event(s, "{\"event\":\"gs-upload\",\"ledger\":" + std::to_string(e.id) + ",\"entry\":" + ledgerJson(e, id) + "}");
            }
            ++s.dbpVersion[e.dbp];
            s.ledger.push_back(std::move(e));
            if (s.ledger.size() > 4096)
            {
                for (auto it = s.ledger.begin(); it != s.ledger.begin() + 1024; ++it)
                    ++s.dbpVersion[it->dbp];
                s.ledger.erase(s.ledger.begin(), s.ledger.begin() + 1024);
            }
        }

        ChunkRes resolveChunk(State &s, const uint8_t *data, uint32_t size)
        {
            ChunkRes r;
            const auto it = s.fingerprints.find(fnv64(data, size));
            if (it != s.fingerprints.end() && it->second.bytes == size)
            {
                r.found = true;
                r.info = it->second;
            }
            return r;
        }

        void addImageData(State &s, const uint8_t *data, uint32_t size)
        {
            Xfer &x = s.xfer;
            if (!x.active)
                return;
            x.chunks.push_back(resolveChunk(s, data, size));
            x.data.insert(x.data.end(), data, data + size);
            if (x.data.size() >= x.expected)
                finalizeTransfer(s);
        }

        // ---- hooks --------------------------------------------------------------------------------------------------------------------------
        void noteLzss(uint8_t *rdram, uint32_t dst, uint32_t src)
        {
            State &s = st();
            std::lock_guard<std::mutex> lock(s.mutex);
            ProfScope prof(PfLzss);
            const uint32_t srcP = phys(src), dstP = phys(dst);
            if (srcP + 16u > s.rdramSize)
                return;
            uint32_t magic = 0, comp = 0, raw = 0;
            std::memcpy(&magic, rdram + srcP, 4);
            std::memcpy(&comp, rdram + srcP + 8, 4);
            std::memcpy(&raw, rdram + srcP + 12, 4);
            if (magic != kLzssMagic || raw == 0u || raw > (32u << 20) || dstP + static_cast<uint64_t>(raw) > s.rdramSize)
                return;
            auto origin = std::make_shared<Origin>();
            origin->kind = Origin::Kind::Lzss;
            origin->compressed = comp;
            origin->raw = raw;
            origin->id = static_cast<int>(s.nextOrigin++);
            const std::vector<Piece> parents = lookupRange(s, srcP, 16u);
            if (!parents.empty() && parents[0].origin && parents[0].origin->kind == Origin::Kind::File && parents[0].verified)
            {
                origin->path = parents[0].origin->path;
                origin->streamOffset = parents[0].off;
            }
            origin->rawSha1 = sha1Hex(rdram + dstP, raw);
            insertSeg(s, dstP, raw, origin, 0);
            keepOrigin(s, origin, rdram + dstP, raw, 0);
            ensureOutput(s);
            if (s.writeOutput && !origin->path.empty())
            {
                // reference decompression output for the editor's own LZSS decoder
                std::string name = origin->path;
                for (char &c : name)
                    if (c == '/' || c == '\\' || c == ':')
                        c = '_';
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), "@%llx.bin", static_cast<unsigned long long>(origin->streamOffset));
                const std::string file = name + suffix;
                if (s.savedDecomp.insert(file).second)
                {
                    std::ofstream out(s.dir / "decomp" / file, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(rdram + dstP), raw);
                }
            }
            std::ostringstream o;
            o << "{\"event\":\"lzss\",\"src\":" << src << ",\"dst\":" << dst << ",\"stream_file\":\"" << jsonEscape(origin->path) << "\",\"stream_offset\":"
              << origin->streamOffset << ",\"compressed\":" << comp << ",\"raw\":" << raw << ",\"raw_sha1\":\"" << origin->rawSha1 << "\"}";
            event(s, o.str());
        }

        void lzssWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t dst = getRegU32(ctx, 4), src = getRegU32(ctx, 5);
            if (g_originalLzss)
                g_originalLzss(rdram, ctx, runtime);
            noteLzss(rdram, dst, src);
        }

        void noteDiscRead(const ps2x::iop::DiscRead &read)
        {
            State &s = st();
            std::lock_guard<std::mutex> lock(s.mutex);
            ProfScope prof(PfDiscRead);
            ensureOutput(s);
            std::ostringstream o;
            o << "{\"event\":\"disc-read\",\"lsn\":" << read.lsn << ",\"sectors\":" << read.sectors << ",\"dest\":" << read.destination << ",\"spans\":[";
            bool first = true;
            for (const ps2x::iop::DiscSpan &span : read.spans)
            {
                auto origin = std::make_shared<Origin>();
                origin->kind = Origin::Kind::File;
                origin->path = span.path;
                origin->id = static_cast<int>(s.nextOrigin++);
                insertSeg(s, phys(read.destination) + span.bufferOffset, span.bytes, origin, span.fileOffset);
                if (phys(read.destination) + static_cast<uint64_t>(span.bufferOffset) + span.bytes <= s.rdramSize)
                    keepOrigin(s, origin, s.rdram + phys(read.destination) + span.bufferOffset, span.bytes, span.fileOffset);
                o << (first ? "" : ",") << "{\"file\":\"" << jsonEscape(span.path) << "\",\"offset\":" << span.fileOffset << ",\"bytes\":" << span.bytes << "}";
                first = false;
            }
            o << "]}";
            event(s, o.str());
        }

        // (width * height) pixels of `psm`, rounded up to whole quadwords as the GIF transfers them
        uint64_t transferBytes(uint32_t psm, uint32_t w, uint32_t h)
        {
            const uint64_t bits = static_cast<uint64_t>(w) * h * bitsPerPixel(psm);
            return (bits + 7u) / 8u;
        }
    } // namespace

    namespace
    {
        bool texmapRequested()
        {
            const char *v = std::getenv("PS2X_TEXMAP");
            return v != nullptr && v[0] != '\0';
        }

        // HD packs: PS2X_HD_PACK=<a pack folder, or a folder of packs>; "off" / "0" disables them. Without the variable the folder `hd_packs` next to the
        // working directory of the port is used when it exists, so installing a pack is just copying its folder there.
        std::string hdPackPath()
        {
            const char *v = std::getenv("PS2X_HD_PACK");
            if (v != nullptr)
                return (v[0] == '\0' || std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0) ? std::string() : std::string(v);
            std::error_code ec;
            return std::filesystem::is_directory("hd_packs", ec) ? std::string("hd_packs") : std::string();
        }

        bool hdPackRequested() { return !hdPackPath().empty(); }
    } // namespace

    bool enabled()
    {
        static const bool value = texmapRequested() || hdPackRequested();
        return value;
    }

    std::string hdPackEffectivePath()
    {
        const std::string path = hdPackPath();
        if (path.empty())
            return path;
        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        return ec ? path : absolute.lexically_normal().string();
    }

    bool replacementActive() { return hdpack::enabled(); }

    void install(PS2Runtime &runtime, uint8_t *rdram, size_t rdramSize)
    {
        if (!enabled())
            return;
        State &s = st();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            s.enabled = true;
            s.rdram = rdram;
            s.rdramSize = rdramSize;
            s.writeOutput = texmapRequested();
            if (s.writeOutput)
                s.dir = std::filesystem::path(std::getenv("PS2X_TEXMAP"));
            s.trace = s.writeOutput && std::getenv("PS2X_TEXMAP_TRACE") != nullptr;
            ensureOutput(s);
        }
        if (hdPackRequested())
        {
            std::string error;
            if (!hdpack::load(hdPackPath(), error))
                std::fprintf(stderr, "[hdpack] no pack in use: %s (original textures)\n", error.c_str());
            // PS2X_HD_PACK_STATE=off: start with the replacements switched off (what F10 does), used to hand a session the state of the main game
            else if (const char *state = std::getenv("PS2X_HD_PACK_STATE"); state != nullptr && std::strcmp(state, "off") == 0)
                hdpack::setEnabled(false);
        }
        ps2x::iop::setDiscReadObserver([](const ps2x::iop::DiscRead &read) { noteDiscRead(read); });
        g_originalLzss = runtime.lookupFunction(kLzssEntry);
        if (!g_originalLzss || !runtime.replaceFunction(kLzssEntry, &lzssWrapper))
            std::fprintf(stderr, "[texprov] could not hook the LZSS routine at 0x%X\n", kLzssEntry);
        if (s.writeOutput)
            std::fprintf(stderr, "[texprov] tracking textures into %s\n", s.dir.string().c_str());
    }

    void registerChainSpans(const uint8_t *chainData, std::vector<ChainSpan> spans)
    {
        if (!enabled())
            return;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.chainSpans.size() > 256)
            s.chainSpans.clear();
        s.chainSpans[chainData] = std::move(spans);
    }

    void noteGifPacket(const uint8_t *data, uint32_t size)
    {
        if (!enabled())
            return;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        ProfScope prof(PfGifPacket);
        if (!s.rdram)
            return;
        const std::vector<ChainSpan> *spans = nullptr;
        if (const auto it = s.chainSpans.find(data); it != s.chainSpans.end())
            spans = &it->second;
        const bool inRam = data >= s.rdram && data < s.rdram + s.rdramSize;
        auto srcRanges = [&](uint32_t offset, uint32_t len) {
            std::vector<std::pair<uint32_t, uint32_t>> ranges; // (EE address, bytes)
            if (spans)
            {
                for (const ChainSpan &span : *spans)
                {
                    const uint32_t from = std::max(offset, span.chainOffset), to = std::min(offset + len, span.chainOffset + span.bytes);
                    if (from < to)
                        ranges.emplace_back(phys(span.srcAddr) + (from - span.chainOffset), to - from);
                }
            }
            else if (inRam)
                ranges.emplace_back(static_cast<uint32_t>(data - s.rdram) + offset, len);
            return ranges;
        };
        uint32_t pos = 0;
        while (pos + 16u <= size)
        {
            uint64_t lo = 0;
            std::memcpy(&lo, data + pos, 8);
            pos += 16u;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            if (flg == 0u)
                pos += nloop * nreg * 16u;
            else if (flg == 1u)
                pos += ((nloop * nreg + 1u) / 2u) * 16u;
            else
            {
                const uint32_t payload = std::min<uint32_t>(nloop * 16u, size - std::min(size, pos));
                if (flg == 2u && payload >= 16u)
                {
                    ChunkInfo info;
                    info.bytes = payload;
                    {
                        ProfScope profLookup(PfLookupRange);
                        for (const auto &range : srcRanges(pos, payload))
                            for (const Piece &p : lookupRange(s, range.first, range.second))
                                info.pieces.push_back(p);
                    }
                    {
                        // pieces whose address based tag is missing or stale: look the bytes up by content in the kept files / streams
                        uint32_t cursor = 0;
                        for (Piece &p : info.pieces)
                        {
                            if ((!p.origin || !p.verified) && p.bytes >= 64u)
                            {
                                ProfScope profContent(PfContent);
                                const auto matches = findByContent(s, data + pos + cursor, p.bytes);
                                std::set<std::string> distinct;
                                for (const auto &m : matches)
                                    distinct.insert(pieceSignature(m.first, m.second));
                                if (distinct.size() == 1)
                                {
                                    p.origin = matches[0].first;
                                    p.off = matches[0].second;
                                    p.verified = true;
                                    p.byContent = true;
                                }
                                else if (distinct.size() > 1)
                                {
                                    info.ambiguous = true; // the same bytes exist in several sources
                                    if (info.pieces.size() == 1u && p.bytes == payload)
                                        for (const auto &m : matches)
                                        {
                                            Piece alt;
                                            alt.origin = m.first;
                                            alt.off = m.second;
                                            alt.bytes = p.bytes;
                                            alt.verified = true;
                                            alt.byContent = true;
                                            if (info.alternates.size() < 6u)
                                                info.alternates.push_back({alt});
                                        }
                                }
                            }
                            cursor += p.bytes;
                        }
                    }
                    if (s.trace)
                    {
                        std::ostringstream t;
                        t << "{\"event\":\"dma-image\",\"bytes\":" << payload << ",\"ranges\":[";
                        bool firstRange = true;
                        for (const auto &range : srcRanges(pos, payload))
                        {
                            t << (firstRange ? "" : ",") << "[" << range.first << "," << range.second << "]";
                            firstRange = false;
                        }
                        t << "],\"sha1\":\"" << sha1Hex(data + pos, payload) << "\",\"pieces\":" << piecesJson(info.pieces) << "}";
                        event(s, t.str());
                    }
                    ProfScope profFingerprint(PfFingerprint);
                    const uint64_t fp = fnv64(data + pos, payload);
                    auto found = s.fingerprints.find(fp);
                    if (found == s.fingerprints.end())
                    {
                        if (s.fingerprints.size() > 60000)
                            s.fingerprints.clear();
                        s.fingerprints[fp] = std::move(info);
                    }
                    else if (found->second.bytes == payload)
                    {
                        // identical bytes seen before: only the same origin keeps the identity unique
                        const auto &a = found->second.pieces;
                        bool same = a.size() == info.pieces.size();
                        for (size_t i = 0; same && i < a.size(); ++i)
                            same = pieceSignature(a[i].origin, a[i].off) == pieceSignature(info.pieces[i].origin, info.pieces[i].off);
                        if (!same)
                        {
                            found->second.ambiguous = true;
                            if (found->second.alternates.size() < 6u)
                            {
                                bool known = false;
                                for (const auto &alt : found->second.alternates)
                                    known = known || (alt.size() == info.pieces.size() && !alt.empty() &&
                                                      pieceSignature(alt[0].origin, alt[0].off) == pieceSignature(info.pieces[0].origin, info.pieces[0].off));
                                if (!known)
                                    found->second.alternates.push_back(info.pieces);
                            }
                        }
                    }
                }
                pos += nloop * 16u;
            }
        }
        // the chain buffer is consumed: forget its map entry
        if (spans)
            s.chainSpans.erase(data);
    }

    void onTransferBegin(const GSBitBltBuf &bitbltbuf, const GSTrxPos &trxpos, const GSTrxReg &trxreg, uint32_t direction)
    {
        if (!enabled())
            return;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.xfer = Xfer{};
        if (direction == 0u && trxreg.rrw && trxreg.rrh)
        {
            s.xfer.active = true;
            s.xfer.bb = bitbltbuf;
            s.xfer.pos = trxpos;
            s.xfer.reg = trxreg;
            s.xfer.expected = transferBytes(bitbltbuf.dpsm, trxreg.rrw, trxreg.rrh);
        }
        else if (direction == 2u)
        {
            uint32_t lo = 0, hi = 0;
            blockSpan(bitbltbuf.dbp, bitbltbuf.dbw, bitbltbuf.dpsm, trxpos.dsax, trxpos.dsay, std::max<uint32_t>(trxreg.rrw, 1u), std::max<uint32_t>(trxreg.rrh, 1u), lo, hi);
            invalidate(s, lo, hi);
        }
    }

    void onImageChunk(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!enabled() || !data || !sizeBytes)
            return;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        ProfScope prof(PfImageChunk);
        addImageData(s, data, sizeBytes);
    }

    void onNativeUpload(const GSBitBltBuf &bitbltbuf, const GSTrxPos &trxpos, const GSTrxReg &trxreg, uint32_t direction, const uint8_t *data, uint32_t sizeBytes)
    {
        if (!enabled())
            return;
        onTransferBegin(bitbltbuf, trxpos, trxreg, direction);
        onImageChunk(data, sizeBytes);
    }

    void onDraw(const GSDrawState &state)
    {
        if (!enabled())
            return;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        ProfScope prof(PfDraw);
        const GSContext &ctx = state.context;
        // pixels rendered into the frame / depth buffers overwrite whatever the ledger said lived there
        const uint64_t frameKey = (static_cast<uint64_t>(ctx.frame.fbp) << 40) ^ (static_cast<uint64_t>(ctx.frame.fbw) << 32) ^ (static_cast<uint64_t>(ctx.frame.psm) << 24) ^
                                  (static_cast<uint64_t>(ctx.scissor.y1) << 8) ^ static_cast<uint64_t>(ctx.zbuf.zbp);
        if (frameKey != s.lastFrameKey)
        {
            s.lastFrameKey = frameKey;
            uint32_t lo = 0, hi = 0;
            const uint32_t rows = std::min<uint32_t>(static_cast<uint32_t>(ctx.scissor.y1) + 1u, 1024u);
            blockSpan(ctx.frame.fbp * 32u, ctx.frame.fbw, ctx.frame.psm, 0u, 0u, std::max(ctx.frame.fbw, 1u) * 64u, rows, lo, hi);
            invalidate(s, lo, hi);
            if (!ctx.zbuf.zmask)
            {
                blockSpan(ctx.zbuf.zbp * 32u, ctx.frame.fbw, ctx.zbuf.psm, 0u, 0u, std::max(ctx.frame.fbw, 1u) * 64u, rows, lo, hi);
                invalidate(s, lo, hi);
            }
        }
        ++s.draws;
        if (!s.writeOutput || !state.prim.tme)
            return;
        const GSTex0Reg &t = ctx.tex0;
        const Ledger *pixels = nullptr, *clut = nullptr;
        for (auto it = s.ledger.rbegin(); it != s.ledger.rend(); ++it)
            if (it->blockLo <= t.tbp0 && t.tbp0 < it->blockHi)
            {
                pixels = &*it;
                break;
            }
        const bool indexed = t.psm == 0x13 || t.psm == 0x14;
        if (indexed)
            for (auto it = s.ledger.rbegin(); it != s.ledger.rend(); ++it)
                if (it->blockLo <= t.cbp && t.cbp < it->blockHi)
                {
                    clut = &*it;
                    break;
                }
        const Identity pid = pixels ? identify(*pixels) : Identity{};
        const Identity cid = clut ? identify(*clut) : Identity{};
        // One record per distinct texture: tracked identities are keyed by the content (SHA-1 of the uploaded pixels and palette), everything else
        // (dynamic or unresolved textures re-uploaded every frame) by where it lives and why it is not trackable.
        std::string key = pid.status == "unique" ? ("U|" + ledgerSha(*pixels) + "|" + (clut ? ledgerSha(*clut) : std::string()))
                                                 : ("N|" + pid.status + "|" + pid.reason + "|" + std::to_string(t.tbp0) + "|" + std::to_string(t.psm) + "|" + std::to_string(t.tw) + "|" + std::to_string(t.th));
        if (pid.status != "unique" && !pid.pieces.empty() && pid.pieces[0].origin)
            key += "|" + pid.pieces[0].origin->path + "@" + std::to_string(pid.pieces[0].origin->streamOffset) + "+" + std::to_string(pid.pieces[0].off);
        key += "|" + std::to_string(t.tbp0) + "|" + std::to_string(t.psm) + "|" + std::to_string(t.tw) + "|" + std::to_string(t.th) + "|" + std::to_string(t.cbp) + "|" + std::to_string(t.cpsm) + "|" + std::to_string(t.csa);
        if (!s.usageKeys.insert(key).second)
            return;
        const uint32_t usage = s.nextUsage++;
        ensureOutput(s);
        std::string pngName;
        if (pixels)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "png/%04u_%x.png", usage, t.tbp0);
            if (decodeToPng(*pixels, clut, t.psm, t.cpsm, t.csa, (s.dir / name).string()))
                pngName = name;
        }
        std::ostringstream o;
        o << "{\"usage\":" << usage << ",\"vblank\":" << g_ps2xGsVblank.load(std::memory_order_relaxed) << ",\"tex0\":{\"tbp0\":" << t.tbp0 << ",\"tbw\":" << unsigned(t.tbw)
          << ",\"psm\":" << unsigned(t.psm) << ",\"width\":" << (1u << t.tw) << ",\"height\":" << (1u << t.th) << ",\"tcc\":" << unsigned(t.tcc) << ",\"tfx\":" << unsigned(t.tfx)
          << ",\"cbp\":" << t.cbp << ",\"cpsm\":" << unsigned(t.cpsm) << ",\"csm\":" << unsigned(t.csm) << ",\"csa\":" << unsigned(t.csa) << "},\"fst\":" << (state.prim.fst ? 1 : 0)
          << ",\"pixels\":" << (pixels ? ledgerJson(*pixels, pid) : std::string("{\"identity\":\"none\",\"reason\":\"no tracked upload covers this texture (dynamic, or uploaded before tracking)\"}"))
          << ",\"palette\":" << (clut ? ledgerJson(*clut, cid) : std::string(indexed ? "{\"identity\":\"none\",\"reason\":\"no tracked palette upload\"}" : "null")) << ",\"png\":\""
          << pngName << "\"}";
        s.texturesOut << o.str() << '\n';
        s.texturesOut.flush();
        event(s, "{\"event\":\"draw\",\"usage\":" + std::to_string(usage) + "}");
    }

    // ---- HD replacement lookup -----------------------------------------------------------------------------------------------------------------
    namespace
    {
        // The contiguous resource region a list of pieces came from (contract: file + stream_offset + offset/length), or false when the bytes do not come
        // from a single known region. The SHA-1 is left empty: it is computed only for regions the pack knows (regionSha1).
        bool regionOfPieces(const std::vector<Piece> &pieces, hdpack::Region &out, bool allowModified = false)
        {
            if (pieces.empty() || !pieces[0].origin)
                return false;
            const Piece &first = pieces[0];
            const Origin &o = *first.origin;
            if (o.path.empty())
                return false;
            const std::string signature = pieceSignature(first.origin, first.off);
            const std::string prefix = signature.substr(0, signature.rfind('|'));
            uint64_t next = first.off, length = 0;
            for (const Piece &p : pieces)
            {
                if (!p.origin || (!p.verified && !allowModified))
                    return false;
                const std::string sig = pieceSignature(p.origin, p.off);
                if (sig.substr(0, sig.rfind('|')) != prefix || p.off != next)
                    return false;
                next += p.bytes;
                length += p.bytes;
            }
            out.file = o.path;
            out.compressed = o.kind == Origin::Kind::Lzss;
            out.streamOffset = out.compressed ? o.streamOffset : 0;
            out.offset = first.off;
            out.length = length;
            out.sha1.clear();
            return true;
        }

        std::string regionSha1(const Ledger &e, const hdpack::Region &r)
        {
            return r.length == e.data.size() ? ledgerSha(e) : sha1Hex(e.data.data(), static_cast<size_t>(std::min<uint64_t>(r.length, e.data.size())));
        }

        const Ledger *latestAt(const State &s, uint32_t dbp)
        {
            for (auto it = s.ledger.rbegin(); it != s.ledger.rend(); ++it)
                if (it->dbp == dbp)
                    return &*it;
            return nullptr;
        }

        const hdpack::Entry *refuse(const ReplacementQuery &q, const char *why, const char **out)
        {
            *out = why;
            static const bool logRefusals = std::getenv("PS2X_HD_LOG") != nullptr;
            if (logRefusals)
            {
                // one line per distinct texture (address, format) and reason
                static std::set<std::string> logged;
                const std::string key = std::to_string(q.tbp0) + "/" + std::to_string(q.psm) + "/" + std::to_string(q.cbp) + "/" + why;
                if (logged.size() < 400 && logged.insert(key).second)
                    std::fprintf(stderr, "[hdpack] refused tbp0=0x%X tbw=%u psm=0x%X tw=%u th=%u cbp=0x%X cpsm=%u csm=%u csa=%u: %s\n", q.tbp0, q.tbw, q.psm, q.tw, q.th, q.cbp, q.cpsm, q.csm, q.csa, why);
            }
            return nullptr;
        }

        // Palette of an indexed texture as the GS reads it: entry n of the 16 (PSMT4) or 256 (PSMT8) entries a texture can address, as CT32 words (R,G,B,A).
        // The CSM1 layout reads the CLUT as stacked 8x2 pixel rectangles of the uploaded CT32 rectangle, so entry i sits at pixel position swizzle(i) of a 16x16
        // upload (index bits 3 and 4 swapped) and at position i of an 8x2 upload; PSMT4 reads 16 entries starting at csa * 16.
        bool buildLivePalette(const Ledger &clut, uint32_t psm, uint32_t csa, std::vector<uint8_t> &out)
        {
            const uint32_t count = psm == 0x13 ? 256u : 16u;
            const bool big = clut.w * clut.h >= 256u;
            if (!big && (psm == 0x13 || csa != 0u))
                return false;
            out.resize(static_cast<size_t>(count) * 4u);
            for (uint32_t n = 0; n < count; ++n)
            {
                const uint32_t i = (psm == 0x13 ? 0u : csa * 16u) + n;
                const uint32_t pos = big ? ((i & 0xE7u) | ((i & 0x10u) >> 1) | ((i & 0x08u) << 1)) : i;
                if ((static_cast<size_t>(pos) + 1u) * 4u > clut.data.size())
                    return false;
                std::memcpy(&out[static_cast<size_t>(n) * 4u], &clut.data[static_cast<size_t>(pos) * 4u], 4u);
            }
            return true;
        }

        // The decision for one texture given the current ledger. Pure function of (query, ledger, pack); `why` always receives the outcome.
        bool computeReplacement(State &s, const ReplacementQuery &q, ReplacementResult &out, const char **why)
        {
            auto no = [&](const char *reason) {
                refuse(q, reason, why);
                return false;
            };
            const bool indexedFormat = q.psm == 0x13 || q.psm == 0x14;
            if (q.psm == 0x1B || q.psm == 0x24 || q.psm == 0x2C)
                return no("refused: PSMT8H / PSMT4HL / PSMT4HH textures are not supported");
            if (indexedFormat && (q.csm != 0u || q.cpsm != 0u))
                return no("refused: palette layout is not CSM1/CT32");
            const Ledger *pixels = latestAt(s, q.tbp0);
            if (!pixels)
                return no("no tracked upload (dynamic or unknown texture)");
            // the texture space must be the uploaded rectangle itself: same base, same format, same stride, no offset
            if (pixels->x != 0u || pixels->y != 0u || pixels->dpsm != q.psm || pixels->dbw != q.tbw)
                return no("refused: the upload geometry differs from how the texture is sampled");
            const Identity pid = identify(*pixels);
            const bool ambiguous = pid.status == "ambiguous";
            if (pid.status != "unique" && pid.status != "equivalent" && pid.status != "modified" && !ambiguous)
                return no("no identity for the pixels");
            const bool modified = pid.status == "modified";
            std::vector<hdpack::Region> candidates;
            if (!ambiguous) // an ambiguous identity has no region to ask the pack about: only the content can say (below)
            {
                hdpack::Region r;
                if (regionOfPieces(pid.pieces, r, modified))
                    candidates.push_back(r);
                for (size_t i = pid.status == "equivalent" ? 1u : pid.candidates.size(); i < pid.candidates.size(); ++i)
                    if (regionOfPieces(pid.candidates[i], r))
                        candidates.push_back(r);
            }
            const hdpack::Entry *byContent = nullptr;
            if (candidates.empty())
            {
                // The bytes cannot be tied to one resource region (e.g. a file the game loaded over a buffer that held something else: part of the upload is attributed
                // to the old buffer). The content itself still identifies the picture: an entry made for exactly these bytes, this rectangle and this palette applies.
                const Ledger *c = indexedFormat ? latestAt(s, q.cbp) : nullptr;
                if (!indexedFormat || c)
                {
                    const std::string palSha = c ? ledgerSha(*c) : std::string();
                    bool differs = false;
                    byContent = hdpack::findByContent(ledgerSha(*pixels), pixels->data.size(), pixels->w, pixels->h, pixels->dpsm, c ? &palSha : nullptr, &differs);
                    if (differs)
                        return no("refused: equivalent origins are replaced with different images");
                }
                if (!byContent)
                    return no(ambiguous ? "refused: ambiguous identity of the pixels" : "refused: the pixels do not come from one resource region");
            }

            // the live palette (only indexed formats): any uploaded palette of the right shape, its identity is its content
            const Ledger *clut = nullptr;
            if (indexedFormat)
            {
                clut = latestAt(s, q.cbp);
                if (!clut)
                    return no("refused: no tracked palette upload");
                const bool shape16 = clut->w == 16u && clut->h == 16u;
                const bool shape8x2 = clut->w == 8u && clut->h == 2u;
                if (clut->dpsm != 0u || clut->x != 0u || clut->y != 0u || (q.psm == 0x13 && !shape16) || (q.psm == 0x14 && !shape16 && !shape8x2))
                    return no("refused: the palette upload is not a whole CT32 palette (16x16, or 8x2 for 4 bit textures)");
                if (q.psm == 0x14 && shape8x2 && q.csa != 0u)
                    return no("refused: CSA != 0 with a 16 entry palette upload");
            }

            // pack entries of every candidate origin
            const hdpack::Entry *chosen = byContent;
            bool known = byContent != nullptr, conflict = false;
            std::string paletteSha;
            if (clut && !hdpack::loaded())
                return no("no pack entry for this identity");
            for (hdpack::Region &r : candidates)
            {
                if (!hdpack::knowsPixels(r))
                    continue;
                known = true;
                r.sha1 = regionSha1(*pixels, r);
                if (clut && paletteSha.empty())
                    paletteSha = ledgerSha(*clut);
                // a modified buffer only matches an entry made for exactly the content that was uploaded (its SHA-1 is mandatory)
                const hdpack::Entry *e = hdpack::find(r, clut ? &paletteSha : nullptr, modified);
                if (!e)
                    continue;
                if (!chosen)
                    chosen = e;
                else if (chosen != e && !hdpack::sameImage(*chosen, *e))
                    conflict = true; // the same bytes held by several resources, replaced with different images
            }
            bool viaContent = byContent != nullptr;
            if (!chosen && modified)
            {
                // A buffer the game modified after loading is identified by the content that was uploaded anyway (pixels.sha1 is mandatory for its entries); when no
                // entry exists for the region it was loaded from, an entry made for exactly these bytes, rectangle and palette (e.g. the same picture loaded from a
                // loose file) still applies.
                if (clut && paletteSha.empty())
                    paletteSha = ledgerSha(*clut);
                chosen = hdpack::findByContent(ledgerSha(*pixels), pixels->data.size(), pixels->w, pixels->h, pixels->dpsm, clut ? &paletteSha : nullptr);
                viaContent = known = chosen != nullptr;
            }
            if (!known || !chosen)
            {
                static const bool logMisses = std::getenv("PS2X_HD_LOG") != nullptr;
                static int missesLogged = 0;
                if (logMisses && known && missesLogged++ < 60)
                    std::fprintf(stderr, "[hdpack] miss: pixels %s@%llu+%llu/%llu sha1 %s palette sha1 %s\n", candidates[0].file.c_str(),
                                 static_cast<unsigned long long>(candidates[0].compressed ? candidates[0].streamOffset : 0), static_cast<unsigned long long>(candidates[0].offset),
                                 static_cast<unsigned long long>(candidates[0].length), candidates[0].sha1.c_str(), paletteSha.c_str());
                return no(known ? "no pack entry for this palette (rgba entries need the palette of the manifest)" : "no pack entry for this identity");
            }
            if (conflict)
                return no("refused: equivalent origins are replaced with different images");
            if (chosen->origWidth != pixels->w || chosen->origHeight != pixels->h || chosen->psm != pixels->dpsm)
                return no("refused: pack entry describes a different rectangle");
            out = ReplacementResult{};
            out.entry = chosen;
            out.key = chosen->key;
            if (chosen->indexed)
            {
                if (!clut || !buildLivePalette(*clut, q.psm, q.csa, out.palette))
                    return no("refused: the live palette cannot be read");
                out.paletteEntries = static_cast<uint32_t>(out.palette.size() / 4u);
                out.paletteHash = fnv64(out.palette.data(), out.palette.size());
                out.key = chosen->key ^ (out.paletteHash * 0x9E3779B97F4A7C15ull) ^ 0x100000000ull;
            }
            *why = viaContent ? "applied (by content: origin not attributable to one region)" : chosen->indexed ? "applied (indexed)" : (modified ? "applied (modified source, content verified)" : (pid.status == "equivalent" ? "applied (equivalent origins)" : "applied"));
            return true;
        }
    } // namespace

    bool findReplacement(const ReplacementQuery &q, ReplacementResult &out, const char **why)
    {
        out = ReplacementResult{};
        const char *unused = nullptr;
        if (!why)
            why = &unused;
        if (!hdpack::enabled())
            return false;
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.enabled)
            return false;
        ProfScope prof(PfFind);
        // The answer only depends on the ledger (and the pack), so it is memoised per texture until the ledger changes.
        uint64_t key = 1469598103934665603ull;
        for (const uint32_t v : {q.tbp0, q.tbw, q.psm, q.tw, q.th, q.cbp, q.cpsm, q.csm, q.csa})
        {
            key ^= v;
            key *= 1099511628211ull;
        }
        const char *reason = nullptr;
        bool found = false;
        const auto memo = s.replacementMemo.find(key);
        if (memo != s.replacementMemo.end() && memo->second.tbpVersion == s.versionOf(memo->second.tbp0) && memo->second.cbpVersion == s.versionOf(memo->second.cbp))
        {
            found = memo->second.found;
            reason = memo->second.reason;
            if (found)
                out = memo->second.result;
        }
        else
        {
            found = computeReplacement(s, q, out, &reason);
            static const bool logDecisions = std::getenv("PS2X_HD_LOG") != nullptr && std::string(std::getenv("PS2X_HD_LOG")) == "2";
            if (logDecisions)
            {
                // PS2X_HD_LOG=2: one line per decision (memoised until the ledger changes): the uploaded rectangle, where its bytes came from and the outcome
                const Ledger *up = latestAt(s, q.tbp0);
                std::string origin = "-";
                if (up)
                {
                    const Identity pid = identify(*up);
                    origin = pid.status;
                    for (size_t i = 0; i < pid.pieces.size() && i < 2; ++i)
                    {
                        const Piece &p = pid.pieces[i];
                        origin += " " + (p.origin ? p.origin->path : std::string("?")) + "+" + std::to_string(p.off) + (p.verified ? "" : "(stale)");
                    }
                }
                std::fprintf(stderr, "[hdpack] decision vb=%llu tbp0=0x%X psm=0x%X upload=%ux%u sha1=%.8s origin=%s -> %s\n", static_cast<unsigned long long>(g_ps2xGsVblank.load(std::memory_order_relaxed)),
                             q.tbp0, q.psm, up ? up->w : 0u, up ? up->h : 0u, up ? ledgerSha(*up).c_str() : "-", origin.c_str(), reason);
            }
            if (s.replacementMemo.size() > 8192)
                s.replacementMemo.clear();
            auto &m = s.replacementMemo[key];
            m.tbp0 = q.tbp0;
            m.cbp = q.cbp;
            m.tbpVersion = s.versionOf(q.tbp0);
            m.cbpVersion = s.versionOf(q.cbp);
            m.found = found;
            m.reason = reason;
            if (found)
                m.result = out;
        }
        ++s.replacementCounters[reason];
        *why = reason;
        return found;
    }

    void forgetReplacementDecisions()
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.replacementMemo.clear();
    }

    std::vector<std::pair<std::string, uint64_t>> replacementReport()
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::vector<std::pair<std::string, uint64_t>> out(s.replacementCounters.begin(), s.replacementCounters.end());
        if (g_profOn)
            for (int i = 0; i < PfCount; ++i)
                out.emplace_back(std::string("time ms (") + std::to_string(g_prof[i].calls.load()) + " calls) " + kProfNames[i], g_prof[i].ns.load() / 1000000u);
        return out;
    }
} // namespace texprov
