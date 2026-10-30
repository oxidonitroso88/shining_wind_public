#include "game_overrides.h"
#include "ps2_runtime.h"
#include "Kernel/Stubs/Common.h"
#include "ps2_call_trace.h"
#include "ps2_text_pitch.h"
#include "ps2_story_text.h"
#include "ps2_script_trace.h"
#include "ps2_soul_session.h"
#include "ps2x/iop/mod_overlay.h"
#include "ps2_host_arena.h"
#include "runtime/gs/tex_provenance.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace
{
    constexpr uint32_t kPathHookAddress = 0x0012A940u;

    PS2Runtime::RecompiledFunction g_originalPathHook = nullptr;

    std::string readGuestString(uint8_t *rdram, uint32_t address)
    {
        std::string result;

        if (address == 0)
            return result;

        // Sólo necesitamos nombres/rutas cortas.
        constexpr size_t kMaxLength = 1024;

        for (size_t i = 0; i < kMaxLength; ++i)
        {
            const char c = static_cast<char>(rdram[address + i]);

            if (c == '\0')
                break;

            result.push_back(c);
        }

        return result;
    }

    void hookPathCreated(
        uint8_t *rdram,
        R5900Context *ctx,
        PS2Runtime *runtime)
    {
        // Estamos entrando exactamente en 0x12A940.
        //
        // 0012A8E8 conserva:
        //   $v0 = handle recién creado
        //   $s1 = path
        //
        // En R5900:
        //   v0 = registro 2
        //   s1 = registro 17

        const uint32_t handle = getRegU32(ctx, 2);       // $v0
        const uint32_t pathAddress = getRegU32(ctx, 17); // $s1

        const std::string path =
            readGuestString(rdram, pathAddress);

        std::cout
            << "[ShiningWind] OPEN vb=" << g_ps2xGsVblank.load(std::memory_order_relaxed) << " "
            << "handle=0x"
            << std::hex << std::setw(8) << std::setfill('0')
            << handle
            << " path=0x"
            << std::setw(8)
            << pathAddress
            << std::dec
            << " \"" << path << "\""
            << std::endl;

        // Importantísimo:
        // dejamos PC en 0x12A940 para que el handler original
        // reentre exactamente en label_12a940 y continúe normalmente.
        ctx->pc = kPathHookAddress;

        if (g_originalPathHook)
        {
            g_originalPathHook(rdram, ctx, runtime);
        }
    }

    // The game carries a static copy of the ROOT directory of ST.CVM in its own executable: 1515 records of 0x30 bytes
    // {u32 size, u32 0, u32 lsn, 2 bytes, name[34]} starting with "." and ".." (verified against the container: 1503 of 1515
    // match file for file, the other 12 are directories). The game does not read the root directory records from the disc, so
    // replaced root files need this table patched with the same size / extent the virtual disc serves (mod_overlay.h).
    // Files in sub-directories are found through ISO directory sectors, which the virtual disc patches itself.
    constexpr uint32_t kRootTableAddress = 0x003A8B48u;
    constexpr uint32_t kRootTableStride = 0x30u;
    constexpr uint32_t kRootTableMaxEntries = 4096u;

    bool rootTableEntryValid(const uint8_t *rdram, uint32_t address, std::string *name)
    {
        uint32_t pad = 0u, lsn = 0u;
        std::memcpy(&pad, rdram + address + 4u, 4u);
        std::memcpy(&lsn, rdram + address + 8u, 4u);
        std::string text(reinterpret_cast<const char *>(rdram + address + 14u), strnlen(reinterpret_cast<const char *>(rdram + address + 14u), 34u));
        if (pad != 0u || lsn == 0u || lsn > 4000000u || text.empty())
            return false;
        for (const char c : text)
            if (c < 0x20 || c > 0x7e)
                return false;
        if (name != nullptr)
            *name = text;
        return true;
    }

    void applyResourceTablePatches(PS2Runtime &runtime)
    {
        namespace mods = ps2x::iop::mods;
        const std::string folder = mods::resolveModFolder();
        if (folder.empty())
            return;
        mods::ModRoot root;
        if (!root.open(folder, nullptr))
            return;
        const auto *files = root.files("ST");
        if (files == nullptr)
            return;

        const std::filesystem::path container = PS2Runtime::getIoPaths().cdRoot / "ST.CVM";
        std::error_code error;
        const uint64_t containerSize = std::filesystem::file_size(container, error);
        if (error)
            return;
        std::ifstream stream(container, std::ios::binary);
        auto reader = [&stream](uint64_t offset, void *destination, size_t size)
        {
            stream.clear();
            stream.seekg(static_cast<std::streamoff>(offset));
            stream.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
            return static_cast<size_t>(stream.gcount()) == size;
        };
        // Same inputs as the virtual disc => the same plan (extents and sizes) without sharing state between EE and IOP.
        const auto overlay = mods::CvmOverlay::build("ST", containerSize, reader, *files, nullptr);
        if (!overlay)
            return;

        uint8_t *rdram = runtime.memory().getRDRAM();
        std::string first;
        if (!rootTableEntryValid(rdram, kRootTableAddress, &first) || first != ".")
        {
            std::cerr << "[mods] the root file table of the game was not found at 0x" << std::hex << kRootTableAddress << std::dec
                      << ": root files of ST.CVM cannot be replaced" << std::endl;
            return;
        }
        std::map<std::string, uint32_t> byName;
        for (uint32_t i = 0; i < kRootTableMaxEntries; ++i)
        {
            const uint32_t address = kRootTableAddress + i * kRootTableStride;
            std::string name;
            if (!rootTableEntryValid(rdram, address, &name))
                break;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (const size_t version = name.rfind(";1"); version != std::string::npos && version + 2 == name.size())
                name.resize(version);
            byName.emplace(name, address);
        }
        for (const auto &replacement : overlay->replacements())
        {
            if (replacement.path.find('/') != std::string::npos)
                continue; // sub-directory file: served by the patched ISO directory sectors
            const auto found = byName.find(replacement.path);
            if (found == byName.end())
            {
                std::cerr << "[mods] ST/" << replacement.path << ": not in the game's root file table, not replaced" << std::endl;
                continue;
            }
            uint32_t oldSize = 0u, oldLsn = 0u;
            std::memcpy(&oldSize, rdram + found->second, 4u);
            std::memcpy(&oldLsn, rdram + found->second + 8u, 4u);
            std::memcpy(rdram + found->second, &replacement.size, 4u);
            std::memcpy(rdram + found->second + 8u, &replacement.lsn, 4u);
            std::cout << "[mods] game file table: ST/" << replacement.path << " size " << oldSize << " -> " << replacement.size << ", lsn "
                      << oldLsn << " -> " << replacement.lsn << std::endl;
        }
    }

    void applyShiningWind(PS2Runtime &runtime)
    {
        // The game owns every byte above _end (its allocator spans [_end, 0x1EFE000)): the host heap (RPC staging buffers etc.)
        // must not live there. 0x80000-0xFFFFF is below the ELF and unused by the game.
        ps2_host_arena::pin(runtime, 0x00080000u, 0x00100000u);
        ps2_call_trace::installFromEnvironment(runtime); // PS2X_TRACE_CALLS (diagnostics, off by default)
        ps2_text_pitch::installFromEnvironment(runtime); // ASCII glyphs always proportional (docs/text-pitch-ascii-2026-10-17.md)
        ps2_story_text::installFromEnvironment(runtime); // English story lines (PS2X_STORY_EN, docs/translation-story-bss-2026-10-17.md)
        ps2_script_trace::installFromEnvironment(runtime); // PS2X_SCRIPT_TRACE (script VM natives, diagnostics, off by default)
        pc_extras::soul_session::installIfRequested(runtime); // --soul-event=<character>:<door> (PC Extras Soul Tree viewer session, off without the argument)
        applyResourceTablePatches(runtime);              // PS2X_MODS (resource replacement, off without a mod folder)
        texprov::install(runtime, runtime.memory().getRDRAM(), PS2_RAM_SIZE); // PS2X_TEXMAP (texture provenance, off by default)
        const uint32_t slot =
            (kPathHookAddress -
             g_ps2RecompiledFunctionTableBase) >> 2;

        if (slot >= g_ps2RecompiledFunctionTableSlotCount)
        {
            std::cerr
                << "[ShiningWind] ERROR: hook address outside function table."
                << std::endl;
            return;
        }

        g_originalPathHook =
            g_ps2RecompiledFunctionTable[slot];

        if (!g_originalPathHook)
        {
            std::cerr
                << "[ShiningWind] ERROR: no original handler at 0x"
                << std::hex << kPathHookAddress
                << std::dec << std::endl;
            return;
        }

        if (!runtime.replaceFunction(
                kPathHookAddress,
                &hookPathCreated))
        {
            std::cerr
                << "[ShiningWind] ERROR: could not install path hook."
                << std::endl;
            return;
        }

        std::cout
            << "[ShiningWind] path hook installed at 0x"
            << std::hex << kPathHookAddress
            << std::dec << std::endl;
    }
}

void ps2_shining_wind_overrides_link_anchor() {}

PS2_REGISTER_GAME_OVERRIDE(
    "Shining Wind",
    "SLPM_666.71",
    0u,
    0u,
    applyShiningWind
);