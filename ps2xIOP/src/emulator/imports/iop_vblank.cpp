#include "iop_vblank.h"

#include "../core/iop_cpu.h"
#include "../iop_emulator_const.h"
#include "../core/iop_kernel.h"

namespace ps2x::iop::detail
{
    IopVblank::IopVblank(IopKernel &kernel) noexcept
        : m_kernel(kernel)
    {
    }

    bool IopVblank::dispatchImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        switch (ordinal)
        {
        case 4: // WaitVblankStart
        case 5: // WaitVblankEnd
        case 6: // WaitVblank
        case 7: // WaitNonVblank
        {
            const bool waitForEnd = ordinal == 5u || ordinal == 7u;
            const uint64_t phase = waitForEnd ? kVblankEndPhaseCycles : 0u;
            const uint64_t fieldStart = currentCycle - (currentCycle % kVblankPeriodCycles);
            uint64_t wakeCycle = fieldStart + phase;
            if (wakeCycle <= currentCycle)
                wakeCycle += kVblankPeriodCycles;
            m_kernel.delayCurrentUntil(wakeCycle, cpu);
            cpu.gpr[2] = 0u;
            return true;
        }
        case 8: // RegisterVblankHandler(startend, priority, handler, arg): 0 = vblank start, 1 = vblank end
        {
            const bool end = cpu.gpr[4] != 0u;
            auto &list = end ? m_end : m_start;
            if (list.empty())
            {
                const uint64_t phase = end ? kVblankEndPhaseCycles : 0u;
                uint64_t &next = end ? m_nextEnd : m_nextStart;
                next = currentCycle - (currentCycle % kVblankPeriodCycles) + phase;
                if (next <= currentCycle)
                    next += kVblankPeriodCycles;
            }
            list.push_back(Handler{cpu.gpr[6], cpu.gpr[7], cpu.gpr[28]});
            cpu.gpr[2] = 0u;
            return true;
        }
        case 9: // ReleaseVblankHandler(startend, handler)
        {
            auto &list = cpu.gpr[4] != 0u ? m_end : m_start;
            for (auto it = list.begin(); it != list.end(); ++it)
            {
                if (it->function == cpu.gpr[5])
                {
                    list.erase(it);
                    break;
                }
            }
            cpu.gpr[2] = 0u;
            return true;
        }
        default:
            return false;
        }
    }

    void IopVblank::reset()
    {
        m_start.clear();
        m_end.clear();
        m_nextStart = 0u;
        m_nextEnd = 0u;
    }

    uint64_t IopVblank::nextEventCycle(uint64_t limit) const noexcept
    {
        if (!m_start.empty())
            limit = limit < m_nextStart ? limit : m_nextStart;
        if (!m_end.empty())
            limit = limit < m_nextEnd ? limit : m_nextEnd;
        return limit;
    }

    std::vector<IopVblank::Handler> IopVblank::collectDue(uint64_t now)
    {
        std::vector<Handler> due;
        const auto collect = [&](const std::vector<Handler> &list, uint64_t &next) {
            if (list.empty() || next > now)
                return;
            due.insert(due.end(), list.begin(), list.end());
            // Coalesce edges that were missed while the IOP was busy: report once, then resume on the next edge.
            const uint64_t missed = (now - next) / kVblankPeriodCycles;
            next += (missed + 1u) * kVblankPeriodCycles;
        };
        collect(m_start, m_nextStart);
        collect(m_end, m_nextEnd);
        return due;
    }
}
