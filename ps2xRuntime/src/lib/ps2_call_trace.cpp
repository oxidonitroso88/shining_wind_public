// Diagnostic: PS2X_TRACE_CALLS="0x12a8e8,0x12b4c8:s0" traces calls of recompiled guest functions without touching generated code.
// The function registered at each address is wrapped (the original still runs with the same context); on entry the argument
// registers a0-a3, ra, sp are printed together with the C strings a0/a1 point to when they look like text, and on return v0.
// Suffix ":s0".. are not needed: only the first four arguments are decoded. PS2X_TRACE_MAX limits the number of lines (default 4000).
#include "ps2_call_trace.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    struct TraceEntry
    {
        uint32_t address = 0u;
        PS2Runtime::RecompiledFunction original = nullptr;
        bool heapCheck = false; // validates the game's allocator lists on entry instead of printing every call
    };

    constexpr size_t kMaxTraces = 32;
    std::array<TraceEntry, kMaxTraces> g_entries;
    std::atomic<long> g_budget{4000};
    std::mutex g_logMutex;

    std::string guestText(const uint8_t *rdram, uint32_t address)
    {
        if (address < 0x100000u || address >= 0x2000000u)
            return {};
        std::string out;
        for (uint32_t i = 0; i < 96u && address + i < 0x2000000u; ++i)
        {
            const uint8_t c = rdram[address + i];
            if (c == 0)
                return out.size() >= 2 ? out : std::string();
            if (c < 0x20 || c > 0x7e)
                return {};
            out.push_back(static_cast<char>(c));
        }
        return out + "...";
    }

    // ---- Game allocator (Shining Wind 0x100830 back-fit / 0x100A20 front-fit) -------------------------------------------
    // Pools live at 0x41E850 + idx*0x24: [+4] sentinel node, [+0x1C] rover. A node is {u32 size, u32 used, u32 prev, u32 next}.
    // The front-fit loop starts at the rover and follows `next` until it meets the sentinel: a rover that is not in the list or a
    // list that does not return to the sentinel spins forever (the symptom seen at 0x100A9C).
    constexpr uint32_t kPoolBase = 0x41E850u, kPoolStride = 0x24u, kPoolCount = 4u;
    bool g_heapReported[kPoolCount] = {};
    std::atomic<long> g_heapChecks{0};

    uint32_t word(const uint8_t *rdram, uint32_t address)
    {
        uint32_t v = 0u;
        if (address < 0x2000000u - 4u)
            std::memcpy(&v, rdram + address, 4u);
        return v;
    }

    std::string stackReturns(const uint8_t *rdram, uint32_t sp)
    {
        std::ostringstream oss;
        oss << std::hex;
        int found = 0;
        for (uint32_t a = sp; a < sp + 0x400u && found < 14; a += 4u)
        {
            const uint32_t v = word(rdram, a);
            if (v >= 0x100008u && v < 0x3FED74u && (v & 3u) == 0u && (word(rdram, v - 8u) >> 26) == 3u)
            {
                oss << " 0x" << v;
                ++found;
            }
        }
        return oss.str();
    }

    void heapCheck(const uint8_t *rdram, R5900Context *ctx, uint32_t entry)
    {
        const uint32_t pool = getRegU32(ctx, 4);
        const uint32_t size = getRegU32(ctx, 5);
        const long n = g_heapChecks.fetch_add(1);
        if (pool >= kPoolCount)
            return;
        if (size >= 0x40000u && n < 4000)
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            std::cerr << "[heap] alloc entry=0x" << std::hex << entry << " pool=" << std::dec << pool << " size=0x" << std::hex << size
                      << " ra=0x" << getRegU32(ctx, 31) << " returns:" << stackReturns(rdram, getRegU32(ctx, 29)) << std::dec << std::endl;
        }
        if (g_heapReported[pool] || rdram[0x41E380u] == 0u) // 0x41E380 = gp-0x7BF0: the allocator initialises itself lazily
            return;
        const uint32_t base = kPoolBase + pool * kPoolStride;
        const uint32_t sentinel = base + 4u, rover = word(rdram, base + 0x1Cu);
        uint32_t prev = sentinel, node = word(rdram, sentinel + 0xCu), count = 0u;
        bool roverSeen = (rover == sentinel);
        std::string problem;
        while (node != sentinel)
        {
            if (node < 0x100000u || node >= 0x2000000u || (node & 3u) != 0u)
            {
                problem = "node pointer out of range";
                break;
            }
            if (word(rdram, node + 8u) != prev)
            {
                problem = "prev link does not match";
                break;
            }
            if (word(rdram, node + 4u) > 1u)
            {
                problem = "used flag is neither 0 nor 1";
                break;
            }
            if (node == rover)
                roverSeen = true;
            prev = node;
            node = word(rdram, node + 0xCu);
            if (++count > 400000u)
            {
                problem = "list does not return to the sentinel";
                break;
            }
        }
        if (problem.empty() && !roverSeen)
            problem = "rover is not a member of the list";
        if (problem.empty())
            return;
        g_heapReported[pool] = true;
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "[heap] CORRUPTION in pool " << pool << " found at entry 0x" << std::hex << entry << " (check #" << std::dec << n << "): "
                  << problem << "; sentinel=0x" << std::hex << sentinel << " rover=0x" << rover << " nodes walked=" << std::dec << count
                  << " bad node=0x" << std::hex << node << " (prev=0x" << prev << ")"
                  << " request size=0x" << size << " ra=0x" << getRegU32(ctx, 31) << " returns:" << stackReturns(rdram, getRegU32(ctx, 29)) << std::endl;
        for (uint32_t a : {node, prev, rover, sentinel})
        {
            if (a >= 0x100000u && a < 0x2000000u - 16u)
                std::cerr << "[heap]   @0x" << a << ": size=0x" << word(rdram, a) << " used=" << word(rdram, a + 4u) << " prev=0x"
                          << word(rdram, a + 8u) << " next=0x" << word(rdram, a + 12u) << std::endl;
        }
        std::cerr << "[heap]   pool descriptor words:" << std::hex;
        for (uint32_t i = 0; i < 9u; ++i)
            std::cerr << " " << word(rdram, base + i * 4u);
        std::cerr << std::endl << "[heap]   16 words at bad node:";
        for (uint32_t i = 0; i < 16u && node >= 0x100000u && node < 0x1FFFFC0u; ++i)
            std::cerr << " " << word(rdram, node + i * 4u);
        std::cerr << std::dec << std::endl;
    }

    template <size_t I>
    void traceTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const TraceEntry &entry = g_entries[I];
        if (entry.heapCheck)
        {
            heapCheck(rdram, ctx, entry.address);
            ctx->pc = entry.address;
            entry.original(rdram, ctx, runtime);
            return;
        }
        const bool log = g_budget.fetch_sub(1) > 0;
        if (log)
        {
            std::ostringstream oss;
            oss << "[trace] call 0x" << std::hex << entry.address << " ra=0x" << getRegU32(ctx, 31) << " sp=0x" << getRegU32(ctx, 29)
                << " a0=0x" << getRegU32(ctx, 4) << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6)
                << " a3=0x" << getRegU32(ctx, 7);
            const std::string s0 = guestText(rdram, getRegU32(ctx, 4));
            const std::string s1 = guestText(rdram, getRegU32(ctx, 5));
            if (!s0.empty())
                oss << " a0=\"" << s0 << "\"";
            if (!s1.empty())
                oss << " a1=\"" << s1 << "\"";
            std::lock_guard<std::mutex> lock(g_logMutex);
            std::cerr << oss.str() << std::endl;
        }
        ctx->pc = entry.address; // re-enter the original exactly at its entry
        entry.original(rdram, ctx, runtime);
        if (log)
        {
            std::ostringstream oss;
            oss << "[trace] ret  0x" << std::hex << entry.address << " v0=0x" << getRegU32(ctx, 2);
            std::lock_guard<std::mutex> lock(g_logMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    template <size_t... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeTrampolines(std::index_sequence<Is...>)
    {
        return {&traceTrampoline<Is>...};
    }
}

    // ---- PS2X_TRACE_SJIS="0xLO-0xHI": log, once per (function, text), every registered function in that range that is called with a pointer
    // (a0-a3, t0-t3) to Shift-JIS text (>= 3 double-byte characters). Finds the functions that receive the message strings of the game's text system.
    constexpr size_t kMaxSjis = 9000;
    std::array<TraceEntry, kMaxSjis> g_sjis;
    std::set<std::pair<uint32_t, uint64_t>> g_sjisSeen;
    std::atomic<long> g_sjisBudget{3000};

    bool looksSjis(const uint8_t *rdram, uint32_t p, std::string &hex)
    {
        if (p < 0x100000u || p >= 0x2000000u - 130u)
            return false;
        int kana = 0, pairs = 0;
        uint32_t i = 0;
        bool ended = false;
        for (; i < 96u;)
        {
            const uint8_t c = rdram[p + i];
            if (c == 0)
            {
                ended = true;
                break;
            }
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC))
            {
                const uint8_t d = rdram[p + i + 1];
                if (!((d >= 0x40 && d <= 0x7E) || (d >= 0x80 && d <= 0xFC)))
                    return false;
                ++pairs;
                if ((c == 0x82 && d >= 0x9F && d <= 0xF1) || (c == 0x83 && d >= 0x40 && d <= 0x96))
                    ++kana;
                i += 2;
            }
            else if ((c >= 0x20 && c < 0x7F) || c == 0x0A || c == 0x0D)
                ++i;
            else
                return false;
        }
        if (!ended || pairs < 3 || kana < 2)
            return false;
        std::ostringstream oss;
        for (uint32_t k = 0; k < i && k < 48u; ++k)
        {
            const char *hexd = "0123456789abcdef";
            oss << hexd[rdram[p + k] >> 4] << hexd[rdram[p + k] & 15];
        }
        hex = oss.str();
        return true;
    }

    template <size_t I>
    void sjisTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const TraceEntry &entry = g_sjis[I];
        if (g_sjisBudget.load() > 0)
        {
            static const int regs[] = {4, 5, 6, 7, 8, 9, 10, 11};
            for (int k = 0; k < 8; ++k)
            {
                std::string hex;
                const uint32_t p = getRegU32(ctx, regs[k]);
                if (!looksSjis(rdram, p, hex))
                    continue;
                uint64_t h = 1469598103934665603ull;
                for (char c : hex)
                    h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
                std::lock_guard<std::mutex> lock(g_logMutex);
                if (g_sjisSeen.insert({entry.address, h}).second && g_sjisBudget.fetch_sub(1) > 0)
                    std::cerr << "[sjis] fn=0x" << std::hex << entry.address << " arg=r" << std::dec << regs[k] << " ptr=0x" << std::hex << p << " ra=0x"
                              << getRegU32(ctx, 31) << " text=" << hex << std::dec << std::endl;
                break;
            }
        }
        ctx->pc = entry.address;
        entry.original(rdram, ctx, runtime);
    }

    template <size_t... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> makeSjisTrampolines(std::index_sequence<Is...>)
    {
        return {&sjisTrampoline<Is>...};
    }

