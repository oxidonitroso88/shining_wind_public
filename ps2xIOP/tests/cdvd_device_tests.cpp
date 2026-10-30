#include "ps2x/iop/cdvd_device.h"
#include "iop_compat_test_support.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/core/iop_memory.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_cpu.h"
#include <iostream>

using namespace ps2x::iop;
int main()
{
    try
    {
        size_t checks = 0;
        auto check = [&](bool ok, const char *why) { iop_test::require(ok, why); ++checks; };
        using Phase = CdvdDevice::Phase;
        CdvdDevice device;
        check(device.init(0).phase == Phase::Rejected, "unprepared Init succeeded");
        device.configureSource(CdvdDevice::Source::ImageCandidate, "not-a-real-disc");
        check(device.diskReady(0).phase == Phase::Rejected, "path implied ready");
        device.prepare();
        check(device.init(0).value == 1, "stable Init did not complete");
        check(device.setMode(2).value == 1 && device.snapshot().selectedMode == 2, "mode lost");
        check(device.snapshot().sourcePath == "not-a-real-disc", "mode changed source");
        check(device.diskReady(0).value == 2 && device.diskReady(1).value == 2, "idle readiness");
        auto command = device.begin(CdvdDevice::Work::Command);
        check(device.diskReady(0).value == 2, "mode0 incorrectly became mode1 polling");
        check(device.diskReady(1).value == 6, "busy query false ready");
        auto init = device.init(0);
        check(init.phase == Phase::Pending && !init.value, "pending Init invented result");
        check(device.init(1).value == 1, "Init1 waited like Init0");
        check(device.init(5).phase == Phase::Rejected, "unsupported active Exit pretended to finish");
        check(device.finish(command), "work completion rejected");
        check(device.poll(init.token, true).value == 1, "Init did not finish on real event");
        check(device.poll(init.token).phase == Phase::Rejected, "completion consumed twice");
        check(!device.finish(command), "work completed twice");
        auto detection = device.begin(CdvdDevice::Work::ReadDuringDetection);
        auto wait = device.diskReady(0);
        check(wait.phase == Phase::Pending && !wait.value, "detection wait fabricated result");
        check(device.finish(detection) && device.poll(wait.token, true).value == 2, "detection completion");
        detection = device.begin(CdvdDevice::Work::ReadDuringDetection);
        wait = device.diskReady(0);
        device.reset();
        check(device.poll(wait.token).phase == Phase::Cancelled, "reset retained pending request");
        check(!device.finish(detection), "old epoch work accepted");
        check(!device.snapshot().prepared && device.snapshot().selectedMode == 0, "reset leaked state");
        device.configureSource(CdvdDevice::Source::Unconfigured, {});
        device.prepare();
        check(device.init(0).value == 1 && device.diskReady(0).value == 2, "empty device cannot accept commands");
        check(device.snapshot().source == CdvdDevice::Source::Unconfigured, "Init invented mounted medium");
        check(device.init(5).value == 1 && device.snapshot().session == CdvdDevice::Session::Uninitialized, "Exit failed");
        check(device.diskReady(8).phase == Phase::Rejected, "fabricated MMIO status");
        check(device.init(99).phase == Phase::Rejected, "unknown init accepted");

        iop_test::Host host;
        detail::IopMemory memory;
        detail::IopKernel kernel(memory);
        detail::IopCdvd imports(host, memory, kernel);
        imports.reset();
        detail::IopCpuState cpu{};
        cpu.gpr[4] = 0;
        check(imports.dispatchImport(4, cpu) && cpu.gpr[2] == 1, "real Init import failed");
        cpu.gpr[4] = 2;
        check(imports.dispatchImport(75, cpu) && cpu.gpr[2] == 1 &&
              imports.deviceSnapshot().selectedMode == 2, "Mmode import not shared");
        cpu.gpr[4] = 0;
        check(imports.dispatchImport(13, cpu) && cpu.gpr[2] == 2, "DiskReady0 import failed");
        cpu.gpr[4] = 1;
        check(imports.dispatchImport(13, cpu) && cpu.gpr[2] == 2, "DiskReady1 import failed");
        cpu.gpr[4] = 8;
        check(!imports.dispatchImport(13, cpu), "raw-status import fabricated a value");
        cpu.gpr[4] = 5;
        check(imports.dispatchImport(4, cpu) &&
              imports.deviceSnapshot().session == CdvdDevice::Session::Uninitialized, "Exit import failed");
        imports.reset();
        check(imports.deviceSnapshot().selectedMode == 0, "import reset leaked mode");
        IopSubsystem iop(host);
        check(iop.cdvdSnapshot().prepared, "real synchronous backend not prepared");
        check(iop.canBindRpc(0x80000592), "prepared backend lacks authorized control provider");
        auto before = iop.cdvdSnapshot().generation;
        iop.reset();
        check(iop.cdvdSnapshot().generation > before && iop.cdvdSnapshot().prepared,
              "backend reset did not start a new prepared generation");
        std::cout << "PASS " << checks << " CDVD model checks\n";
        return 0;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
