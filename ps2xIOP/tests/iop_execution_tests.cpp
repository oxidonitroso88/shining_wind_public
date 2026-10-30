// Focused regression tests for the IOP CPU-core fixes adopted from feature/performance-patch-1 (ran-j/PS2Recomp):
// shared instruction fetch, not-taken branch delay slot (BD/EPC), pending load on exception, import-stub metadata freshness,
// plus DeleteThread of the running thread (node must survive until the time slice ends).
#include "iop_compat_test_support.h"
#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_imports.h"

namespace
{
    using namespace iop_test;
    using namespace ps2x::iop::detail;

    void instructionChanges()
    {
        IopMemory memory;
        IopCpuCore core(memory);
        IopImportRegistry imports(memory);
        IopCpuState cpu{};
        const auto step = [&]()
        {
            const uint32_t instruction = memory.read32(cpu.pc);
            require(!imports.decode(cpu.pc, instruction), "ordinary instruction mistaken for an import");
            return core.executeInstruction(cpu, instruction);
        };
        cpu.pc = 0x80001000;
        memory.write32(0x1000, 0x24020011); // addiu v0, zero, 0x11
        require(step() && cpu.gpr[2] == 0x11, "first instruction failed");
        cpu.pc = 0x80001000;
        memory.write32(0x1000, 0x24020022);
        require(step() && cpu.gpr[2] == 0x22, "modified instruction was not refetched");
        cpu.pc = 0x1000;
        memory.write32(0x1000, 0x8c020800); // lw v0, 0x800(zero)
        memory.write32(0x1004, 0x24430001); // addiu v1, v0, 1 (sees old v0)
        memory.write32(0x800, 0x44);
        require(step() && cpu.gpr[2] == 0x22, "load delay changed");
        require(step() && cpu.gpr[2] == 0x44 && cpu.gpr[3] == 0x23,
                "instruction sharing changed load delay semantics");
        cpu.pc = 0x1000;
        memory.write32(0x1000, 0x10000001); // beq zero, zero, +1
        memory.write32(0x1004, 0x24020033);
        require(step() && cpu.pc == 0x1004 && cpu.branchPending, "branch delay slot was skipped");
        require(step() && cpu.pc == 0x1008 && cpu.gpr[2] == 0x33, "branch delay slot did not execute");
        cpu = {};
        cpu.pc = 0x1000;
        memory.write32(0x1000, 0x14200001);
        memory.write32(0x1004, 0x8c020001);
        require(step() && cpu.branchPending, "untaken branch must retain its delay slot");
        cpu.pendingLoad = true;
        cpu.pendingLoadReg = 9;
        cpu.pendingLoadValue = 55;
        require(step() && cpu.exception && cpu.cop0[14] == 0x1000 &&
                    (cpu.cop0[13] & 0x8000007c) == 0x80000010 && cpu.cop0[8] == 1 &&
                    cpu.gpr[9] == 55 && !cpu.pendingLoad,
                "delay-slot address exception must preserve EPC, BD and the prior load");
    }

    void modifiedImportStub()
    {
        IopMemory memory;
        IopImportRegistry imports(memory);
        memory.write32(0x1000, 0x41e00000);
        memory.write16(0x1008, 0x0101);
        constexpr char name[8] = "tstlib";
        require(memory.writeRam(0x100c, name, sizeof(name)), "import name write failed");
        memory.write32(0x1014, 0x03e00008);
        memory.write32(0x1018, 0x24000003);
        const auto decode = [&]() { return imports.decode(0x80001014, memory.read32(0x80001014)); };
        auto call = decode();
        require(call && call->ordinal == 3 && call->version == 0x0101, "shared fetch lost import metadata");
        memory.write16(0x1008, 0x0102);
        memory.write32(0x1018, 0x24000007);
        call = decode();
        require(call && call->ordinal == 7 && call->version == 0x0102, "import metadata became stale");
        memory.write32(0x1014, 0x24020011);
        require(!decode(), "patched import stub still dispatched as an import");
    }

    void deleteRunningThread()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        IopCpuState caller{};
        const auto call = [&](uint16_t ordinal, uint32_t a0)
        {
            caller.gpr[4] = a0;
            require(kernel.dispatchThreadImport(ordinal, caller, 0), "thread import not handled");
            return caller.gpr[2];
        };
        constexpr uint32_t descriptor = 0x2000;
        memory.write32(descriptor + 0, 0x02000000); // attr (TH_C)
        memory.write32(descriptor + 4, 0);
        memory.write32(descriptor + 8, 0x1000);     // entry
        memory.write32(descriptor + 12, 0x400);     // stack size
        memory.write32(descriptor + 16, 0x20);      // priority
        const uint32_t idA = call(4, descriptor);
        const uint32_t idB = call(4, descriptor);
        require(idA != 0 && idB != 0 && static_cast<int32_t>(idA) > 0 && kernel.threadCount() == 2, "CreateThread failed");
        require(call(6, idA) == 0, "StartThread failed");
        IopThread *running = kernel.beginNextReady(0);
        require(running && static_cast<uint32_t>(running->id) == idA, "thread A should be running");
        // Deleting a different dormant thread frees it immediately.
        require(call(5, idB) == 0 && kernel.threadCount() == 1, "DeleteThread of a dormant thread must remove it");
        // Deleting the running thread must not free the node under the scheduler: it stops and is reaped afterwards.
        IopCpuState &self = running->cpu;
        self.gpr[4] = idA;
        require(kernel.dispatchThreadImport(5, self, 0) && self.gpr[2] == 0, "DeleteThread(self) failed");
        require(kernel.threadCount() == 1 && self.stopped && self.yielded && running->state == IopThreadState::Dead,
                "deleting the running thread must mark it dead and stop it, not erase it");
        kernel.endTimeslice(*running, 0xFFFFFFF0u);
        require(kernel.threadCount() == 0, "the dead running thread must be reaped after its time slice");
    }
}

int main()
{
    const Test tests[] = {
        {"Modified instructions, load delay, delay-slot exception state", instructionChanges},
        {"Modified import stub and metadata", modifiedImportStub},
        {"DeleteThread of the running thread", deleteRunningThread},
    };
    return run(tests);
}
