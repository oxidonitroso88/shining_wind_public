#include <cstdio>
#include "iop_sysmem.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <string>

namespace ps2x::iop::detail
{
    IopSysmem::IopSysmem(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopSysmem::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // AllocSysMemory
        {
            const uint32_t address = a0 == 2u
                                         ? m_memory.allocate(a1, 16u, a2)
                                         : m_memory.allocate(a1, 16u);
            if (address == 0u)
            {
                std::fprintf(stderr, "[ps2xIOP:warning] [IOP:sysmem] AllocSysMemory(mode=%u, size=%u) failed; max free=%u; allocations:", a0, a1, m_memory.maxFreeMemory());
                for (const auto &block : m_memory.allocations())
                    std::fprintf(stderr, " %x+%x", block.address, block.size);
                std::fprintf(stderr, "\n");
            }
            setV0(address);
            return true;
        }
        case 5: // FreeSysMemory
            setV0(m_memory.freeAllocation(a0) ? 0u : 0xFFFFFFFFu);
            return true;
        case 6: // QueryMemSize
            setV0(IopMemory::RamSize);
            return true;
        case 7: // QueryMaxFreeMemSize: largest contiguous block
        case 8: // QueryTotalFreeMemSize: all free bytes
            std::fprintf(stderr, "[IOP:sysmem] QueryFree ordinal=%u -> %u\n", static_cast<unsigned>(ordinal),
                         ordinal == 7u ? m_memory.maxFreeMemory() : m_memory.totalFreeMemory());
            {
                static bool dumped = false;
                if (!dumped && m_memory.maxFreeMemory() < 120000u)
                {
                    dumped = true;
                    std::fprintf(stderr, "[IOP:sysmem] allocations:");
                    for (const auto &block : m_memory.allocations())
                        std::fprintf(stderr, " %x+%x", block.address, block.size);
                    std::fprintf(stderr, "\n");
                }
            }
            setV0(ordinal == 7u ? m_memory.maxFreeMemory() : m_memory.totalFreeMemory());
            return true;
        case 9: // QueryBlockTopAddress
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->address);
            else
                setV0(0u);
            return true;
        case 10: // QueryBlockSize
            if (const auto block = m_memory.allocationContaining(a0))
                setV0(block->size);
            else
                setV0(0xFFFFFFFFu);
            return true;
        case 14: // Kprintf
        {
            const std::string format = m_memory.readString(a0, 512u);
            m_host.log(LogLevel::Info, std::string("[IOP Kprintf] ") + format);
            setV0(static_cast<uint32_t>(format.size()));
            return true;
        }
        case 15:
            setV0(0u);
            return true;
        default:
            return false;
        }
    }
}