namespace ps2_call_trace
{
    void installFromEnvironment(PS2Runtime &runtime)
    {
        static const auto trampolines = makeTrampolines(std::make_index_sequence<kMaxTraces>{});
        size_t used = 0;
        // PS2X_HEAP_CHECK=1: validate the game's allocator pools at every allocation and report the first corruption.
        if (const char *heap = std::getenv("PS2X_HEAP_CHECK"); heap != nullptr && *heap != '\0' && *heap != '0')
        {
            for (const uint32_t address : {0x100830u, 0x100A20u})
            {
                PS2Runtime::RecompiledFunction original = runtime.hasFunction(address) ? runtime.lookupFunction(address) : nullptr;
                if (original == nullptr)
                    continue;
                g_entries[used] = {address, original, true};
                if (runtime.replaceFunction(address, trampolines[used]))
                {
                    std::cerr << "[heap] checking allocator 0x" << std::hex << address << std::dec << std::endl;
                    ++used;
                }
            }
        }
        if (const char *range = std::getenv("PS2X_TRACE_SJIS"); range != nullptr && *range != '\0')
        {
            static const auto sjisTramp = makeSjisTrampolines(std::make_index_sequence<kMaxSjis>{});
            size_t n = 0;
            std::string all(range);
            size_t at = 0;
            while (at < all.size())
            {
                size_t end = all.find(',', at);
                if (end == std::string::npos)
                    end = all.size();
                const std::string item = all.substr(at, end - at);
                at = end + 1;
                const size_t dash = item.find('-');
                if (dash == std::string::npos)
                    continue;
                const uint32_t lo = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
                const uint32_t hi = static_cast<uint32_t>(std::strtoul(item.c_str() + dash + 1, nullptr, 0));
                for (uint32_t addr = lo & ~3u; addr < hi && n < kMaxSjis; addr += 4u)
                {
                    PS2Runtime::RecompiledFunction original = runtime.hasFunction(addr) ? runtime.lookupFunction(addr) : nullptr;
                    if (original == nullptr)
                        continue;
                    g_sjis[n] = {addr, original};
                    if (runtime.replaceFunction(addr, sjisTramp[n]))
                        ++n;
                }
            }
            std::cerr << "[sjis] tracing " << n << " functions in " << range << std::endl;
        }
        const char *spec = std::getenv("PS2X_TRACE_CALLS");
        if (spec == nullptr || *spec == '\0')
            return;
        if (const char *max = std::getenv("PS2X_TRACE_MAX"))
            g_budget.store(std::atol(max));

        std::string text(spec);
        size_t pos = 0;
        while (pos < text.size() && used < kMaxTraces)
        {
            size_t end = text.find(',', pos);
            const std::string item = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            if (item.empty())
                continue;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
            // The function to wrap: whatever is registered now (main table, supplement). Overlay code is not traced here.
            PS2Runtime::RecompiledFunction original = runtime.hasFunction(address) ? runtime.lookupFunction(address) : nullptr;
            if (original == nullptr)
            {
                std::cerr << "[trace] 0x" << std::hex << address << std::dec << ": no registered function, not traced" << std::endl;
                continue;
            }
            g_entries[used] = {address, original};
            if (runtime.replaceFunction(address, trampolines[used]))
            {
                std::cerr << "[trace] tracing 0x" << std::hex << address << std::dec << std::endl;
                ++used;
            }
        }
    }
}
