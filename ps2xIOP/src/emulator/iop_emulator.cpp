#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "imports/iop_heap_rpc.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>
#include <set>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <array>
#include <iomanip>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            std::string irxName; // name from the IRX's .iopmod section (empty if absent)
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        struct PadEvent
        {
            uint64_t cyc = 0;
            uint16_t buttons = 0xFFFFu;
            uint8_t sticks[4] = {0x80u, 0x80u, 0x80u, 0x80u};
        };

        static std::vector<PadEvent> loadPadReplay()
        {
            std::vector<PadEvent> events;
            const char *path = std::getenv("PS2X_PAD_REPLAY");
            if (!path || !*path)
                return events;
            if (std::FILE *f = std::fopen(path, "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), f))
                {
                    unsigned long long cyc = 0;
                    unsigned buttons = 0xFFFFu;
                    unsigned sticks = 0x80808080u;
                    const char *at = std::strstr(line, "[pad] cyc=");
                    if (!at || std::sscanf(at, "[pad] cyc=%llu block=%*u buttons=%x sticks=%x", &cyc, &buttons, &sticks) < 2)
                        continue;
                    if (!std::strstr(at, "sticks="))
                        sticks = 0x80808080u;
                    PadEvent event;
                    event.cyc = cyc;
                    event.buttons = static_cast<uint16_t>(buttons);
                    for (size_t i = 0; i < 4u; ++i)
                        event.sticks[i] = static_cast<uint8_t>(sticks >> (24u - 8u * i));
                    events.push_back(event);
                }
                std::fclose(f);
                std::fprintf(stderr, "[pad-replay] %zu events from %s\n", events.size(), path);
            }
            return events;
        }

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            memory.setCycleSource(&totalCycles);
            memory.setPadSource([this](int port, PadSample &sample) {
                uint16_t buttons = 0xFFFFu;
                uint8_t sticks[4] = {0x80u, 0x80u, 0x80u, 0x80u};
                static const std::vector<PadEvent> replayEvents = loadPadReplay();
                if (!host.padState(port, buttons, sticks) && replayEvents.empty())
                    return false;
                // PS2X_PAD_REPLAY=<file>: replays "[pad] cyc=.. buttons=.. sticks=.." lines (as written by PS2X_PAD_LOG, e.g. a game.stderr.log) by IOP cycle,
                // replacing the live pad. Deterministic only as far as the emulated timeline is.
                const std::vector<PadEvent> &replay = replayEvents;
                if (!replay.empty())
                {
                    const PadEvent *current = nullptr;
                    for (const PadEvent &event : replay)
                    {
                        if (event.cyc > totalCycles)
                            break;
                        current = &event;
                    }
                    buttons = current ? current->buttons : 0xFFFFu;
                    for (size_t i = 0; i < 4u; ++i)
                        sticks[i] = current ? current->sticks[i] : 0x80u;
                }
                if (static const bool padLog = std::getenv("PS2X_PAD_LOG") != nullptr; padLog)
                {
                    static uint16_t lastButtons = 0xFFFFu;
                    static uint8_t lastSticks[4] = {0x80u, 0x80u, 0x80u, 0x80u};
                    bool stickMoved = false;
                    for (size_t i = 0; i < 4u; ++i)
                        stickMoved = stickMoved || std::abs(int(sticks[i]) - int(lastSticks[i])) >= 6;
                    if (buttons != lastButtons || stickMoved)
                    {
                        std::fprintf(stderr, "[pad] cyc=%llu block=%llu buttons=%04x sticks=%02x%02x%02x%02x\n", static_cast<unsigned long long>(totalCycles),
                                     static_cast<unsigned long long>(memory.admaBlockCount()), buttons, sticks[0], sticks[1], sticks[2], sticks[3]);
                        lastButtons = buttons;
                        for (size_t i = 0; i < 4u; ++i)
                            lastSticks[i] = sticks[i];
                    }
                }
                sample.buttons = buttons;
                for (size_t i = 0; i < 4u; ++i)
                    sample.sticks[i] = sticks[i];
                return true;
            });
            reset();
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            vblank.reset();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            cycleBudget = 0;
            lastAudioPumpCycle = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            pendingIopCycles = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        // PS2X_DUMP_ON_ZERO=<prefix> [PS2X_DUMP_ON_ZERO_FROM=<AutoDMA block>]: when the AutoDMA stream goes silent right after audible
        // blocks, write the EE RAM and the IOP RAM (first two events only) for offline analysis of the audio pipeline.
        void dumpOnSilence()
        {
            static const char *prefix = std::getenv("PS2X_DUMP_ON_ZERO");
            if (!prefix || !*prefix || !memory.takeZeroBlockEvent())
                return;
            static const uint64_t from = []() { const char *v = std::getenv("PS2X_DUMP_ON_ZERO_FROM"); return v ? std::strtoull(v, nullptr, 10) : 0ull; }();
            static int dumps = 0;
            if (dumps >= 2 || memory.admaBlockCount() < from)
                return;
            ++dumps;
            const std::string base = std::string(prefix) + "." + std::to_string(dumps);
            std::vector<uint8_t> chunk(1u << 20);
            if (std::FILE *f = std::fopen((base + ".ee.bin").c_str(), "wb"))
            {
                for (uint32_t offset = 0; offset < (32u << 20); offset += static_cast<uint32_t>(chunk.size()))
                    if (host.readGuest(offset, chunk.data(), chunk.size()))
                        std::fwrite(chunk.data(), 1, chunk.size(), f);
                std::fclose(f);
            }
            if (std::FILE *f = std::fopen((base + ".iop.bin").c_str(), "wb"))
            {
                for (uint32_t offset = 0; offset < (2u << 20); offset += static_cast<uint32_t>(chunk.size()))
                    if (memory.readRam(offset, chunk.data(), chunk.size()))
                        std::fwrite(chunk.data(), 1, chunk.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[dump-on-zero] %s at AutoDMA block %llu, IOP cycle %llu\n", base.c_str(),
                         static_cast<unsigned long long>(memory.admaBlockCount()), static_cast<unsigned long long>(totalCycles));
        }

        // PS2X_DUMP_ON_NOISE=<prefix>: at the first three noise-like AutoDMA blocks write the IOP RAM (2 MB) and the EE RAM (32 MB) for offline analysis.
        void dumpOnNoise()
        {
            static const char *prefix = std::getenv("PS2X_DUMP_ON_NOISE");
            if (!prefix || !*prefix || !memory.takeNoiseEvent())
                return;
            static int dumps = 0;
            if (dumps >= 3)
                return;
            ++dumps;
            const std::string base = std::string(prefix) + "." + std::to_string(dumps);
            std::vector<uint8_t> chunk(1u << 20);
            if (std::FILE *f = std::fopen((base + ".ee.bin").c_str(), "wb"))
            {
                for (uint32_t offset = 0; offset < (32u << 20); offset += static_cast<uint32_t>(chunk.size()))
                    if (host.readGuest(offset, chunk.data(), chunk.size()))
                        std::fwrite(chunk.data(), 1, chunk.size(), f);
                std::fclose(f);
            }
            if (std::FILE *f = std::fopen((base + ".iop.bin").c_str(), "wb"))
            {
                for (uint32_t offset = 0; offset < (2u << 20); offset += static_cast<uint32_t>(chunk.size()))
                    if (memory.readRam(offset, chunk.data(), chunk.size()))
                        std::fwrite(chunk.data(), 1, chunk.size(), f);
                std::fclose(f);
            }
            std::fprintf(stderr, "[dump-on-noise] %s at AutoDMA block %llu, IOP cycle %llu\n", base.c_str(),
                         static_cast<unsigned long long>(memory.admaBlockCount()), static_cast<unsigned long long>(totalCycles));
        }

        // PS2X_AUDIO_PROBE=<hex EE address>[,...]: prints those EE words at every AutoDMA block ("[probe] blk=N cyc=C a=v ...").
        void probeAudio()
        {
            static const std::vector<uint32_t> addresses = []() {
                std::vector<uint32_t> list;
                if (const char *v = std::getenv("PS2X_AUDIO_PROBE"))
                {
                    std::string text(v);
                    std::stringstream parts(text);
                    std::string item;
                    while (std::getline(parts, item, ','))
                        if (!item.empty())
                            list.push_back(static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)));
                }
                return list;
            }();
            static uint64_t last = ~0ull;
            if (addresses.empty() || memory.admaBlockCount() == last)
                return;
            last = memory.admaBlockCount();
            std::string line = "[probe] blk=" + std::to_string(last) + " cyc=" + std::to_string(totalCycles);
            for (const uint32_t address : addresses)
            {
                uint32_t value = 0u;
                host.readGuest(address, &value, sizeof(value));
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), " %x=%x", address, value);
                line += buffer;
            }
            std::fprintf(stderr, "%s\n", line.c_str());
        }

        const bool diagnosticHooks = std::getenv("PS2X_AUDIO_PROBE") != nullptr || std::getenv("PS2X_DUMP_ON_ZERO") != nullptr || std::getenv("PS2X_DUMP_ON_NOISE") != nullptr;
        void schedulePendingDma()
        {
            // Per executed instruction: the three diagnostic hooks cost a function call each, so they only run when one of them is enabled.
            if (diagnosticHooks)
            {
                probeAudio();
                dumpOnSilence();
                dumpOnNoise();
            }
            if (memory.hasDmaStart())
            {
                if (const auto dma = memory.takeDmaStart())
                    pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, cpu.branchPending);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                return rpc.dispatchSifManImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // A misaligned fetch is an address-error exception (AdEL); do not execute from a truncated address.
            if (cpu.pc & 3u)
            {
                cpuCore.raiseException(cpu, 4u, cpu.pc, cpu.branchPending, cpu.pc);
                ++totalInstructions;
                ++totalCycles;
                return true;
            }

            // One fetch shared by the import-stub check and the interpreter.
            const uint32_t instruction = memory.read32(cpu.pc);
            if (const auto import = imports.decode(cpu.pc, instruction))
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                if (profileEnabled) ++importCounts[std::string(import->library) + ":" + std::to_string(import->ordinal)];
                if (disposition == ImportDisposition::Missing)
                {
                    ++failedRpcImports;
                    if (missingImportsReported.insert(std::string(import->library) + ":" + std::to_string(import->ordinal)).second)
                        log(LogLevel::Warning, "[IOP:import] unimplemented " + std::string(import->library) + " ordinal " + std::to_string(import->ordinal) +
                                                   " called from ra=0x" + [&] { std::ostringstream o; o << std::hex << cpu.gpr[31]; return o.str(); }());
                }
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                {
                    if (jumpImportsReported.insert(std::string(import->library) + ":" + std::to_string(import->ordinal)).second)
                        log(LogLevel::Info, "[IOP:import] " + std::string(import->library) + " ordinal " + std::to_string(import->ordinal) + " jumps to guest 0x" +
                                                [&] { std::ostringstream o; o << std::hex << cpu.pc; return o.str(); }());
                    return true;
                }
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            if (profileEnabled && (totalInstructions % 97u) == 0u) { ++pcCounts[cpu.pc]; ++raCounts[cpu.gpr[31]]; }
            recentPcs[recentPcPos++ & 63u] = cpu.pc;
            const bool running = cpuCore.executeInstruction(cpu, instruction);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        uint32_t loggedCpuExceptions = 0u;
        // Debug aid: PS2X_IOP_WATCH=<hex IOP address> logs every CPU-visible change of that word.
        // DMA/SIF writes are noticed on the next executed instruction, so the pc is only a hint.
        uint32_t watchAddress = []() -> uint32_t {
            const char *text = std::getenv("PS2X_IOP_WATCH");
            return text ? static_cast<uint32_t>(std::strtoul(text, nullptr, 16)) & ~3u : 0u;
        }();
        uint32_t watchLast = 0u;
        uint32_t watchHits = 0u;

        // On the real IOP an interrupt is the only thing that can pull a thread off the CPU or run a handler behind its back,
        // and none is taken while interrupts are disabled (CpuSuspendIntr/mtc0). The kernel here swaps threads at slice
        // boundaries and delivers DMA/VBLANK/callback "interrupts" between instructions, so both must be held back while the
        // running thread sits in such a section; otherwise two threads (or a handler) enter the same CpuSuspendIntr-protected
        // critical section and corrupt shared driver state (a ring-buffer chunk goes missing, the audio driver never
        // completes its next start). A thread that keeps interrupts off for a very long time is let through anyway so that a
        // code path which was only ever unblocked by our asynchronous delivery cannot hang the IOP.
        static constexpr uint32_t kMaxMaskedRun = 20000u;
        uint32_t maskedRun = 0u;

        // The interrupt-enable state is the global I_CTRL that intrman's CpuSuspendIntr/CpuResumeIntr drive; threads start with
        // cop0 Status zeroed, so the per-thread IEc bit cannot be used to tell "disabled" from "never enabled".
        bool interruptsMasked(const CpuState &) const
        {
            return memory.interruptControl() == 0u;
        }

        bool deferInterrupts(const CpuState &cpu) const
        {
            return maskedRun < kMaxMaskedRun && interruptsMasked(cpu);
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                const uint32_t faultPc = cpu.pc;
                if (!step(cpu))
                    break;
                if (watchAddress != 0u)
                {
                    const uint32_t now = memory.read32(watchAddress);
                    if (now != watchLast)
                    {
                        std::ostringstream text;
                        text << "[IOP:watch] 0x" << std::hex << watchAddress << ": 0x" << watchLast << " -> 0x" << now
                             << " after pc=0x" << faultPc << " ra=0x" << cpu.gpr[31] << " a0=0x" << cpu.gpr[4]
                             << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6] << " instr=0x" << memory.read32(faultPc);
                        watchLast = now;
                        if (watchHits++ < 16u)
                            log(LogLevel::Warning, text.str());
                    }
                }
                if (cpu.exception && loggedCpuExceptions < 8u)
                {
                    ++loggedCpuExceptions;
                    std::ostringstream text;
                    text << "[IOP:exception] cause=0x" << std::hex << ((cpu.cop0[13] >> 2u) & 0x1Fu) << " pc=0x" << faultPc
                         << " instr=0x" << memory.read32(faultPc) << " ra=0x" << cpu.gpr[31] << " sp=0x" << cpu.gpr[29]
                         << " badvaddr=0x" << cpu.cop0[8] << " status=0x" << cpu.cop0[12];
                    log(LogLevel::Warning, text.str());
                    if (loggedCpuExceptions == 1u)
                    {
                        std::ostringstream regs;
                        regs << "[IOP:exception] first-fault gpr:" << std::hex;
                        for (uint32_t r = 1; r < 32u; ++r)
                            regs << " r" << std::dec << r << "=" << std::hex << cpu.gpr[r];
                        regs << " code@ra-0x30:";
                        for (uint32_t a = cpu.gpr[31] - 0x30u; a <= cpu.gpr[31] + 8u; a += 4u)
                            regs << " " << memory.read32(a);
                        log(LogLevel::Warning, regs.str());
                    }
                }
                maskedRun = interruptsMasked(cpu) ? maskedRun + 1u : 0u;
                if (deferInterrupts(cpu))
                    continue;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
                if (!servicingGuestCallbacks && vblank.hasHandlers())
                    serviceVblankHandlers();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions, bool *completed = nullptr)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            const uint64_t executedBefore = totalInstructions;
            runCpu(cpu, budget);
            if (completed) *completed = cpu.pc == kCallReturnSentinel;
            if (completed && cpu.pc != kCallReturnSentinel)
            {
                std::ostringstream why;
                why << "[IOP:call] 0x" << std::hex << address << " stopped at pc=0x" << cpu.pc << " ra=0x" << cpu.gpr[31]
                    << std::dec << " stopped=" << cpu.stopped << " yielded=" << cpu.yielded
                    << " instructions=" << (totalInstructions - executedBefore) << " recent=";
                for (uint32_t i = 0; i < 24u; ++i)
                    why << std::hex << recentPcs[(recentPcPos + 64u - 24u + i) & 63u] << " ";
                why << " cop0: status=" << std::hex << cpu.cop0[12] << " cause=" << cpu.cop0[13] << " epc=" << cpu.cop0[14] << " badvaddr=" << cpu.cop0[8];
                why << " code@pc=";
                for (uint32_t i = 0; i < 24u; ++i)
                    why << std::hex << memory.read32((cpu.pc & 0x1FFFFFFFu) - 8u * 4u + 4u * i) << " ";
                log(LogLevel::Warning, why.str());
            }
            return cpu.gpr[2];
        }

        uint64_t failedRpcImports = 0;
        bool executeRpcFunction(uint32_t address, uint32_t a0, uint32_t a1,
                                uint32_t a2, uint32_t gp, uint32_t &result) override
        {
            const auto failures = failedRpcImports;
            bool returned = false;
            result = callFunction(address, a0, a1, a2, 0, gp, kMaxCallInstructions, &returned);
            if (!returned || failures != failedRpcImports)
                log(LogLevel::Warning, "[IOP:rpc] server function 0x" + [&] { std::ostringstream o; o << std::hex << address; return o.str(); }() +
                                           " fn=" + std::to_string(a0) + (returned ? " returned but used unimplemented imports" : " did not return within its instruction budget"));
            return returned && failures == failedRpcImports;
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                {
                    if (!intrman.dispatchInterrupt(irq, *this))
                    {
                        static uint32_t dropped = 0;
                        if (dropped++ < 200u)
                            log(LogLevel::Warning, "[IOP:dma-irq-dropped] irq=" + std::to_string(irq) + " completed while the IRQ had no enabled handler (cycle " + std::to_string(totalCycles) + ")");
                    }
                }
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void serviceVblankHandlers()
        {
            if (servicingGuestCallbacks || !vblank.hasHandlers() || vblank.nextEventCycle(UINT64_MAX) > totalCycles)
                return;
            const std::vector<IopVblank::Handler> due = vblank.collectDue(totalCycles);
            if (due.empty())
                return;
            servicingGuestCallbacks = true;
            try
            {
                for (const IopVblank::Handler &handler : due)
                    (void)callFunction(handler.function, handler.argument, 0u, 0u, 0u, handler.gp, 100000u);
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                // The EE hands the IOP a budget of cycles. An IOP thread that runs past its slice (interrupt-deferred sections, a cycle that
                // must advance, an import that completes late) leaves totalCycles ahead of the budget; the budget is cumulative so the
                // overshoot is repaid out of the next grants instead of being added on top (else the IOP clock, which paces the SPU2,
                // drifts ahead of the EE clock: 1-2 % after a minute, enough to drain the ADX ring of a movie).
                // Time spent outside the budget (module start-up, boot, synchronous calls) is not repaid beyond kMaxRepayCycles.
                constexpr uint64_t kMaxRepayCycles = 400000u;
                if (totalCycles > cycleBudget + kMaxRepayCycles)
                    cycleBudget = totalCycles;
                cycleBudget += cycles;
                const uint64_t target = cycleBudget;
                while (totalCycles < target)
                {
                    const bool deferred = memory.interruptControl() == 0u && maskedRun < kMaxMaskedRun;
                    if (!deferred)
                    {
                        servicePendingDmaInterrupts();
                        servicePendingGuestCallbacks();
                        serviceVblankHandlers();
                        timrman.serviceDue(totalCycles, *this);
                    }
                    IopThread *next = kernel.beginNextReady(totalCycles);
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = vblank.nextEventCycle(nextWake);
                        nextWake = timrman.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    if (profileEnabled && (++sliceCounter % 400000u) == 0u && threadDumps < 24u)
                    {
                        ++threadDumps;
                        log(LogLevel::Info, "[IOP:sample] slices=" + std::to_string(sliceCounter) + " running id=" + std::to_string(next->id) +
                                                " pc=0x" + [&] { std::ostringstream o; o << std::hex << next->cpu.pc; return o.str(); }() +
                                                kernel.describeThreads() + [&] {
                                                    std::vector<std::pair<uint64_t, std::string>> top;
                                                    for (const auto &entry : importCounts) top.emplace_back(entry.second, entry.first);
                                                    std::sort(top.rbegin(), top.rend());
                                                    std::string text;
                                                    {
                                                        std::vector<std::pair<uint64_t, uint32_t>> pcs;
                                                        for (const auto &entry : pcCounts) pcs.emplace_back(entry.second, entry.first);
                                                        std::sort(pcs.rbegin(), pcs.rend());
                                                        std::ostringstream pcText;
                                                        pcText << " pcs:" << std::hex;
                                                        for (size_t i = 0; i < pcs.size() && i < 24; ++i) pcText << " " << pcs[i].second << "x" << std::dec << pcs[i].first << std::hex;
                                                        text = pcText.str();
                                                    }
                                                    {
                                                        std::vector<std::pair<uint64_t, uint32_t>> ras;
                                                        for (const auto &entry : raCounts) ras.emplace_back(entry.second, entry.first);
                                                        std::sort(ras.rbegin(), ras.rend());
                                                        std::ostringstream raText;
                                                        raText << " ras:" << std::hex;
                                                        for (size_t i = 0; i < ras.size() && i < 12; ++i) raText << " " << ras[i].second << "x" << std::dec << ras[i].first << std::hex;
                                                        text += raText.str();
                                                    }
                                                    text += " imports:";
                                                    for (size_t i = 0; i < top.size() && i < 10; ++i) text += " " + top[i].second + "x" + std::to_string(top[i].first);
                                                    return text;
                                                }());
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    // A thread inside an interrupts-disabled section keeps the CPU until it leaves it (see deferInterrupts).
                    while (!next->cpu.stopped && !next->cpu.yielded && deferInterrupts(next->cpu))
                        runCpu(next->cpu, kDefaultSlice);
                    if (static const bool overshootWho = std::getenv("PS2X_IOP_OVERSHOOT") != nullptr; overshootWho && totalCycles > target + 100u)
                        std::fprintf(stderr, "[iop-overshoot-slice] thread=%d pc=%x ran=%llu past=%llu interrupts=%u\n", static_cast<int>(next->id), next->cpu.pc, static_cast<unsigned long long>(totalCycles - before), static_cast<unsigned long long>(totalCycles - target), memory.interruptControl());
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (totalCycles == before)
                        ++totalCycles;
                }
                // Real-time audio (M3): deliver the mix about every 2 ms of emulated time, independent of AutoDMA traffic.
                if (totalCycles - lastAudioPumpCycle >= 1536u)
                {
                    lastAudioPumpCycle = totalCycles;
                    memory.pumpAudioOutput();
                }
                // PS2X_IOP_OVERSHOOT=1 (diagnostic): growth of the debt (cycles run past the cumulative EE grant) after a call.
                if (static const bool overshootLog = std::getenv("PS2X_IOP_OVERSHOOT") != nullptr; overshootLog)
                {
                    static uint64_t previousOver = 0u;
                    const uint64_t over = totalCycles > target ? totalCycles - target : 0u;
                    if (over > previousOver + 100u)
                        std::fprintf(stderr, "[iop-overshoot] cyc=%llu over=%llu grew=%llu grant=%llu\n", static_cast<unsigned long long>(totalCycles), static_cast<unsigned long long>(over), static_cast<unsigned long long>(over - previousOver), static_cast<unsigned long long>(cycles));
                    previousOver = over;
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                {
                    std::ostringstream text;
                    text << "[IOP] module arena exhausted path='" << path << "' image=0x" << std::hex << image.size()
                         << " cursor=0x" << moduleCursor << " arenaEnd=0x" << IopMemory::HeapBase;
                    log(LogLevel::Error, text.str());
                }
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            // Debug aid: PS2X_IOP_DUMP_DIR=<dir> writes each loaded module's relocated RAM image
            // (file name carries the load base) so it can be disassembled offline.
            if (const char *dumpDir = std::getenv("PS2X_IOP_DUMP_DIR"))
            {
                std::vector<uint8_t> bytes(loaded.size);
                if (memory.readRam(loaded.base, bytes.data(), bytes.size()))
                {
                    char name[96];
                    std::snprintf(name, sizeof(name), "/mod%02u_base%06x_entry%06x.bin", module.id, loaded.base, loaded.entry);
                    std::ofstream out(std::string(dumpDir) + name, std::ios::binary);
                    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                }
            }

            uint32_t args = 0u;
            if (arguments && argumentSize)
            {
                args = allocate(argumentSize + 1u, 16u);
                if (args)
                {
                    writeRam(args, arguments, argumentSize);
                    write8(args + argumentSize, 0u);
                }
            }
            const uint32_t startResult = callFunction(module.entry, argumentSize, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            // Module name from the .iopmod section (type 0x70000080): {modinfo, entry, gp, text, data, bss, u16 version, name[]}.
            std::string moduleName;
            if (image.size() >= 52u)
            {
                auto rd32 = [&](size_t at) { uint32_t v = 0; if (at + 4 <= image.size()) std::memcpy(&v, image.data() + at, 4); return v; };
                const uint32_t shoff = rd32(32);
                const uint32_t shentsize = image[46] | (image[47] << 8);
                const uint32_t shnum = image[48] | (image[49] << 8);
                for (uint32_t i = 0; i < shnum && shentsize >= 40u; ++i)
                {
                    const size_t sh = static_cast<size_t>(shoff) + static_cast<size_t>(i) * shentsize;
                    if (rd32(sh + 4) != 0x70000080u) continue;
                    const size_t content = rd32(sh + 16);
                    for (size_t k = content + 26u; k < image.size() && k < content + 26u + 24u && image[k]; ++k)
                        moduleName.push_back(static_cast<char>(image[k]));
                    break;
                }
            }
            modules[result.moduleId].irxName = moduleName;
            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId << " name=" << moduleName
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            log(LogLevel::Info, "[IOP:threads]" + kernel.describeThreads());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
            {
                std::array<uint8_t, 16> head{};
                const bool readable = host.readGuest(guestAddress, head.data(), head.size());
                std::ostringstream why;
                why << "[IOP:modload] module buffer @0x" << std::hex << guestAddress << " is not a loadable ELF (readable=" << readable << " head=";
                for (uint8_t byte : head) why << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                why << ")";
                host.log(LogLevel::Warning, why.str());
                return {true, -1, -1};
            }
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleFromIopMemory(uint32_t iopAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            const auto read = [this](uint32_t address, void *destination, size_t size) {
                return size != 0u && address < IopMemory::RamSize && size <= IopMemory::RamSize - address && memory.readRam(address, destination, size);
            };
            if (!IopModuleLoader::readElfFromMemory(read, iopAddress, image))
            {
                std::array<uint8_t, 16> head{};
                const bool readable = read(iopAddress, head.data(), head.size());
                std::ostringstream why;
                why << "[IOP:modload] module buffer in IOP RAM @0x" << std::hex << iopAddress << " is not a loadable ELF (readable=" << readable << " head=";
                for (uint8_t byte : head) why << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
                why << ")";
                host.log(LogLevel::Warning, why.str());
                return {true, -1, -1};
            }
            std::ostringstream tag;
            tag << "iopbuffer@0x" << std::hex << iopAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint64_t sliceCounter = 0;
        // PS2X_IOP_PROFILE=1: sample IOP thread states, hot PCs and import counts (diagnostics only).
        const bool profileEnabled = std::getenv("PS2X_IOP_PROFILE") != nullptr;
        std::map<std::string, uint64_t> importCounts;
        std::set<std::string> missingImportsReported;
        std::set<std::string> jumpImportsReported;
        std::map<uint32_t, uint64_t> pcCounts;
        std::map<uint32_t, uint64_t> raCounts;
        std::array<uint32_t, 64> recentPcs{};
        uint32_t recentPcPos = 0;
        uint32_t threadDumps = 0;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t cycleBudget = 0; // cumulative cycles granted by the EE (see runCycles)
        uint64_t lastAudioPumpCycle = 0;
        uint64_t totalInstructions = 0;
        uint64_t eeCycleCarry = 0;
        uint64_t pendingIopCycles = 0;
        // Brings the IOP up to date with the EE cycles that runEeCycles has accumulated but not executed yet. Every
        // interaction with the IOP (RPC, SIF transfers, counters) calls it so batching never changes visible ordering.
        void flushPendingCycles() noexcept
        {
            if (pendingIopCycles == 0u)
                return;
            const uint64_t run = pendingIopCycles;
            pendingIopCycles = 0u;
            runCycles(run);
        }
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    CdvdDevice::Snapshot IopEmulator::cdvdSnapshot() const
    {
        return m_impl->cdvd.deviceSnapshot();
    }

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleFromIopMemory(uint32_t iopAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleFromIopMemory(iopAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        // The EE reports its progress in tiny steps (one guest dispatch ~ 1 IOP cycle). Running the IOP scheduler for
        // every step costs more than the IOP code itself, so the cycles are accumulated and executed in batches. The IOP
        // never lags the EE by more than kIopBatchCycles (~1.7 us of real time).
        constexpr uint64_t kIopBatchCycles = 64u;
        m_impl->pendingIopCycles += iopCycles;
        if (m_impl->pendingIopCycles >= kIopBatchCycles)
        {
            const uint64_t run = m_impl->pendingIopCycles;
            m_impl->pendingIopCycles = 0u;
            m_impl->runCycles(run);
        }
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        m_impl->flushPendingCycles();
        // Physical servers keep priority. Otherwise use the boot HLE control
        // provider backed by the very same CDVDMAN instance as IOP imports.
        if (!m_impl->rpc.hasServer(request.sid) && m_impl->cdvd.hasControlServer(request.sid))
            return m_impl->cdvd.handleControlRpc(request);
        if (!m_impl->rpc.hasServer(request.sid) && isIopHeapSid(request.sid))
            return handleIopHeapRpc(m_impl->host, m_impl->memory, request);
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid) || m_impl->cdvd.hasControlServer(sid) || isIopHeapSid(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->flushPendingCycles();
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        m_impl->flushPendingCycles();
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        m_impl->flushPendingCycles();
        return m_impl->totalInstructions;
    }

    std::string IopEmulator::moduleIrxName(int32_t moduleId) const
    {
        const auto found = m_impl->modules.find(moduleId);
        return found == m_impl->modules.end() ? std::string{} : found->second.irxName;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
