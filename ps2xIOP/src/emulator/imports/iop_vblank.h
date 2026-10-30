#pragma once

#include <cstdint>
#include <vector>

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopKernel;

    class IopVblank
    {
    public:
        explicit IopVblank(IopKernel &kernel) noexcept;

        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);

        struct Handler
        {
            uint32_t function = 0;
            uint32_t argument = 0;
            uint32_t gp = 0;
        };

        void reset();
        [[nodiscard]] bool hasHandlers() const noexcept { return !m_start.empty() || !m_end.empty(); }
        // Earliest vblank edge (start or end) that has a registered handler and is later than `fallback`'s bound.
        [[nodiscard]] uint64_t nextEventCycle(uint64_t limit) const noexcept;
        // Handlers whose edge elapsed up to `now`; each edge is reported once, missed edges are coalesced.
        [[nodiscard]] std::vector<Handler> collectDue(uint64_t now);

    private:
        IopKernel &m_kernel;
        std::vector<Handler> m_start;
        std::vector<Handler> m_end;
        uint64_t m_nextStart = 0;
        uint64_t m_nextEnd = 0;
    };
}
