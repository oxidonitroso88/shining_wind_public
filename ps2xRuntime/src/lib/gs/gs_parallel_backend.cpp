// Experimental GS backend on parallel-gs (Arntzen Software, Vulkan, LGPLv3+). See gs_parallel_backend.h.
//
// Design ("hybrid"): the frontend keeps parsing everything (debug history, preferred display source, presentation heuristics)
// and calls this backend's Submit/LoadClut/UploadImage as usual, but those are no-ops here: the same commands reach parallel-gs
// raw through the GSRawTap. parallel-gs is the source of truth for the 4 MiB VRAM. Everything that needs host-side VRAM access
// (Present, ReadVram/WriteVram, framebuffer clears, local->host transfers, snapshots) runs on an internal non-drawing
// GSCpuBackend over a host mirror, which is refreshed from the GPU lazily (only when the GPU changed VRAM since the last mirror)
// and pushed back to the GPU after host-side writes.
#include "gs_interface.hpp"
#include "gs_renderer.hpp"
#include "context.hpp"
#include "device.hpp"
#include "thread_id.hpp"

#include "runtime/gs/gs_parallel_backend.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_raw_tap.h"
#include "runtime/gs/gs_present_info.h"
#include "runtime/gs/gs_types.h"
#include "runtime/gs/hd_pack.h"
#include "runtime/gs/tex_provenance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    constexpr uint32_t kMaxImageQwordsPerTag = 0x7FFFu; // GIF NLOOP is 15 bits

    class ParallelGSBackend final : public GSRasterBackend, public GSRawTap, private ParallelGS::SignalInterface, private ParallelGS::TextureReplacer
    {
    public:
        ParallelGSBackend()
        {
            if (!Vulkan::Context::init_loader(nullptr))
                throw std::runtime_error("no Vulkan loader");
            m_context.set_num_thread_indices(1);
            if (!m_context.init_instance_and_device(nullptr, 0, nullptr, 0,
                                                    Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                                        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                                        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
                throw std::runtime_error("Vulkan device creation failed (a GPU with descriptor indexing, subgroup ops and descriptor buffers is required)");
            m_device.set_context(m_context);
            m_device.init_frame_contexts(4);
            // PS2X_GS_SCALE=1|2|4: internal resolution factor. 2 = 2x2 super-sampling (1280x896), 4 = 4x4 (2560x1792), scanned out at the higher
            // resolution (parallel-gs high_resolution_scanout). 1 (default) keeps the original 640x448 presentation through the CPU CRTC.
            // PS2X_GS_PROMOTE=1 enables parallel-gs' back-buffer promotion (replaces blitted scanout with the original higher-resolution input).
            ParallelGS::GSOptions options{};
            if (const char *scale = std::getenv("PS2X_GS_SCALE"))
            {
                const int s = std::atoi(scale);
                if (s == 2) { options.super_sampling = ParallelGS::SuperSampling::X4; m_scale = 2; }
                else if (s >= 4) { options.super_sampling = ParallelGS::SuperSampling::X16; m_scale = 4; }
            }
            if (const char *output = std::getenv("PS2X_GS_OUTPUT"))
                m_outputLog2 = std::atoi(output) >= 4 ? 2u : 1u;
            // PS2X_GS_SSTEX=1: super-sampled textures (parallel-gs' own replayer enables them with the high resolution scanout so that data rendered with
            // super-sampling survives the game's blit passes). Off by default: no difference was seen with this game, and textures bound as super-sampled arrays
            // cannot be replaced by HD texture packs.
            // PS2X_GS_PROMOTE=1 needs them (the promoted copy of the game's frame keeps its samples only as a super-sampled texture), so it turns them on
            // unless PS2X_GS_SSTEX=0 says otherwise. Without PS2X_GS_PROMOTE nothing changes.
            if (const char *promote = std::getenv("PS2X_GS_PROMOTE"); promote && promote[0] != '0')
                options.super_sampled_textures = true;
            if (const char *sstex = std::getenv("PS2X_GS_SSTEX"))
                options.super_sampled_textures = sstex[0] != '0';
            if (!m_gs.init(&m_device, options))
                throw std::runtime_error("parallel-gs initialisation failed");
            std::fprintf(stderr, "[gs] internal resolution factor %d (PS2X_GS_SCALE), scanout factor %u (PS2X_GS_OUTPUT)\n", m_scale, 1u << m_outputLog2);
            {
                ParallelGS::Hacks hacks{};
                bool anyHack = false;
                if (const char *promote = std::getenv("PS2X_GS_PROMOTE"); promote && promote[0] != '0')
                {
                    hacks.backbuffer_promotion = true;
                    anyHack = true;
                }
#if defined(PARALLEL_GS_HD_PER_SAMPLE)
                // PS2X_GS_HD_SSAA=1: sprites that sample an HD texture replacement interpolate their UVs per super-sample (default: once per pixel,
                // MSAA-like), so that PS2X_GS_SCALE / PS2X_GS_OUTPUT show the detail of the HD images (X4 patch of parallel-gs).
                if (const char *hdssaa = std::getenv("PS2X_GS_HD_SSAA"); hdssaa && hdssaa[0] != '0')
                {
                    hacks.hd_per_sample_sprites = true;
                    anyHack = true;
                }
#endif
                if (anyHack)
                    m_gs.set_hacks(hacks);
            }
            if (std::getenv("PS2X_GS_PERF"))
            {
                ParallelGS::DebugMode dm{};
                dm.timestamps = true;       // GPU time per stage of the renderer, reported by PS2X_GS_PERF
                m_gs.set_debug_mode(dm);
            }
            if (const char *t = std::getenv("PS2X_GS_DOWNSCALE_TARGET"))
            {
                unsigned fw = 0, fh = 0;
                if (std::sscanf(t, "%ux%u", &fw, &fh) == 2)
                {
                    m_forcedW = fw;
                    m_forcedH = fh;
                }
            }
            m_gs.set_signal_interface(this);
            // HD texture packs (PS2X_HD_PACK): the host decides the identity of each texture, parallel-gs binds the replacement image. Always installed when a
            // pack was requested (the lookup is a no-op while replacements are off or no pack loaded); never installed otherwise.
            if (texprov::enabled())
            {
                uint64_t budgetMb = 512;
                if (const char *b = std::getenv("PS2X_HD_BUDGET_MB"))
                    budgetMb = std::max<uint64_t>(1, std::strtoull(b, nullptr, 10));
                m_gs.set_texture_replacer(this, budgetMb << 20);
                m_hdStats = std::getenv("PS2X_HD_STATS") != nullptr;
            }
        }

        ~ParallelGSBackend() override
        {
            if (gsRawTap() == static_cast<GSRawTap *>(this))
                gsSetRawTap(nullptr);
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            Util::register_thread_index(0);
            m_gs.flush();
            m_device.wait_idle();
            if (m_hdStats)
                reportHd();
        }

        // ---- GSRawTap: the raw command stream that parallel-gs rasterises
        void OnGifPackets(const uint8_t *data, uint32_t sizeBytes) override
        {
            Lock lock(m_mutex);
            sizeBytes &= ~15u; // whole quadwords only (the frontend ignores a trailing partial tag as well)
            if (!data || !sizeBytes)
                return;
            m_gs.gif_transfer(3, data, sizeBytes);
            m_gpuDirty = true;
        }

        void OnRegister(uint8_t addr, uint64_t value) override
        {
            Lock lock(m_mutex);
            // 0x58 and up are privileged/alias registers (DISPFB, DISPLAY, SIGNAL, ...): they never draw and are handled by the frontend.
            if (addr >= 0x58)
                return;
            m_gs.write_register(static_cast<ParallelGS::RegisterAddr>(addr), value);
            m_gpuDirty = true;
        }

        void OnImageUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes) override
        {
            Lock lock(m_mutex);
            if (!data || !sizeBytes)
                return;
            m_gs.write_register(ParallelGS::RegisterAddr::BITBLTBUF, bitbltbuf);
            m_gs.write_register(ParallelGS::RegisterAddr::TRXPOS, trxpos);
            m_gs.write_register(ParallelGS::RegisterAddr::TRXREG, trxreg);
            m_gs.write_register(ParallelGS::RegisterAddr::TRXDIR, trxdir);
            uint32_t whole = sizeBytes & ~15u;
            const uint8_t *p = data;
            while (whole)
            {
                const uint32_t qwords = std::min<uint32_t>(whole / 16u, kMaxImageQwordsPerTag);
                const uint64_t tag[2] = {uint64_t(qwords) | (1ull << 15) | (2ull << 58), 0ull}; // NLOOP, EOP, FLG=IMAGE
                m_gs.gif_transfer(3, tag, sizeof(tag));
                m_gs.gif_transfer(3, p, qwords * 16u);
                p += qwords * 16u;
                whole -= qwords * 16u;
            }
            for (uint32_t off = sizeBytes & ~15u; off < sizeBytes; off += 8u) // trailing 8-byte piece through HWREG
            {
                uint64_t q = 0;
                std::memcpy(&q, data + off, std::min<uint32_t>(8u, sizeBytes - off));
                m_gs.write_register(ParallelGS::RegisterAddr::HWREG, q);
            }
            m_gpuDirty = true;
        }

        // ---- GSRasterBackend
        void Initialize(uint8_t *vram, uint32_t vramSize) override
        {
            Lock lock(m_mutex);
            if (!vram || vramSize < kVramSize)
                throw std::invalid_argument("parallel-gs backend requires 4 MiB of VRAM");
            m_hostVram = vram;
            m_vramSize = vramSize;
            m_cpu.Initialize(vram, vramSize);
            pushHostVram();
        }

        void Reset() override
        {
            Lock lock(m_mutex);
            m_gs.flush();
            m_device.wait_idle();
            m_gs.reset_context_state();
            m_cpu.Reset();
        }

        // Drawing and uploads are driven by the raw tap.
        void Submit(const GSPrimitiveBatch &) override {}
        void LoadClut(const GSTex0Reg &, const GSTexClutReg &) override {}
        void UploadImage(const uint8_t *, uint32_t) override {}
        void TextureFlush() override {}

        void BeginTransfer(const GSTransferCommand &command) override
        {
            // Only local->host reads need the host-side engine; its source is the mirrored VRAM.
            if (command.direction == 1u)
            {
                Lock lock(m_mutex);
                mirrorHostVram();
                m_cpu.BeginTransfer(command);
            }
        }

        void Flush() override
        {
            Lock lock(m_mutex);
            m_gs.flush();
        }

        void Sync(GSSyncReason reason) override
        {
            Lock lock(m_mutex);
            m_gs.flush();
            if (reason == GSSyncReason::Reset)
                m_device.wait_idle();
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            Lock lock(m_mutex);
            if (m_hdStats && (++m_presents % 600u) == 0u)
                reportHd();
            if (m_scale > 1 || m_vsyncPresent)
            {
                PresentationFrame high = presentHighResolution(request);
                if (high)
                    return high;
            }
            gs_present_info::setLogicalSize(0u, 0u);
            mirrorHostVram();
            return m_cpu.Present(request);
        }

        bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override
        {
            Lock lock(m_mutex);
            mirrorHostVram();
            const bool ok = m_cpu.ClearFramebuffer(context, rgba);
            if (ok)
                pushHostVram();
            return ok;
        }

        uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override
        {
            Lock lock(m_mutex);
            return m_cpu.ConsumeLocalToHostBytes(dst, maxBytes);
        }

        uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override
        {
            Lock lock(m_mutex);
            const_cast<ParallelGSBackend *>(this)->mirrorHostVram();
            return m_cpu.ReadVram(psm, base, bw, x, y);
        }

        void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override
        {
            Lock lock(m_mutex);
            mirrorHostVram();
            m_cpu.WriteVram(psm, base, bw, x, y, value);
            pushHostVram();
        }

        void SnapshotVram(std::vector<uint8_t> &out) const override
        {
            Lock lock(m_mutex);
            const_cast<ParallelGSBackend *>(this)->mirrorHostVram();
            m_cpu.SnapshotVram(out);
        }

        GSTransferSnapshot GetTransferSnapshot() const override
        {
            Lock lock(m_mutex);
            return m_cpu.GetTransferSnapshot();
        }

    private:
        // ---- ParallelGS::TextureReplacer (HD texture packs)
        bool lookup_replacement(const ParallelGS::TextureDescriptor &, const ParallelGS::TEX0Bits &t, ParallelGS::TextureReplacement &out) override
        {
            const texprov::ReplacementQuery query{uint32_t(t.TBP0), uint32_t(t.TBW), uint32_t(t.PSM), uint32_t(t.TW), uint32_t(t.TH),
                                                  uint32_t(t.CBP), uint32_t(t.CPSM), uint32_t(t.CSM), uint32_t(t.CSA)};
            const char *why = nullptr;
            texprov::ReplacementResult found;
            if (!texprov::findReplacement(query, found, &why))
                return false;
            const hdpack::Entry *entry = found.entry;
            if (m_replacements.size() > 2048)
                m_replacements.clear(); // pgs asks for the pixels right after a lookup, so only recent keys matter
            m_replacements[found.key] = Pending{entry, std::move(found.palette), found.paletteEntries, found.paletteHash};
            out.key = found.key;
            out.rgba = nullptr; // decoded / composed on demand, only when the GPU image is not cached yet
            out.width = entry->width;
            out.height = entry->height;
            out.scale_x = entry->scaleX;
            out.scale_y = entry->scaleY;
            out.origin_x = 0;
            out.origin_y = 0;
            out.orig_width = entry->origWidth;
            out.orig_height = entry->origHeight;
            out.linear = entry->linear;
            return true;
        }

        const uint8_t *replacement_pixels(uint64_t key) override
        {
            const auto it = m_replacements.find(key);
            const uint8_t *pixels = nullptr;
            if (it != m_replacements.end())
                pixels = it->second.entry->indexed ? hdpack::pixelsIndexed(*it->second.entry, it->second.palette.data(), it->second.paletteEntries, it->second.paletteHash)
                                                   : hdpack::pixels(*it->second.entry);
            if (!pixels)
                texprov::forgetReplacementDecisions(); // the entry has just been disabled (missing PNG / wrong size): decide again
            return pixels;
        }

        void reportHd() const
        {
            const ParallelGS::TextureReplacementStats st = m_gs.get_texture_replacement_stats();
            std::fprintf(stderr, "[hdpack] gpu: lookups %llu, bound %llu, rejected %llu, images %llu (evicted %llu), resident %.2f MiB\n", static_cast<unsigned long long>(st.lookups),
                         static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.rejected), static_cast<unsigned long long>(st.images_created),
                         static_cast<unsigned long long>(st.images_evicted), double(st.resident_bytes) / (1024.0 * 1024.0));
            for (const auto &kv : texprov::replacementReport())
                std::fprintf(stderr, "[hdpack]   %-72s %llu\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
        }

        // Granite needs the calling thread registered; every entry point is serialised, so index 0 is shared.
        struct Lock
        {
            std::lock_guard<std::recursive_mutex> guard;
            explicit Lock(std::recursive_mutex &m) : guard(m) { Util::register_thread_index(0); }
        };

        // The display area as the game programmed it is read by the CPU CRTC as (DW+1)/(MAGH+1) pixels (DW=639, MAGH=0 -> 640). parallel-gs models the real
        // hardware, where a 640 pixel line is 2560 video clocks at 4 clocks per pixel (DW=2559, MAGH=3): convert, keeping the pixel width.
        static uint64_t normalizeDisplayRegister(uint64_t display)
        {
            const uint64_t dw = (display >> 32) & 0xFFFu;
            const uint64_t magh = (display >> 23) & 0xFu;
            const uint64_t width = (dw + 1u) / (magh + 1u);
            if (width < 64u || width > 1023u)
                return display;
            display &= ~((0xFFFull << 32) | (0xFull << 23));
            return display | (3ull << 23) | (((width * 4u - 1u) & 0xFFFull) << 32);
        }

        // High-resolution scanout (PS2X_GS_SCALE > 1): the CRTC state goes to parallel-gs, vsync() produces the (super-sampled) scanout image and it is
        // read back as RGBA8. Returns an empty frame when nothing is displayed (the caller then falls back to the CPU CRTC).
        // Diagnostics (PS2X_GS_SLICE_DUMP=<folder>, PS2X_GS_SLICE_DUMP_AT=<vsync tick>): writes the displayed frame buffer as seen in every slice of the GPU VRAM
        // (0 = the normal picture, 1 = super-sample marker plane, 2.. = super-sample layers) as PPM files, to look at how super-sampled data is stored.
        void dumpSlices(const GSPresentationRequest &request)
        {
            static const char *dir = std::getenv("PS2X_GS_SLICE_DUMP");
            if (!dir || request.vsyncTick < std::strtoull(std::getenv("PS2X_GS_SLICE_DUMP_AT") ? std::getenv("PS2X_GS_SLICE_DUMP_AT") : "0", nullptr, 10))
                return;
            static bool done = false;
            if (done)
                return;
            done = true;
            const uint64_t dispfb = (request.pmode & 2u) ? request.dispfb2 : request.dispfb1;
            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu), fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu), psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
            std::vector<uint8_t> vram(kVramSize);
            auto tmp = std::make_unique<GSCpuBackend>();
            for (uint32_t slice = 0; slice < 8; ++slice)
            {
                if (!m_gs.debug_read_vram_slice(slice, 0, kVramSize, vram.data()))
                    break;
                tmp->Initialize(vram.data(), kVramSize);
                const std::string path = std::string(dir) + "/slice" + std::to_string(slice) + ".ppm";
                if (FILE *f = std::fopen(path.c_str(), "wb"))
                {
                    const uint32_t w = std::max<uint32_t>(1u, fbw * 64u), h = 448u;
                    std::fprintf(f, "P6 %u %u 255 ", w, h);
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                        {
                            const uint32_t v = tmp->ReadVram(psm, fbp * 32u, fbw, x, y);
                            const uint8_t rgb[3] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)};
                            std::fwrite(rgb, 1, 3, f);
                        }
                    std::fclose(f);
                }
            }
            std::fprintf(stderr, "[gs] slices of fbp %u fbw %u psm %u written to %s\n", fbp, fbw, psm, dir);
        }

        PresentationFrame presentHighResolution(const GSPresentationRequest &request)
        {
            PresentationFrame frame{};
            dumpSlices(request);
            auto &priv = m_gs.get_priv_register_state();
            std::memcpy(&priv.pmode, &request.pmode, sizeof(uint64_t));
            uint64_t smode2 = request.smode2;
            if (const char *o = std::getenv("PS2X_GS_SMODE2")) smode2 = std::strtoull(o, nullptr, 16); // experiment
            // Field rendered games (Shining Wind: INT=1, FFMD=1 but DISPFB alternates between two 640x224 field buffers while DISPLAY asks for 448 lines) are
            // handled by parallel-gs' own field aware path (default, smoothest result). PS2X_GS_FIELD_PROGRESSIVE=1 (experiment) instead describes the scanout of
            // one field as what it is, a 640x224 progressive (240p) picture.
            const bool fieldProgressive = m_scale > 1 && (smode2 & 3u) == 3u && std::getenv("PS2X_GS_FIELD_PROGRESSIVE") != nullptr;
            auto fieldDisplay = [&](uint64_t display) {
                if (!fieldProgressive)
                    return display;
                const uint64_t dy = (display >> 12) & 0x7FFu;
                const uint64_t dh = (display >> 44) & 0x7FFu;
                display &= ~((0x7FFull << 12) | (0x7FFull << 44));
                return display | ((dy >> 1) << 12) | ((((dh + 1u) >> 1) - 1u) << 44);
            };
            if (fieldProgressive)
                smode2 &= ~3ull;
            std::memcpy(&priv.smode2, &smode2, sizeof(uint64_t));
            // SMODE1 is not part of the presentation request: Shining Wind is NTSC analog (640x448); parallel-gs needs CMOD/LC to pick the video mode.
            std::memset(&priv.smode1, 0, sizeof(uint64_t));
            priv.smode1.CMOD = ParallelGS::SMODE1Bits::CMOD_NTSC;
            priv.smode1.LC = ParallelGS::SMODE1Bits::LC_ANALOG;
            std::memcpy(&priv.dispfb1, &request.dispfb1, sizeof(uint64_t));
            const uint64_t display1 = normalizeDisplayRegister(fieldDisplay(request.display1));
            std::memcpy(&priv.display1, &display1, sizeof(uint64_t));
            std::memcpy(&priv.dispfb2, &request.dispfb2, sizeof(uint64_t));
            const uint64_t display2 = normalizeDisplayRegister(fieldDisplay(request.display2));
            std::memcpy(&priv.display2, &display2, sizeof(uint64_t));
            std::memcpy(&priv.bgcolor, &request.bgcolor, sizeof(uint64_t));
            ParallelGS::VSyncInfo info{};
            info.phase = static_cast<uint32_t>(request.vsyncTick & 1u);
            info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
            info.dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            info.adapt_to_internal_horizontal_resolution = true;
            info.force_progressive = true;
            info.anti_blur = true;
            info.high_resolution_scanout = m_scale > 1 && std::getenv("PS2X_GS_NOHIRES") == nullptr;
#if defined(PARALLEL_GS_SCANOUT_LOG2)
            // PS2X_GS_OUTPUT=2|4: scanout factor of the high resolution output (default 2 = 1280x447, the original behaviour). 4 = one output pixel per
            // super-sample (2560x~894 with the 4x4 grid), only with PS2X_GS_SCALE=4; parallel-gs falls back to 2 otherwise (X4 patch of parallel-gs).
            info.high_resolution_scanout_log2 = m_outputLog2;
#endif
            if (info.high_resolution_scanout && std::getenv("PS2X_GS_SSAA_NOPROG") != nullptr) info.force_progressive = false; // experiment
            const auto perfT0 = std::chrono::steady_clock::now();
            ParallelGS::ScanoutResult result = m_gs.vsync(info);
            const auto perfT1 = std::chrono::steady_clock::now();
            static int logged = 0;
            if (logged < 6)
            {
                ++logged;
                std::fprintf(stderr, "[gs] high-res scanout: %s%s (pmode=0x%llx smode2=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx)\n", result.image ? "image " : "no image",
                             result.image ? (std::to_string(result.image->get_width()) + "x" + std::to_string(result.image->get_height())).c_str() : "",
                             static_cast<unsigned long long>(request.pmode), static_cast<unsigned long long>(request.smode2), static_cast<unsigned long long>(request.dispfb1), static_cast<unsigned long long>(request.display1), static_cast<unsigned long long>(request.dispfb2), static_cast<unsigned long long>(request.display2));
            }
            if (!result.image)
                return frame;
            const uint32_t srcW = result.image->get_width();
            const uint32_t srcH = result.image->get_height();
            // PS2X_GS_GPU_DOWNSCALE=1 (experimental): the frame is rendered, scanned out and kept at the full internal resolution, and only the copy that goes to
            // the window is shrunk on the GPU (bilinear blit) to the size the picture occupies on screen, so the read-back and the host copies move 4x less data.
            uint32_t w = srcW, h = srcH;
            bool shrink = false;
            if (m_gpuDownscale)
            {
                uint32_t tw = 0, th = 0;
                gs_present_info::targetSize(tw, th);
                if (m_forcedW)                  // PS2X_GS_DOWNSCALE_TARGET=WxH: measurement aid, independent of the window
                {
                    tw = m_forcedW;
                    th = m_forcedH;
                }
                if (tw >= 64 && th >= 64 && tw < srcW && th < srcH)
                {
                    w = tw;
                    h = th;
                    shrink = true;
                }
            }
            // The readback buffer is reused while the size stays the same: a fresh 18 MiB host buffer per frame costs page faults on every first touch.
            Vulkan::BufferHandle &readback = m_readback;
            if (!readback || m_readbackSize != static_cast<VkDeviceSize>(w) * h * 4u)
            {
                Vulkan::BufferCreateInfo bufInfo{};
                bufInfo.size = static_cast<VkDeviceSize>(w) * h * 4u;
                bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                bufInfo.domain = Vulkan::BufferDomain::CachedHost;
                readback = m_device.create_buffer(bufInfo);
                m_readbackSize = bufInfo.size;
            }
            if (shrink && (!m_shrunk || m_shrunk->get_width() != w || m_shrunk->get_height() != h))
            {
                auto info = Vulkan::ImageCreateInfo::immutable_2d_image(w, h, VK_FORMAT_R8G8B8A8_UNORM);
                info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                m_shrunk = m_device.create_image(info);
            }
            auto cmd = m_device.request_command_buffer();
            Vulkan::QueryPoolHandle tsStart, tsEnd;
            if (m_perf)
                tsStart = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            cmd->image_barrier(*result.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            if (shrink)
            {
                cmd->image_barrier(*m_shrunk, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                cmd->blit_image(*m_shrunk, *result.image, {0, 0, 0}, {int32_t(w), int32_t(h), 1}, {0, 0, 0}, {int32_t(srcW), int32_t(srcH), 1}, 0, 0, 0, 0, 1,
                                VK_FILTER_LINEAR);
                cmd->image_barrier(*m_shrunk, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                cmd->copy_image_to_buffer(*readback, *m_shrunk, 0, {}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            }
            else
                cmd->copy_image_to_buffer(*readback, *result.image, 0, {}, {w, h, 1}, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            if (m_perf)
                tsEnd = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            m_device.submit(cmd);
            m_device.wait_idle();
            if (m_perf && tsStart && tsEnd && tsStart->is_signalled() && tsEnd->is_signalled())
                m_perfGpuPresent += m_device.convert_device_timestamp_delta(tsStart->get_timestamp_ticks(), tsEnd->get_timestamp_ticks()) * 1000.0;
            const uint8_t *src = static_cast<const uint8_t *>(m_device.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (!src)
                return frame;
            const auto perfT2 = std::chrono::steady_clock::now();
            // one pass: copy and force the alpha channel opaque (32-bit words, little endian: A is the top byte)
            frame.pixels.resize(static_cast<size_t>(w) * h * 4u);
            {
                const size_t words = static_cast<size_t>(w) * h;
                uint32_t *dst = reinterpret_cast<uint32_t *>(frame.pixels.data());
                for (size_t i = 0; i < words; ++i)
                {
                    uint32_t px;
                    std::memcpy(&px, src + i * 4u, 4u);
                    dst[i] = px | 0xFF000000u;
                }
            }
            if (m_perf)
                perfRecord(perfT0, perfT1, perfT2, std::chrono::steady_clock::now(), w, h);
            frame.width = w;
            frame.height = h;
            // the picture is mode_width x (mode_height, doubled when the original signal was interlaced) in logical pixels
            // A promoted back buffer (PS2X_GS_PROMOTE) is already the whole frame (mode_height lines, progressive), not one field: no doubling then.
            const uint32_t outputFactor = result.mode_width ? std::max<uint32_t>(1u, srcW / result.mode_width) : 1u;
            const bool wholeFrame = result.mode_height >= 400u || (srcH / outputFactor) >= 400u;
            const uint32_t logicalHeight = result.mode_height * (((request.smode2 & 1u) && !wholeFrame) ? 2u : 1u);
            gs_present_info::setLogicalSize(result.high_resolution_scanout ? result.mode_width : 0u, result.high_resolution_scanout ? logicalHeight : 0u);
            return frame;
        }

        // PS2X_GS_PERF=1: every 300 presents, average cost (ms) of the parts of the high resolution presentation and the real period between presents.
        void perfRecord(std::chrono::steady_clock::time_point t0, std::chrono::steady_clock::time_point t1, std::chrono::steady_clock::time_point t2,
                        std::chrono::steady_clock::time_point t3, uint32_t w, uint32_t h)
        {
            auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            m_perfVsync += ms(t0, t1);
            m_perfWait += ms(t1, t2);
            m_perfCopy += ms(t2, t3);
            if (m_perfLast.time_since_epoch().count() != 0)
            {
                const double period = ms(m_perfLast, t0);
                m_perfPeriod += period;
                m_perfMax = std::max(m_perfMax, period);
                if (period > 20.0)
                    ++m_perfLate;
                // frames that took > 25 ms between two presentations: when, how long, and how much of it was this presentation
                if (period + ms(t0, t3) > 25.0 && m_perfSpikeLines < 400)
                {
                    ++m_perfSpikeLines;
                    std::fprintf(stderr, "[spike] n=%llu frame=%.1f ms (outside presentation %.1f, presentation %.1f: vsync %.1f wait %.1f copy %.1f)\n", static_cast<unsigned long long>(m_perfTotal), period + ms(t0, t3), period,
                                 ms(t0, t3), ms(t0, t1), ms(t1, t2), ms(t2, t3));
                }
            }
            m_perfLast = t3;
            ++m_perfTotal;
            if (++m_perfN == 300)
            {
                auto gpu = [&](ParallelGS::TimestampType t) {
                    const double now = m_gs.get_accumulated_timestamps(t);
                    const double d = (now - m_perfGpuPrev[int(t)]) * 1000.0 / 300.0;
                    m_perfGpuPrev[int(t)] = now;
                    return d;
                };
                std::fprintf(stderr, "[perf] %ux%u  per present (ms): gs.vsync %.2f  readback+wait_idle %.2f  host copy %.2f | outside presentation %.2f (max %.1f) | >20 ms: %u/300 | GPU ms: present(blit+copy) %.2f  shading %.2f  binning %.2f  setup %.2f  vsync %.2f  texupload %.2f  copyvram %.2f  gs-readback %.2f\n", w, h,
                             m_perfVsync / 300, m_perfWait / 300, m_perfCopy / 300, m_perfPeriod / 299, m_perfMax, m_perfLate, m_perfGpuPresent / 300,
                             gpu(ParallelGS::TimestampType::Shading), gpu(ParallelGS::TimestampType::Binning), gpu(ParallelGS::TimestampType::TriangleSetup),
                             gpu(ParallelGS::TimestampType::VSync), gpu(ParallelGS::TimestampType::TextureUpload), gpu(ParallelGS::TimestampType::CopyVRAM),
                             gpu(ParallelGS::TimestampType::Readback));
                m_perfGpuPresent = 0;
                m_perfN = m_perfLate = 0;
                m_perfVsync = m_perfWait = m_perfCopy = m_perfPeriod = m_perfMax = 0;
            }
        }

        // GPU -> host mirror, only when the GPU may have changed VRAM since the last mirror.
        void mirrorHostVram()
        {
            if (!m_gpuDirty || !m_hostVram)
                return;
            m_gs.flush();
            const void *vram = m_gs.map_vram_read(0, m_vramSize);
            std::memcpy(m_hostVram, vram, m_vramSize);
            m_gpuDirty = false;
        }

        // host -> GPU after a host-side VRAM modification.
        void pushHostVram()
        {
            if (!m_hostVram)
                return;
            (void)m_gs.map_vram_read(0, m_vramSize); // make the shadow current before overwriting it
            std::memcpy(m_gs.map_vram_write(0, m_vramSize), m_hostVram, m_vramSize);
            m_gs.end_vram_write(0, m_vramSize);
            m_gpuDirty = false;
        }

        bool on_signal(uint64_t) override { return false; }
        bool on_finish(uint64_t) override { return false; }
        bool on_label(uint64_t) override { return false; }

        mutable std::recursive_mutex m_mutex;
        Vulkan::Context m_context;
        Vulkan::Device m_device;
        mutable ParallelGS::GSInterface m_gs;
        mutable GSCpuBackend m_cpu; // never rasterises: presentation and host-side VRAM access over m_hostVram
        uint8_t *m_hostVram = nullptr;
        uint32_t m_vramSize = 0;
        bool m_gpuDirty = false;
        bool m_hdStats = false;
        uint32_t m_presents = 0;
        struct Pending
        {
            const hdpack::Entry *entry = nullptr;
            std::vector<uint8_t> palette; // indexed entries: the live palette the image is composed with
            uint32_t paletteEntries = 0;
            uint64_t paletteHash = 0;
        };
        std::unordered_map<uint64_t, Pending> m_replacements;
        Vulkan::BufferHandle m_readback;
        Vulkan::ImageHandle m_shrunk;
        bool m_gpuDownscale = std::getenv("PS2X_GS_GPU_DOWNSCALE") != nullptr;
        uint32_t m_forcedW = 0, m_forcedH = 0;
        double m_perfGpuPresent = 0;
        double m_perfGpuPrev[int(ParallelGS::TimestampType::Count)] = {};
        VkDeviceSize m_readbackSize = 0;
        bool m_perf = std::getenv("PS2X_GS_PERF") != nullptr;
        std::chrono::steady_clock::time_point m_perfLast{};
        double m_perfVsync = 0, m_perfWait = 0, m_perfCopy = 0, m_perfPeriod = 0, m_perfMax = 0;
        uint32_t m_perfN = 0, m_perfLate = 0, m_perfSpikeLines = 0;
        uint64_t m_perfTotal = 0;
        int m_scale = 1;
        uint32_t m_outputLog2 = 1; // log2 of the high resolution scanout factor (PS2X_GS_OUTPUT)
        bool m_vsyncPresent = std::getenv("PS2X_GS_VSYNC_PRESENT") != nullptr; // experiment: scan out through parallel-gs even at 1x
    };
}

std::unique_ptr<GSRasterBackend> CreateParallelGSBackend()
{
    try
    {
        auto backend = std::make_unique<ParallelGSBackend>();
        gsSetRawTap(static_cast<GSRawTap *>(backend.get()));
        std::fprintf(stderr, "[gs] backend: parallel-gs (Vulkan)\n");
        return backend;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "[gs] parallel-gs (Vulkan) could not start: %s\n", error.what());
        return nullptr;
    }
}
