// See include/ps2_script_trace.h.
#include "ps2_script_trace.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

extern std::atomic<unsigned long long> g_ps2xGsVblank;

namespace
{
    constexpr uint32_t kNativeTable = 0x3CFD90u;
    constexpr uint32_t kCodeLo = 0x100000u, kCodeHi = 0x352200u;
    constexpr size_t kMaxNatives = 320;

    PS2Runtime::RecompiledFunction g_original[kMaxNatives] = {};
    uint32_t g_address[kMaxNatives] = {};
    size_t g_count = 0;
    std::FILE *g_out = nullptr;
    unsigned long long g_from = 0, g_to = ~0ull, g_max = 300000, g_lines = 0;
    std::mutex g_mutex;
    uint32_t rd32(const uint8_t *rdram, uint32_t a)
    {
        uint32_t v = 0;
        if (a + 4u <= 0x2000000u)
            std::memcpy(&v, rdram + a, 4u);
        return v;
    }

    std::string pretty(const uint8_t *rdram, uint32_t pool, uint32_t word)
    {
        auto text = [&](uint32_t address) -> std::string {
            if (address < 0x100000u || address >= 0x1FFFF00u)
                return {};
            const uint8_t *s = rdram + address;
            std::string out;
            for (int i = 0; i < 64 && s[i] != 0; ++i)
            {
                if (s[i] < 0x20 || s[i] > 0x7e)
                    return {};
                out.push_back(static_cast<char>(s[i]));
            }
            return out.size() >= 3 ? out : std::string();
        };
        std::string out = text(word);
        if (out.empty() && pool != 0u && word < 0x400000u)
            out = text(pool + word);
        return out;
    }

    void traced(size_t index, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const unsigned long long vb = g_ps2xGsVblank.load(std::memory_order_relaxed);
        const bool log = g_out != nullptr && vb >= g_from && vb <= g_to && g_lines < g_max;
        const uint32_t vm = getRegU32(ctx, 4);
        const uint32_t base = rd32(rdram, vm + 8u), pool = rd32(rdram, vm + 0x14u), pc0 = rd32(rdram, vm + 0x1Cu);
        const uint32_t stack = rd32(rdram, vm + 0x20u), argBase = rd32(rdram, vm + 0x24u);
        uint32_t args[6] = {};
        if (log)
            for (int i = 0; i < 6; ++i)
                args[i] = rd32(rdram, stack + 4u * (argBase + static_cast<uint32_t>(i)));
        ctx->pc = g_address[index];
        g_original[index](rdram, ctx, runtime);
        if (!log)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_lines;
        std::fprintf(g_out, "[nat] vb=%llu #%zu @%06x vm=%06x code=%06x pool=%06x pc=%x args=", vb, index, g_address[index], vm, base, pool, pc0);
        for (int i = 0; i < 6; ++i)
            std::fprintf(g_out, "%s%x", i ? "," : "", args[i]);
        for (int i = 0; i < 6; ++i)
        {
            const std::string s = pretty(rdram, pool, args[i]);
            if (!s.empty())
                std::fprintf(g_out, " a%d=\"%s\"", i, s.c_str());
        }
        std::fprintf(g_out, " ret=%x\n", getRegU32(ctx, 2));
        std::fflush(g_out);
    }

    template <size_t N>
    void thunk(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        traced(N, rdram, ctx, runtime);
    }

    template <size_t... I>
    constexpr auto makeThunks(std::index_sequence<I...>)
    {
        return std::array<PS2Runtime::RecompiledFunction, sizeof...(I)>{{&thunk<I>...}};
    }
}

namespace ps2_script_trace
{
    void installFromEnvironment(PS2Runtime &runtime)
    {
        const char *file = std::getenv("PS2X_SCRIPT_TRACE");
        if (file == nullptr || *file == '\0')
            return;
        g_out = std::string(file) == "1" ? stderr : std::fopen(file, "w");
        if (g_out == nullptr)
            return;
        if (const char *w = std::getenv("PS2X_SCRIPT_TRACE_VB"))
        {
            unsigned long long a = 0, b = 0;
            if (std::sscanf(w, "%llu-%llu", &a, &b) == 2)
            {
                g_from = a;
                g_to = b;
            }
        }
        if (const char *m = std::getenv("PS2X_SCRIPT_TRACE_MAX"))
            g_max = std::strtoull(m, nullptr, 10);
        const uint8_t *rdram = runtime.memory().getRDRAM();
        static const auto thunks = makeThunks(std::make_index_sequence<kMaxNatives>{});
        for (size_t i = 0; i < kMaxNatives; ++i)
        {
            const uint32_t a = rd32(rdram, kNativeTable + 4u * static_cast<uint32_t>(i));
            if (a < kCodeLo || a >= kCodeHi || (a & 3u) != 0u)
                break;
            g_address[i] = a;
            ++g_count;
        }
        size_t installed = 0;
        for (size_t i = 0; i < g_count; ++i)
        {
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j)
                duplicate = duplicate || g_address[j] == g_address[i];
            if (duplicate || !runtime.hasFunction(g_address[i]))
                continue;
            g_original[i] = runtime.lookupFunction(g_address[i]);
            if (g_original[i] != nullptr && runtime.replaceFunction(g_address[i], thunks[i]))
                ++installed;
        }
        std::fprintf(stderr, "[script-trace] %zu natives in the table, %zu hooked\n", g_count, installed);
    }
}
