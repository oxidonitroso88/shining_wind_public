// See include/extras/extras_gate.h.
#include "extras/extras_gate.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/pad_input.h"
#include "ps2x/iop/audio_output.h"

#include <atomic>
#include <cstdio>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace pc_extras::gate
{
    namespace
    {
        std::atomic<unsigned> g_reasons{0u};
        PS2Runtime *g_runtime = nullptr;

        void apply(bool on)
        {
            std::fprintf(stderr, "[extras] game input blocked, audio muted, emulation %s (VBlank %llu)\n", on ? "paused" : "resumed", g_ps2xGsVblank.load(std::memory_order_relaxed)); std::fflush(stderr);
            padinput::global().setGameInputBlocked(on);
            ps2x::iop::setAudioMuted(on);
            if (g_runtime != nullptr)
                g_runtime->eeScheduler().setUserPause(on);
        }
    }

    void bind(PS2Runtime *runtime) { g_runtime = runtime; }

    void set(Reason reason, bool on)
    {
        const unsigned before = g_reasons.load();
        const unsigned after = on ? (before | reason) : (before & ~static_cast<unsigned>(reason));
        g_reasons.store(after);
        if ((before != 0u) != (after != 0u))
            apply(after != 0u);
    }

    bool active() { return g_reasons.load() != 0u; }
    bool has(Reason reason) { return (g_reasons.load() & reason) != 0u; }
}
