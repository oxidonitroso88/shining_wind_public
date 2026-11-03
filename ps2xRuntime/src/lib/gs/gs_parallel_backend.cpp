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

#include "ps2_perf.h"
#include "ps2_perf_irq.h"
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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    // PS2X_GS_VKLOG=1: diagnostics for GPUs where the Vulkan driver crashes (see docs/vulkan-amd-startup-crash-2026-10-30.md). Writes to stderr and to
    // %APPDATA%\ShiningWind\vk_diag.log (the release exe has no console), reports which descriptor path Granite chose, keeps the last backend stage in a
    // global and installs a vectored handler that logs the stage, the faulting module+offset and a stack trace on an access violation. Off: nothing changes.
    namespace vkdiag
    {
        bool g_on = false;
        std::FILE *g_file = nullptr;
        std::atomic<const char *> g_stage{"(none)"};
        std::atomic<uint64_t> g_stageCount{0};
        std::atomic<int> g_shaderIndex{-1};          // shader module being created (vkCreateShaderModule wrapper); -1 outside of it
        std::atomic<uint64_t> g_shaderHash{0};
        std::atomic<size_t> g_shaderBytes{0};
        PFN_vkCreateShaderModule g_realCreateShaderModule = nullptr;

        void log(const char *fmt, ...)
        {
            if (!g_on)
                return;
            char line[1024];
            va_list ap;
            va_start(ap, fmt);
            std::vsnprintf(line, sizeof(line), fmt, ap);
            va_end(ap);
            std::fputs(line, stderr);
            if (g_file)
            {
                std::fputs(line, g_file);
                std::fflush(g_file);
            }
        }

        // Records the stage the backend is entering; logs the first time each distinct stage (string literal address) is seen.
        void stage(const char *name)
        {
            if (!g_on)
                return;
            g_stage.store(name, std::memory_order_relaxed);
            g_stageCount.fetch_add(1, std::memory_order_relaxed);
            static std::mutex m;
            static std::vector<const char *> seen;
            std::lock_guard<std::mutex> lock(m);
            if (std::find(seen.begin(), seen.end(), name) != seen.end())
                return;
            seen.push_back(name);
            log("[vkdiag] stage (first time): %s\n", name);
        }

#ifdef _WIN32
        void describeAddress(void *addr, char *out, size_t outSize)
        {
            HMODULE mod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(addr), &mod) && mod)
            {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(mod, path, MAX_PATH);
                const char *base = std::strrchr(path, '\\');
                std::snprintf(out, outSize, "%s+0x%llx", base ? base + 1 : path, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
            }
            else
                std::snprintf(out, outSize, "%p", addr);
        }

        LONG CALLBACK onException(EXCEPTION_POINTERS *info)
        {
            const DWORD code = info->ExceptionRecord->ExceptionCode;
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
                return EXCEPTION_CONTINUE_SEARCH;
            static std::atomic<bool> once{false};
            if (once.exchange(true))
                return EXCEPTION_CONTINUE_SEARCH;
            char where[512];
            describeAddress(info->ExceptionRecord->ExceptionAddress, where, sizeof(where));
            if (g_shaderIndex.load() >= 0)
                log("[vkdiag] EXCEPTION happened INSIDE vkCreateShaderModule: shader #%d, %zu bytes, hash %016llx\n", g_shaderIndex.load(), g_shaderBytes.load(),
                    static_cast<unsigned long long>(g_shaderHash.load()));
            log("[vkdiag] EXCEPTION 0x%08lX at %s, stage '%s' (#%llu), access %s address 0x%llx\n", code, where, g_stage.load(), static_cast<unsigned long long>(g_stageCount.load()),
                info->ExceptionRecord->NumberParameters > 0 ? (info->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC") : "?",
                info->ExceptionRecord->NumberParameters > 1 ? static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]) : 0ull);
            void *frames[32];
            const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < n; ++i)
            {
                describeAddress(frames[i], where, sizeof(where));
                log("[vkdiag]   #%u %s\n", static_cast<unsigned>(i), where);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
#endif

        // Creation order of the shaders of ParallelGS::Shaders (gs/shaders/slangmosh.hpp). Shaders with identical SPIR-V are created once (upscale_vert
        // reuses sharpen_vert), so names after that point are the likely ones; size and hash are the reliable identity.
        const char *const kShaderNames[] = {"binning", "upload[0]", "upload[1]", "clut_write", "vram_copy", "triangle_setup", "single_sample_heuristic", "ubershader[0][0]",
                                            "ubershader[1][0]", "ubershader[0][1]", "ubershader[1][1]", "quad", "sample_circuit[0]", "sample_circuit[1]", "blit_circuit", "weave",
                                            "sharpen_vert (= upscale_vert)", "sharpen_frag", "upscale_frag", "blit", "extwrite", "qword_clear", "sampler_feedback", "ui_vert",
                                            "ui_frag[0][0]", "ui_frag[1][0]", "ui_frag[0][1]", "ui_frag[1][1]"};

        const char *capabilityName(uint32_t c)
        {
            switch (c)
            {
            case 1: return "Shader";
            case 9: return "Float16";
            case 10: return "Float64";
            case 11: return "Int64";
            case 12: return "Int64Atomics";
            case 22: return "Int16";
            case 39: return "Int8";
            case 61: return "GroupNonUniform";
            case 62: return "GroupNonUniformVote";
            case 63: return "GroupNonUniformArithmetic";
            case 64: return "GroupNonUniformBallot";
            case 65: return "GroupNonUniformShuffle";
            case 66: return "GroupNonUniformShuffleRelative";
            case 67: return "GroupNonUniformClustered";
            case 68: return "GroupNonUniformQuad";
            case 4433: return "StorageBuffer16BitAccess";
            case 4434: return "UniformAndStorageBuffer16BitAccess";
            case 4448: return "StorageBuffer8BitAccess";
            case 4449: return "UniformAndStorageBuffer8BitAccess";
            case 5301: return "ShaderNonUniform";
            case 5302: return "RuntimeDescriptorArray";
            case 5345: return "VulkanMemoryModel";
            case 5347: return "PhysicalStorageBufferAddresses";
            default: return "?";
            }
        }

        std::string literalString(const uint32_t *words, uint32_t wordCount)
        {
            const char *s = reinterpret_cast<const char *>(words);
            size_t n = 0;
            while (n < static_cast<size_t>(wordCount) * 4u && s[n])
                ++n;
            return std::string(s, n);
        }

        // Module-level scan of the SPIR-V (bounds checked, the logger must not crash on a bad module): version, capabilities, extensions, entry points, local size.
        void describeSpirv(const uint32_t *code, size_t words)
        {
            if (words < 5 || code[0] != 0x07230203u)
            {
                log("[vkdiag]   NOT valid SPIR-V (magic 0x%08x, %zu words)\n", words ? code[0] : 0u, words);
                return;
            }
            log("[vkdiag]   SPIR-V version %u.%u generator 0x%08x bound %u\n", (code[1] >> 16) & 0xFF, (code[1] >> 8) & 0xFF, code[2], code[3]);
            std::string caps, exts;
            size_t i = 5;
            while (i < words)
            {
                const uint32_t op = code[i] & 0xFFFFu, n = code[i] >> 16;
                if (n == 0 || i + n > words)
                {
                    log("[vkdiag]   malformed instruction at word %zu\n", i);
                    break;
                }
                if (op == 17 && n >= 2) // OpCapability
                    caps += std::to_string(code[i + 1]) + "(" + capabilityName(code[i + 1]) + ") ";
                else if (op == 10 && n >= 2) // OpExtension
                    exts += literalString(&code[i + 1], n - 1) + " ";
                else if (op == 15 && n >= 4) // OpEntryPoint: model, id, name
                    log("[vkdiag]   entry point model %u '%s'\n", code[i + 1], literalString(&code[i + 3], n - 3).c_str());
                else if (op == 16 && n >= 6 && code[i + 2] == 17) // OpExecutionMode LocalSize
                    log("[vkdiag]   LocalSize %u x %u x %u\n", code[i + 3], code[i + 4], code[i + 5]);
                else if (op == 54) // OpFunction: every capability / extension is declared before the first one
                    break;
                i += n;
            }
            log("[vkdiag]   capabilities: %s\n", caps.c_str());
            log("[vkdiag]   extensions: %s\n", exts.empty() ? "(none)" : exts.c_str());
        }

        VKAPI_ATTR VkResult VKAPI_CALL wrappedCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *alloc, VkShaderModule *module)
        {
            static std::atomic<int> counter{0};
            const int index = counter.fetch_add(1);
            uint64_t hash = 1469598103934665603ull; // FNV-1a over the code words
            const size_t words = info->codeSize / 4u;
            for (size_t i = 0; i < words; ++i)
                hash = (hash ^ info->pCode[i]) * 1099511628211ull;
            const char *name = index < static_cast<int>(sizeof(kShaderNames) / sizeof(kShaderNames[0])) ? kShaderNames[index] : "(beyond the known list)";
            log("[vkdiag] vkCreateShaderModule #%d (likely '%s'): %zu bytes, hash %016llx, flags 0x%x, pNext %p\n", index, name, static_cast<size_t>(info->codeSize),
                static_cast<unsigned long long>(hash), static_cast<unsigned>(info->flags), info->pNext);
            describeSpirv(info->pCode, words);
            // PS2X_GS_VKLOG_DUMP_SPIRV=1: the exact SPIR-V handed to the driver, written BEFORE the call (it survives a crash inside it) to
            // %APPDATA%\ShiningWind\spirv\<index>_<hash>.spv, for spirv-val / spirv-dis and for comparing builds.
            if (const char *dump = std::getenv("PS2X_GS_VKLOG_DUMP_SPIRV"); dump && dump[0] != '0')
            {
                if (const char *appdata = std::getenv("APPDATA"))
                {
                    const std::string dir = std::string(appdata) + "\\ShiningWind\\spirv";
                    CreateDirectoryA(dir.c_str(), nullptr);
                    char file[64];
                    std::snprintf(file, sizeof(file), "\\%02d_%016llx.spv", index, static_cast<unsigned long long>(hash));
                    if (std::FILE *f = std::fopen((dir + file).c_str(), "wb"))
                    {
                        std::fwrite(info->pCode, 1, info->codeSize, f);
                        std::fclose(f);
                    }
                }
            }
            g_shaderIndex.store(index);
            g_shaderHash.store(hash);
            g_shaderBytes.store(info->codeSize);
            const VkResult r = g_realCreateShaderModule(device, info, alloc, module);
            g_shaderIndex.store(-1);
            log("[vkdiag]   -> vkCreateShaderModule #%d returned %d, module %p\n", index, static_cast<int>(r), module ? reinterpret_cast<void *>(*module) : nullptr);
            return r;
        }

        // Replaces the entry of the device's function table that Granite's Shader::Shader calls (device_table.vkCreateShaderModule).
        void hookShaderCreation(const Vulkan::Device &dev)
        {
            if (!g_on)
                return;
            auto &table = const_cast<VolkDeviceTable &>(dev.get_device_table());
            if (!table.vkCreateShaderModule)
                return;
            g_realCreateShaderModule = table.vkCreateShaderModule;
            table.vkCreateShaderModule = wrappedCreateShaderModule;
            log("[vkdiag] vkCreateShaderModule is being logged\n");
        }

        void init()
        {
            const char *v = std::getenv("PS2X_GS_VKLOG");
            if (!v || v[0] == '0')
                return;
            g_on = true;
#ifdef _WIN32
            if (const char *appdata = std::getenv("APPDATA"))
            {
                const std::string dir = std::string(appdata) + "\\ShiningWind";
                CreateDirectoryA(dir.c_str(), nullptr);
                g_file = std::fopen((dir + "\\vk_diag.log").c_str(), "w");
            }
            AddVectoredExceptionHandler(1, onException);
#endif
            log("[vkdiag] enabled (log file %s)\n", g_file ? "%APPDATA%\\ShiningWind\\vk_diag.log" : "unavailable");
        }

        // What Granite actually selected on this device (nothing is assumed: the values come from the created device).
        void reportDevice(const Vulkan::Context &ctx, const Vulkan::Device &dev)
        {
            if (!g_on)
                return;
            const VkPhysicalDeviceProperties &p = ctx.get_gpu_props();
            const Vulkan::DeviceFeatures &f = ctx.get_enabled_device_features();
            const Vulkan::DeviceFeatures &d = dev.get_device_features();
            log("[vkdiag] GPU '%s' vendor 0x%04x device 0x%04x driver %u.%u.%u (0x%08x) api %u.%u.%u driverId %d\n", p.deviceName, p.vendorID, p.deviceID, VK_API_VERSION_MAJOR(p.driverVersion),
                VK_API_VERSION_MINOR(p.driverVersion), VK_API_VERSION_PATCH(p.driverVersion), p.driverVersion, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                VK_API_VERSION_PATCH(p.apiVersion), static_cast<int>(f.driver_id));
            log("[vkdiag] descriptor path: descriptorBuffer feature=%d supports_descriptor_buffer=%d descriptorHeap feature=%d supports_buffer_or_heap=%d (device copy: buffer=%d heap_or_buffer=%d)\n",
                f.descriptor_buffer_features.descriptorBuffer, f.supports_descriptor_buffer, f.descriptor_heap_features.descriptorHeap, f.supports_descriptor_buffer_or_heap,
                d.supports_descriptor_buffer, d.supports_descriptor_buffer_or_heap);
            const auto &db = f.descriptor_buffer_properties;
            log("[vkdiag] descriptor buffer props: combinedImageSamplerDescriptorSingleArray=%d samplerDescSize=%zu sampledImageDescSize=%zu maxSamplerRange=%llu maxResourceRange=%llu offsetAlignment=%llu\n",
                db.combinedImageSamplerDescriptorSingleArray, static_cast<size_t>(db.samplerDescriptorSize), static_cast<size_t>(db.sampledImageDescriptorSize),
                static_cast<unsigned long long>(db.maxSamplerDescriptorBufferRange), static_cast<unsigned long long>(db.maxResourceDescriptorBufferRange),
                static_cast<unsigned long long>(db.descriptorBufferOffsetAlignment));
            log("[vkdiag] required by sizeof*512K: sampler %llu, sampledImage %llu bytes\n", static_cast<unsigned long long>(db.samplerDescriptorSize) * 512ull * 1024ull,
                static_cast<unsigned long long>(db.sampledImageDescriptorSize) * 512ull * 1024ull);
            const auto &v12 = f.vk12_features;
            log("[vkdiag] vk1.2: descriptorIndexing=%d runtimeDescriptorArray=%d descriptorBindingPartiallyBound=%d nonUniformIndexing(sampled)=%d bufferDeviceAddress=%d shaderInt8=%d storage8=%d\n",
                v12.descriptorIndexing, v12.runtimeDescriptorArray, v12.descriptorBindingPartiallyBound, v12.shaderSampledImageArrayNonUniformIndexing, v12.bufferDeviceAddress,
                v12.shaderInt8, v12.storageBuffer8BitAccess);
            log("[vkdiag] vk1.3: sync2=%d subgroupSizeControl=%d computeFullSubgroups=%d  feat: shaderInt64=%d shaderInt16=%d storage16=%d\n", f.vk13_features.synchronization2,
                f.vk13_features.subgroupSizeControl, f.vk13_features.computeFullSubgroups, f.enabled_features.shaderInt64, f.enabled_features.shaderInt16,
                f.vk11_features.storageBuffer16BitAccess);
            log("[vkdiag] subgroup: size %u (min %u max %u) supportedStages 0x%x supportedOperations 0x%x; maxComputeSharedMemory %u\n", f.vk11_props.subgroupSize, f.vk13_props.minSubgroupSize,
                f.vk13_props.maxSubgroupSize, f.vk11_props.subgroupSupportedStages, f.vk11_props.subgroupSupportedOperations, p.limits.maxComputeSharedMemorySize);
            log("[vkdiag] enabled device extensions (%u):\n", f.num_device_extensions);
            for (uint32_t i = 0; i < f.num_device_extensions; ++i)
                log("[vkdiag]   %s\n", f.device_extensions[i]);
        }
    }

