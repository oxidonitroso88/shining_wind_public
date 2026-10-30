// See include/ps2_soul_session.h.
#include "ps2_soul_session.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_session_policy.h"
#include "extras/extras_host.h"
#include "extras/session_config.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace pc_extras::soul_session
{
    namespace
    {
        // The 55 events: even labels 2000..2108 of the town scripts (docs/soultree-bytecode-and-launch-2026-10-24.md, extracted by tools/script/soul_events.py).
        // Display names are the game's own romanisation (file names pac_souldoor_<id>NN); the Japanese names are in docs/soultree-door-names-research-2026-10-24.md.
        constexpr Character kCharacters[] = {
            {"seena", "Seena", 2070, 64, 5},     {"clala", "Clala", 2010, 62, 5},   {"houmei", "Houmei", 2020, 69, 5},
            {"caris", "Caris", 2000, 61, 5},     {"kureha", "Kureha", 2050, 63, 5}, {"jincrow", "Jincrow", 2040, 65, 5},
            {"xecty", "Xecty", 2080, 66, 5},     {"rouen", "Rouen", 2060, 70, 5},   {"hyouun", "Hyouun", 2030, 68, 5},
            {"xero", "Xero", 2090, 67, 5},       {"souma", "Souma", 2104, 71, 1},   {"trihard", "Trihard", 2100, 72, 2},
            {"hilda", "Hilda", 2106, 73, 2},
        };

        Request g_request;
        bool g_requested = false;

        // Guest addresses of this build of the game (SLPM_666.71). Each one is checked against a fingerprint of its first instructions before anything is hooked.
        constexpr uint32_t kNativeTable = 0x3CFD90u, kGlobalVars = 0x448BE0u;
        constexpr uint32_t kManagerUpdateFn = 0x1E56D0u, kRequestEventFn = 0x1E54F0u, kLabelLookupFn = 0x1EA390u;
        constexpr uint32_t kModeUpdateFn = 0x1B0AE0u, kSceneLoadFn = 0x259680u;
        constexpr uint32_t kNativeSpiritDoorOpen = 138, kNativeOpenSoulDoor = 205, kNativeLoadEventPack = 174;
                constexpr uint32_t kTown40 = 40u;              // town40_01: no prologue event
        constexpr unsigned long long kLaunchVblank = 900; // scene up and free (about 17 s after the start)

        PS2Runtime::RecompiledFunction g_managerUpdateOriginal = nullptr, g_requestEventOriginal = nullptr, g_labelLookupOriginal = nullptr;
        PS2Runtime::RecompiledFunction g_modeUpdateOriginal = nullptr, g_sceneLoadOriginal = nullptr;
        PS2Runtime::RecompiledFunction g_nativeOriginal[3] = {};
        uint32_t g_nativeAddress[3] = {};
        uint32_t g_vm = 0;
        bool g_launched = false;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            if (a + 4u <= 0x2000000u)
                std::memcpy(&v, rdram + a, 4u);
            return v;
        }

        void wr32(uint8_t *rdram, uint32_t a, uint32_t v)
        {
            if (a + 4u <= 0x2000000u)
                std::memcpy(rdram + a, &v, 4u);
        }

        // Boot: opening movie (mode 6) -> new game with default data (7) -> field scene (8). No title, no pad, no memory card needed.
        void modeUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t mgr = getRegU32(ctx, 4);
            const uint32_t mode = rd32(rdram, mgr + 0x68u);
            if (mode == 6u || mode == 7u)
            {
                wr32(rdram, mgr + 0x68u, mode + 1u);
                std::fprintf(stderr, "[soul] vb=%llu mode %u -> %u\n", g_ps2xGsVblank.load(std::memory_order_relaxed), mode, mode + 1u);
            }
            ctx->pc = kModeUpdateFn;
            g_modeUpdateOriginal(rdram, ctx, runtime);
        }

        // Every lookup of the map follows scene+0xB4, so replacing the id here loads town40_01 whatever the new-game defaults say.
        void sceneLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            g_vm = 0u; // the town script's VM is learnt from its first LoadEventPack
            std::fprintf(stderr, "[soul] vb=%llu scene load\n", g_ps2xGsVblank.load(std::memory_order_relaxed));
            SET_GPR_U32(ctx, 5, kTown40);
            ctx->pc = kSceneLoadFn;
            g_sceneLoadOriginal(rdram, ctx, runtime);
        }

        // Once the scene is free (manager state 0), ask the engine's own RequestEvent for the event's label.
        void managerUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t mgr = getRegU32(ctx, 4);
            static unsigned long long lastLog = 0;
            if (!g_launched && mgr == g_vm && g_ps2xGsVblank.load(std::memory_order_relaxed) >= lastLog + 300)
            {
                lastLog = g_ps2xGsVblank.load(std::memory_order_relaxed);
                std::fprintf(stderr, "[soul] vb=%llu waiting: manager state %u\n", lastLog, rd32(rdram, mgr + 0xC34u));
            }
            if (!g_launched && mgr == g_vm && g_vm != 0u && g_ps2xGsVblank.load(std::memory_order_relaxed) >= kLaunchVblank &&
                rd32(rdram, mgr + 0xC34u) == 0u)
            {
                wr32(rdram, kGlobalVars + 4u * g_request.variable, g_request.value);
                R5900Context sub = *ctx;
                SET_GPR_U32(&sub, 4, mgr);
                SET_GPR_U32(&sub, 5, g_request.label);
                SET_GPR_U32(&sub, 6, 0u);
                sub.pc = kRequestEventFn;
                g_requestEventOriginal(rdram, &sub, runtime);
                g_launched = true;
                std::fprintf(stderr, "[soul] event started: label %u, G[%u]=%u\n", g_request.label, g_request.variable, g_request.value);
                std::fprintf(stderr, "%s\n", pc_extras::session_config::describe("session").c_str());
            }
            ctx->pc = kManagerUpdateFn;
            g_managerUpdateOriginal(rdram, ctx, runtime);
        }

        // Every Soul Tree routine ends with goto_label <label + 1>: the content of the event is over. The process exits right there, so neither the
        // game's own tail (SetCharInfo, ChangeMap, ...) nor anything else the event could have changed outlives the event.
        void labelLookup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_launched && getRegU32(ctx, 4) == g_vm && getRegU32(ctx, 5) == g_request.label + 1u)
            {
                std::fprintf(stderr, "[soul] event finished\n");
                std::fflush(nullptr);
                std::_Exit(kExitEventFinished);
            }
            ctx->pc = kLabelLookupFn;
            g_labelLookupOriginal(rdram, ctx, runtime);
        }

        // SpiritDoorOpen (138) acts only on a door object in state 2 with +0x124 set (what the solved puzzle leaves behind).
        // OpenSoulDoor (205) is replaced: no puzzle UI, the script gets "door opened" through the manager's own wait state (0x30).
        template <int N>
        void nativeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t vm = getRegU32(ctx, 4);
            if (N == 2)
            {
                if (g_vm == 0u)
                {
                    g_vm = vm;
                    std::fprintf(stderr, "[soul] script vm %06x\n", vm); // the first VM that loads an event pack after the scene started is the town script's
                }
            }
            else if (N == 0)
            {
                const uint32_t door = rd32(rdram, vm + 0xC90u);
                if (door >= 0x100000u && door + 0x130u <= 0x2000000u)
                {
                    wr32(rdram, door + 0x104u, 2u);
                    wr32(rdram, door + 0x124u, 1u);
                }
            }
            else
            {
                const uint32_t ui = rd32(rdram, vm + 0xC64u);
                if (ui >= 0x100000u && ui + 0x2A40u <= 0x2000000u)
                {
                    const uint32_t stack = rd32(rdram, vm + 0x20u), argBase = rd32(rdram, vm + 0x24u);
                    wr32(rdram, ui + 0x2A24u, rd32(rdram, stack + 4u * argBase));
                    wr32(rdram, ui + 0x2A28u, rd32(rdram, stack + 4u * (argBase + 1u)));
                    wr32(rdram, ui + 0x2A2Cu, 1u);
                    wr32(rdram, vm + 0xC34u, 0x30u);
                }
                return;
            }
            ctx->pc = g_nativeAddress[N];
            g_nativeOriginal[N](rdram, ctx, runtime);
        }

        uint32_t fingerprint(const uint8_t *rdram, uint32_t address)
        {
            uint32_t h = 2166136261u;
            for (uint32_t i = 0; i < 32u; ++i)
                h = (h ^ rdram[address + i]) * 16777619u;
            return h;
        }

        struct Expected
        {
            uint32_t address, hash;
        };
        // FNV-1a of the first 32 bytes of each function of the original SLPM_666.71 (checked before installing anything).
        constexpr Expected kExpected[] = {
            {kManagerUpdateFn, 0x6b5fedc3u}, {kRequestEventFn, 0x54d4c9d7u}, {kLabelLookupFn, 0x3fb2aef3u}, {kModeUpdateFn, 0x5b1d3cb0u}, {kSceneLoadFn, 0x56688dacu},
        };
    }

    const Character *characters(size_t &count)
    {
        count = sizeof(kCharacters) / sizeof(kCharacters[0]);
        return kCharacters;
    }

    const Character *findCharacter(std::string_view id)
    {
        for (const Character &c : kCharacters)
            if (id == c.id)
                return &c;
        return nullptr;
    }

    std::string doorTitle(int door)
    {
        static const char *const ordinals[] = {"First", "Second", "Third", "Fourth", "Fifth"};
        if (door < 1 || door > 5)
            return {};
        return std::string(ordinals[door - 1]) + " Door of the Heart";
    }

    bool parseRequest(std::string_view text, Request &out)
    {
        const size_t colon = text.find(':');
        if (colon == std::string_view::npos)
            return false;
        const Character *c = findCharacter(text.substr(0, colon));
        const std::string number(text.substr(colon + 1));
        char *end = nullptr;
        const long door = std::strtol(number.c_str(), &end, 10);
        if (c == nullptr || end == number.c_str() || *end != '\0' || door < 1 || door > c->doors)
            return false;
        out.label = c->firstLabel + 2u * static_cast<uint32_t>(door - 1);
        out.variable = c->doorVariable;
        out.value = static_cast<uint32_t>(door - 1);
        return true;
    }

    void setRequest(const Request &request)
    {
        g_request = request;
        g_requested = true;
        ps2x::session::setReadOnly(); // from this point on nothing of this process can reach the player's data
    }

    bool requested() { return g_requested; }
    bool eventStarted() { return g_launched; }

    void installIfRequested(PS2Runtime &runtime)
    {
        if (!g_requested)
            return;
        const uint8_t *rdram = runtime.memory().getRDRAM();
        for (const Expected &e : kExpected)
        {
            const uint32_t got = fingerprint(rdram, e.address);
            if (e.hash != 0u && got != e.hash)
            {
                std::fprintf(stderr, "[soul] this executable is not the one the viewer was written for (function %06x: %08x, expected %08x): viewer disabled\n",
                             e.address, got, e.hash);
                std::fflush(nullptr);
                std::_Exit(2);
            }
            if (e.hash == 0u)
                std::fprintf(stderr, "[soul] fingerprint %06x = %08x\n", e.address, got);
        }
        g_nativeAddress[0] = rd32(rdram, kNativeTable + 4u * kNativeSpiritDoorOpen);
        g_nativeAddress[1] = rd32(rdram, kNativeTable + 4u * kNativeOpenSoulDoor);
        g_managerUpdateOriginal = runtime.lookupFunction(kManagerUpdateFn);
        g_requestEventOriginal = runtime.lookupFunction(kRequestEventFn);
        g_labelLookupOriginal = runtime.lookupFunction(kLabelLookupFn);
        g_modeUpdateOriginal = runtime.lookupFunction(kModeUpdateFn);
        g_sceneLoadOriginal = runtime.lookupFunction(kSceneLoadFn);
        g_nativeAddress[2] = rd32(rdram, kNativeTable + 4u * kNativeLoadEventPack);
        g_nativeOriginal[2] = runtime.lookupFunction(g_nativeAddress[2]);
        g_nativeOriginal[0] = runtime.lookupFunction(g_nativeAddress[0]);
        g_nativeOriginal[1] = runtime.lookupFunction(g_nativeAddress[1]);
        const bool ok = g_managerUpdateOriginal && g_requestEventOriginal && g_labelLookupOriginal && g_modeUpdateOriginal && g_sceneLoadOriginal &&
                        g_nativeOriginal[0] && g_nativeOriginal[1] && g_nativeOriginal[2] && runtime.replaceFunction(kManagerUpdateFn, &managerUpdate) &&
                        runtime.replaceFunction(kLabelLookupFn, &labelLookup) && runtime.replaceFunction(kModeUpdateFn, &modeUpdate) &&
                        runtime.replaceFunction(kSceneLoadFn, &sceneLoad) && runtime.replaceFunction(g_nativeAddress[0], &nativeHook<0>) &&
                        runtime.replaceFunction(g_nativeAddress[1], &nativeHook<1>) && runtime.replaceFunction(g_nativeAddress[2], &nativeHook<2>);
        if (!ok)
        {
            std::fprintf(stderr, "[soul] could not install the session hooks: viewer disabled\n");
            std::fflush(nullptr);
            std::_Exit(2);
        }
        std::string title = "Soul Tree Events";
        for (const Character &c : kCharacters)
            if (c.doorVariable == g_request.variable)
                title = std::string(c.display) + " - " + doorTitle(static_cast<int>(g_request.value) + 1);
        pc_extras::host::enterSessionMode(title, &eventStarted); // the child shows "loading" until the event starts and handles "stop"
        std::fprintf(stderr, "[soul] session installed: label %u\n", g_request.label);
    }
}
