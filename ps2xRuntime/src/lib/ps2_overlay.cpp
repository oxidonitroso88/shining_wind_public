#include "ps2_overlay.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace ps2_overlay
{
    namespace
    {
        struct State
        {
            std::mutex mutex;
            std::vector<const Descriptor *> overlays;
            std::atomic<const Descriptor *> active{nullptr};
            std::atomic<bool> dirty{true};
            std::atomic<uint32_t> regionBegin{0xFFFFFFFFu};
            std::atomic<uint32_t> regionEnd{0u};
            std::atomic<uint64_t> switches{0};
            bool everLogged = false;
        };

        State &state()
        {
            static State s; // function-local: registration happens during static initialisation of the generated code
            return s;
        }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0u;
            std::memcpy(&value, rdram + address, 4u);
            return value;
        }

        bool quickMatch(const Descriptor &d, const uint8_t *rdram)
        {
            for (uint32_t i = 0; i < d.sampleCount; ++i)
            {
                if (readWord(rdram, d.codeBegin + i * d.sampleStride) != d.sampleWords[i])
                {
                    return false;
                }
            }
            return true;
        }

        bool fullMatch(const Descriptor &d, const uint8_t *rdram)
        {
            return hashBytes(rdram + d.codeBegin, d.codeEnd - d.codeBegin) == d.codeHash;
        }

        bool inCode(const Descriptor &d, uint32_t address)
        {
            return (address >= d.codeBegin && address < d.codeEnd) || (address >= d.extraBegin && address < d.extraEnd);
        }

        bool matches(const Descriptor &d, const uint8_t *rdram)
        {
            return quickMatch(d, rdram) && fullMatch(d, rdram);
        }

        PS2Runtime::RecompiledFunction find(const Descriptor &d, uint32_t address)
        {
            const Function *begin = d.functions;
            const Function *end = d.functions + d.functionCount;
            const Function *it = std::lower_bound(begin, end, address,
                                                  [](const Function &f, uint32_t a) { return f.address < a; });
            return (it != end && it->address == address) ? it->function : nullptr;
        }

        // Re-verifies the installed overlay (state mutex held). Returns the overlay whose code is in memory.
        const Descriptor *refresh(State &s, const uint8_t *rdram)
        {
            const Descriptor *previous = s.active.load(std::memory_order_acquire);
            const Descriptor *now = nullptr;
            if (previous != nullptr && matches(*previous, rdram))
            {
                now = previous;
            }
            else
            {
                for (const Descriptor *candidate : s.overlays)
                {
                    if (candidate != previous && matches(*candidate, rdram))
                    {
                        now = candidate;
                        break;
                    }
                }
            }

            s.dirty.store(false, std::memory_order_release);
            if (now != previous || !s.everLogged)
            {
                s.switches.fetch_add(1, std::memory_order_relaxed);
                s.everLogged = true;
                std::cerr << "[overlay] installed code: " << (now ? now->name : "none (no registered overlay matches the memory)")
                          << " (was " << (previous ? previous->name : "none") << ")" << std::endl;
            }
            s.active.store(now, std::memory_order_release);
            return now;
        }
    } // namespace

    void registerOverlay(const Descriptor *descriptor)
    {
        if (descriptor == nullptr)
        {
            return;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (std::find(s.overlays.begin(), s.overlays.end(), descriptor) != s.overlays.end())
        {
            return;
        }
        s.overlays.push_back(descriptor);
        s.regionBegin.store(std::min(s.regionBegin.load(), descriptor->codeBegin));
        s.regionEnd.store(std::max(s.regionEnd.load(), descriptor->codeEnd));
        if (descriptor->extraEnd > descriptor->extraBegin)
        {
            s.regionBegin.store(std::min(s.regionBegin.load(), descriptor->extraBegin));
            s.regionEnd.store(std::max(s.regionEnd.load(), descriptor->extraEnd));
        }
        s.dirty.store(true, std::memory_order_release);
    }

    uint32_t registerSupplement(const char *name, const Function *functions, uint32_t count)
    {
        uint32_t set = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t address = functions[i].address;
            if ((address & 3u) != 0u || address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
            {
                std::cerr << "[supplement] " << name << ": 0x" << std::hex << address << std::dec << " is outside the main table" << std::endl;
                continue;
            }
            const uint32_t slot = (address - g_ps2RecompiledFunctionTableBase) >> 2;
            if (g_ps2RecompiledFunctionTable[slot] == nullptr)
            {
                g_ps2RecompiledFunctionTable[slot] = functions[i].function;
                ++set;
            }
        }
        return set;
    }

    bool inOverlayRegion(uint32_t address)
    {
        State &s = state();
        return address >= s.regionBegin.load(std::memory_order_relaxed) && address < s.regionEnd.load(std::memory_order_relaxed);
    }

    PS2Runtime::RecompiledFunction resolve(const uint8_t *rdram, uint32_t address)
    {
        State &s = state();
        if (rdram == nullptr || !inOverlayRegion(address))
        {
            return nullptr;
        }

        const Descriptor *current = s.active.load(std::memory_order_acquire);
        if (current != nullptr && inCode(*current, address) &&
            !s.dirty.load(std::memory_order_acquire) && quickMatch(*current, rdram))
        {
            return find(*current, address);
        }

        std::lock_guard<std::mutex> lock(s.mutex);
        const Descriptor *installed = refresh(s, rdram);
        if (installed == nullptr || !inCode(*installed, address))
        {
            return nullptr;
        }
        return find(*installed, address);
    }

    void notifyCodeFlush()
    {
        state().dirty.store(true, std::memory_order_release);
    }

    const Descriptor *active()
    {
        return state().active.load(std::memory_order_acquire);
    }

    std::string describe(const uint8_t *rdram)
    {
        State &s = state();
        std::ostringstream oss;
        const Descriptor *current = s.active.load(std::memory_order_acquire);
        oss << "overlays registered=" << registeredCount() << " active=" << (current ? current->name : "none")
            << " switches=" << s.switches.load() << std::hex;
        const uint32_t begin = s.regionBegin.load();
        if (rdram != nullptr && begin != 0xFFFFFFFFu)
        {
            oss << " region@0x" << begin << "[0..3]=" << readWord(rdram, begin) << "," << readWord(rdram, begin + 4u) << ","
                << readWord(rdram, begin + 8u) << "," << readWord(rdram, begin + 12u);
        }
        return oss.str();
    }

    void resetForTests()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.overlays.clear();
        s.active.store(nullptr);
        s.dirty.store(true);
        s.regionBegin.store(0xFFFFFFFFu);
        s.regionEnd.store(0u);
        s.switches.store(0);
        s.everLogged = false;
    }

    uint32_t registeredCount()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        return static_cast<uint32_t>(s.overlays.size());
    }

    uint64_t switchCount()
    {
        return state().switches.load();
    }
} // namespace ps2_overlay