#define VKDIAG_STAGE(name) vkdiag::stage(name)

    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;
    constexpr uint32_t kMaxImageQwordsPerTag = 0x7FFFu; // GIF NLOOP is 15 bits

    class ParallelGSBackend final : public GSRasterBackend, public GSRawTap, private ParallelGS::SignalInterface, private ParallelGS::TextureReplacer
    {
    public:
        ParallelGSBackend()
        {
            vkdiag::init();
            VKDIAG_STAGE("ctor: init_loader");
            if (!Vulkan::Context::init_loader(nullptr))
                throw std::runtime_error("no Vulkan loader");
            m_context.set_num_thread_indices(1);
            VKDIAG_STAGE("ctor: init_instance_and_device");
            if (!m_context.init_instance_and_device(nullptr, 0, nullptr, 0,
                                                    Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
                                                        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
                                                        Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT))
                throw std::runtime_error("Vulkan device creation failed (a GPU with descriptor indexing, subgroup ops and descriptor buffers is required)");
            VKDIAG_STAGE("ctor: device.set_context");
            m_device.set_context(m_context);
            VKDIAG_STAGE("ctor: device.init_frame_contexts");
            m_device.init_frame_contexts(4);
            vkdiag::reportDevice(m_context, m_device);
            vkdiag::hookShaderCreation(m_device);
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
            VKDIAG_STAGE("ctor: parallel-gs init (pipelines/shaders)");
            if (!m_gs.init(&m_device, options))
                throw std::runtime_error("parallel-gs initialisation failed");
            std::fprintf(stderr, "[gs] internal resolution factor %d (PS2X_GS_SCALE), scanout factor %u (PS2X_GS_OUTPUT)\n", m_scale, 1u << m_outputLog2);
            if (ps2perf::enabled())
            {
                ps2perf::setInfo("scale", std::to_string(m_scale).c_str());
                ps2perf::setInfo("output", m_scale > 1 ? std::to_string(1u << m_outputLog2).c_str() : "n/a(x1)"); // the scanout factor only applies above x1
                ps2perf::setInfo("gpu", (std::string(m_context.get_gpu_props().deviceName) + (m_presentUnlock ? " [present-unlock]" : "")).c_str());
            }
            if (m_presentUnlock)
                std::fprintf(stderr, "[gs] PS2X_GS_PRESENT_UNLOCK=1: the high resolution presentation waits for its own fence without holding the backend mutex\n");
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
            if (std::getenv("PS2X_GS_PERF") || ps2perf::g_level >= 2)
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
            VKDIAG_STAGE("ctor: done");
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
            VKDIAG_STAGE("OnGifPackets: gif_transfer");
            sizeBytes &= ~15u; // whole quadwords only (the frontend ignores a trailing partial tag as well)
            if (!data || !sizeBytes)
                return;
            const ps2perf::Scope perfScope(ps2perf::S::EeTapGif, sizeBytes);
            m_gs.gif_transfer(3, data, sizeBytes);
            m_gpuDirty = true;
        }

        void OnRegister(uint8_t addr, uint64_t value) override
        {
            Lock lock(m_mutex);
            // 0x58 and up are privileged/alias registers (DISPFB, DISPLAY, SIGNAL, ...): they never draw and are handled by the frontend.
            if (addr >= 0x58)
                return;
            VKDIAG_STAGE("OnRegister: write_register");
            const ps2perf::Scope perfScope(ps2perf::S::EeTapRegister);
            m_gs.write_register(static_cast<ParallelGS::RegisterAddr>(addr), value);
            m_gpuDirty = true;
        }

        void OnImageUpload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t trxdir, const uint8_t *data, uint32_t sizeBytes) override
        {
            Lock lock(m_mutex);
            if (!data || !sizeBytes)
                return;
            VKDIAG_STAGE("OnImageUpload: write_register/gif_transfer");
            const ps2perf::Scope perfScope(ps2perf::S::EeTapImage, sizeBytes);
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
            VKDIAG_STAGE("Initialize: pushHostVram");
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
            VKDIAG_STAGE("Flush: gs.flush");
            m_gs.flush();
        }

        void Sync(GSSyncReason reason) override
        {
            Lock lock(m_mutex);
            VKDIAG_STAGE("Sync: gs.flush");
            m_gs.flush();
            if (reason == GSSyncReason::Reset)
                m_device.wait_idle();
        }

        PresentationFrame Present(const GSPresentationRequest &request) override
        {
            std::unique_lock<std::mutex> presentSerial(m_presentSerial, std::defer_lock);
            if (m_presentUnlock)
                presentSerial.lock();
            Lock lock(m_mutex, ps2perf::S::MainPresentLock);
            const ps2perf::Scope perfScope(ps2perf::S::MainBackendPresent);
            VKDIAG_STAGE("Present");
            if (m_hdStats && (++m_presents % 600u) == 0u)
                reportHd();
            if (m_scale > 1 || m_vsyncPresent)
            {
                PresentationFrame high = presentHighResolution(request);
                collectGpuTimes();
                collectFlushStats();
                if (high)
                    return high;
            }
            gs_present_info::setLogicalSize(0u, 0u);
            mirrorHostVram();
            const ps2perf::Scope perfCpu(ps2perf::S::MainCpuPresent);
            PresentationFrame low = m_cpu.Present(request);
            collectGpuTimes();
            collectFlushStats();
            return low;
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
            ps2perf::TimedGuard<std::recursive_mutex> guard;
            explicit Lock(std::recursive_mutex &m, ps2perf::S waitStage = ps2perf::S::EeTapLock) : guard(m, waitStage) { Util::register_thread_index(0); }
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
            VKDIAG_STAGE("presentHighResolution: gs.vsync");
            ParallelGS::ScanoutResult result;
            {
                const ps2perf::Scope perfVsync(ps2perf::S::MainVsync);
                result = m_gs.vsync(info);
            }
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
            VKDIAG_STAGE("presentHighResolution: readback command buffer");
            auto cmd = m_device.request_command_buffer();
            Vulkan::QueryPoolHandle tsStart, tsEnd;
            if (m_gpuTs)
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
            if (m_gpuTs)
                tsEnd = cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            // PS2X_GS_PRESENT_UNLOCK: Present() holds m_mutex exactly once here (it is only called by the presenter, never from inside a backend call), so a
            // single unlock really releases it. Only this thread's own objects (readback buffer, result.image, the fence) are touched while it is released;
            // nothing that needs Granite's thread index 0 or parallel-gs state (map_host_buffer and Fence::wait take no command buffer / pool).
            const bool unlocked = m_presentUnlock;
            if (unlocked)
            {
                Vulkan::Fence fence;
                m_device.submit(cmd, &fence); // submit with a fence flushes the queue: the fence is valid and signals when the read-back is done
                m_mutex.unlock();
                {
                    const ps2perf::Scope perfWait(ps2perf::S::MainWaitIdle);
                    fence->wait();
                }
            }
            else
            {
                const ps2perf::Scope perfWait(ps2perf::S::MainWaitIdle);
                m_device.submit(cmd);
                m_device.wait_idle();
            }
            // the mutex is taken again before leaving (the caller's Lock unlocks it), also on the early return below
            struct Relock
            {
                ParallelGSBackend *self;
                bool active;
                void now()
                {
                    if (active)
                    {
                        self->m_mutex.lock();
                        Util::register_thread_index(0);
                        active = false;
                    }
                }
                ~Relock() { now(); }
            } relock{this, unlocked};
            if (!unlocked && m_gpuTs && tsStart && tsEnd && tsStart->is_signalled() && tsEnd->is_signalled())
            {
                const double presentSeconds = m_device.convert_device_timestamp_delta(tsStart->get_timestamp_ticks(), tsEnd->get_timestamp_ticks());
                m_perfGpuPresent += presentSeconds * 1000.0;
                if (ps2perf::enabled())
                    ps2perf::add(ps2perf::S::GpuPresentCopy, static_cast<uint64_t>(presentSeconds * 1.0e9));
            }
            const uint8_t *src = static_cast<const uint8_t *>(m_device.map_host_buffer(*readback, Vulkan::MEMORY_ACCESS_READ_BIT));
            if (!src)
                return frame;
            const auto perfT2 = std::chrono::steady_clock::now();
            // one pass: copy and force the alpha channel opaque (32-bit words, little endian: A is the top byte)
            frame.pixels.resize(static_cast<size_t>(w) * h * 4u);
            {
                const ps2perf::Scope perfCopy(ps2perf::S::MainHostCopy, static_cast<uint64_t>(w) * h * 4u);
                const size_t words = static_cast<size_t>(w) * h;
                uint32_t *dst = reinterpret_cast<uint32_t *>(frame.pixels.data());
                for (size_t i = 0; i < words; ++i)
                {
                    uint32_t px;
                    std::memcpy(&px, src + i * 4u, 4u);
                    dst[i] = px | 0xFF000000u;
                }
            }
            relock.now(); // the code below uses parallel-gs state again
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

        // PS2X_PERF (any level, 0016): work the renderer flushed since the previous present (primitives, render passes, palette updates, copies). Called with
        // m_mutex held (Present re-takes it before returning), the same lock the EE holds while it feeds the renderer, so the stats are not read concurrently.
        void collectFlushStats()
        {
            if (!ps2perf::enabled())
                return;
            const ParallelGS::FlushStats fs = m_gs.consume_flush_stats();
            auto &g = ps2perf::g_gsFlush;
            g.primitives.fetch_add(fs.num_primitives, std::memory_order_relaxed);
            g.renderPasses.fetch_add(fs.num_render_passes, std::memory_order_relaxed);
            g.paletteUpdates.fetch_add(fs.num_palette_updates, std::memory_order_relaxed);
            g.copies.fetch_add(fs.num_copies, std::memory_order_relaxed);
            g.copyBarriers.fetch_add(fs.num_copy_barriers, std::memory_order_relaxed);
            g.presents.fetch_add(1, std::memory_order_relaxed);
        }

        // PS2X_PERF=2: GPU time spent per renderer stage since the previous call, from parallel-gs' timestamp queries (executed GPU time, not waiting).
        void collectGpuTimes()
        {
            if (!ps2perf::enabled() || ps2perf::g_level < 2)
                return;
            static const struct { ParallelGS::TimestampType type; ps2perf::S stage; } map[] = {
                {ParallelGS::TimestampType::Shading, ps2perf::S::GpuShading},        {ParallelGS::TimestampType::Binning, ps2perf::S::GpuBinning},
                {ParallelGS::TimestampType::TriangleSetup, ps2perf::S::GpuSetup},    {ParallelGS::TimestampType::VSync, ps2perf::S::GpuVsync},
                {ParallelGS::TimestampType::TextureUpload, ps2perf::S::GpuTexUpload}, {ParallelGS::TimestampType::CopyVRAM, ps2perf::S::GpuCopyVram},
                {ParallelGS::TimestampType::Readback, ps2perf::S::GpuReadback}};
            for (const auto &m : map)
            {
                const double now = m_gs.get_accumulated_timestamps(m.type);
                const double d = now - m_perfGpuPrev2[int(m.type)];
                m_perfGpuPrev2[int(m.type)] = now;
                if (d > 0.0)
                    ps2perf::add(m.stage, static_cast<uint64_t>(d * 1.0e9));
            }
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
            VKDIAG_STAGE("mirrorHostVram: flush + map_vram_read");
            const ps2perf::Scope perfScope(ps2perf::S::MainMirror, m_vramSize);
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
            VKDIAG_STAGE("pushHostVram: map_vram_read/map_vram_write/end_vram_write");
            const ps2perf::Scope perfScope(ps2perf::S::PushVram, m_vramSize);
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
        bool m_gpuTs = std::getenv("PS2X_GS_PERF") != nullptr || ps2perf::g_level >= 2; // GPU timestamp queries around the presentation copy
        double m_perfGpuPrev2[int(ParallelGS::TimestampType::Count)] = {};
        std::chrono::steady_clock::time_point m_perfLast{};
        double m_perfVsync = 0, m_perfWait = 0, m_perfCopy = 0, m_perfPeriod = 0, m_perfMax = 0;
        uint32_t m_perfN = 0, m_perfLate = 0, m_perfSpikeLines = 0;
        uint64_t m_perfTotal = 0;
        int m_scale = 1;
        uint32_t m_outputLog2 = 1; // log2 of the high resolution scanout factor (PS2X_GS_OUTPUT)
        bool m_vsyncPresent = std::getenv("PS2X_GS_VSYNC_PRESENT") != nullptr; // experiment: scan out through parallel-gs even at 1x
        // PS2X_GS_PRESENT_UNLOCK=1 (experiment, off by default): the high resolution presentation submits its read-back with a fence and waits for THAT fence
        // with m_mutex released, instead of wait_idle() under the mutex. m_presentSerial keeps two presenters (window loop, frame dump) from interleaving.
        bool m_presentUnlock = []() { const char *v = std::getenv("PS2X_GS_PRESENT_UNLOCK"); return v && *v && *v != '0'; }();
        std::mutex m_presentSerial;
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
