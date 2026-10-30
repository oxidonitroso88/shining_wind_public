#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace ps2x::iop
{
    // Command-level virtual device, not a model of optical mechanics or MMIO.
    // In particular Ready does not certify a mounted/authenticated PS2 disc.
    class CdvdDevice
    {
    public:
        enum class Session { Uninitialized, Initializing, Initialized };
        enum class Phase { Rejected, Pending, Completed, Cancelled };
        enum class Work { Command, ReadDuringDetection };
        enum class Source { Unconfigured, ImageCandidate, DirectoryCandidate };
        struct Token { uint64_t generation = 0, id = 0; };
        struct Result
        {
            Phase phase = Phase::Rejected;
            Token token{};
            std::optional<int32_t> value;
        };
        struct Snapshot
        {
            bool prepared = false;
            Session session = Session::Uninitialized;
            uint32_t selectedMode = 0;
            uint64_t generation = 1;
            size_t activeOperations = 0;
            Source source = Source::Unconfigured;
            std::string sourcePath;
            // Configuration provenance only, never a mounted-disc assertion.
        };

        void configureSource(Source source, std::string path)
        {
            state.source = source;
            state.sourcePath = std::move(path);
        }
        // Called only once the owning synchronous backend's memory/kernel/host
        // interfaces exist. No filesystem check is used as command readiness.
        void prepare() { state.prepared = true; progress(); }
        void reset()
        {
            ++state.generation;
            state.prepared = false;
            state.session = Session::Uninitialized;
            state.selectedMode = 0;
            work.clear(); waiters.clear();
        }
        Snapshot snapshot() const
        {
            auto copy = state; copy.activeOperations = work.size(); return copy;
        }
        Token begin(Work kind)
        {
            if (!state.prepared) return {};
            const Token token{state.generation, ++serial};
            work.emplace(token.id, kind); return token;
        }
        bool finish(Token token)
        {
            if (token.generation != state.generation || !work.erase(token.id)) return false;
            progress(); return true;
        }
        Result init(uint32_t mode)
        {
            if (mode != 0 && mode != 1 && mode != 5) return {};
            if (!state.prepared) return {}; // backend absent, not an operation success
            if (mode == 5 && !work.empty()) return {}; // deferred Exit not supported
            if (mode == 5)
            {
                state.session = Session::Uninitialized;
                return {Phase::Completed, {}, 1};
            }
            if (mode == 1 || ready())
            {
                state.session = Session::Initialized;
                return {Phase::Completed, {}, 1};
            }
            state.session = Session::Initializing;
            return pending(true);
        }
        Result setMode(uint32_t mode)
        {
            if (!state.prepared) return {};
            state.selectedMode = mode; // PS2SDK sceCdMmode stores its argument
            return {Phase::Completed, {}, 1};
        }
        Result diskReady(uint32_t mode)
        {
            if (!state.prepared || mode > 1) return {}; // raw mode8 not emulated
            if (mode == 1) return {Phase::Completed, {}, ready() ? 2 : 6};
            // SDK base variant mode0 waits specifically in read2/detection.
            // It is not mode1 repeatedly polled until all work is idle.
            if (!detectionRead()) return {Phase::Completed, {}, 2};
            return pending(false);
        }
        Result poll(Token token, bool consume = false)
        {
            if (!token.id) return {};
            if (token.generation != state.generation) return {Phase::Cancelled, token, {}};
            const auto it = waiters.find(token.id);
            if (it == waiters.end()) return {};
            auto result = it->second.result;
            if (consume && result.phase != Phase::Pending) waiters.erase(it);
            return result;
        }
    private:
        struct Waiter { bool initialization; Result result; };
        bool ready() const { return state.prepared && work.empty(); }
        bool detectionRead() const
        {
            for (const auto &[id, kind] : work)
                if (kind == Work::ReadDuringDetection) return true;
            return false;
        }
        Result pending(bool initialization)
        {
            Result result{Phase::Pending, {state.generation, ++serial}, {}};
            waiters.emplace(result.token.id, Waiter{initialization, result});
            return result;
        }
        void progress()
        {
            for (auto &[id, waiter] : waiters)
            {
                if (waiter.result.phase != Phase::Pending) continue;
                if (waiter.initialization ? ready() : !detectionRead())
                {
                    waiter.result.phase = Phase::Completed;
                    waiter.result.value = waiter.initialization ? 1 : 2;
                    if (waiter.initialization) state.session = Session::Initialized;
                }
            }
        }
        Snapshot state;
        uint64_t serial = 0;
        std::map<uint64_t, Work> work;
        std::map<uint64_t, Waiter> waiters;
    };
}
