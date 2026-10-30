"""Builds the Windows port of Shining Wind (SLPM-66671) from YOUR OWN disc image. One command, nothing from the game is shipped with this repository.

    python tools/port/make_port.py --iso "C:\\path\\Shining Wind.iso" [--work port-work] [--dist ShiningWind-port] [--jobs 2] [--stage all]

Stages (run in this order; `--stage` runs one of them, they are re-runnable):
  check      tools present (git, CMake, Visual Studio 2022 C++ build tools, Python 3.9+), free disk space
  extract    ELF, OVERLAY/ and SYSTEM.CNF from the image into <work>/game; the ELF must be the supported edition (size + SHA-256)
  recomp     builds the recompiler (ps2_recomp) into <work>/recomp-build
  generate   recompiles the main program with data/shining_wind_function_map.csv + data/shining_wind.toml.in into ps2xRuntime/src/runner
  overlays   the 11 code overlays and the supplementary functions (tools/overlay) into ps2xRuntime/src/runner_overlays
  gs         fetches parallel-gs (pinned commit), applies third_party_patches/parallel-gs (0001-0004: HD texture replacement and x4 output)
             and regenerates its embedded shaders with slangmosh
  build      configures and builds ps2EntryRunner (about 25-30 minutes, 2 jobs) into <work>/runtime-build
  package    copies the executable and its DLLs to <dist> (ShiningWind.exe) with the licence files

The generated sources (about 300 MB) are derived from the game: they stay on your machine (git-ignored), do not publish them.
Needs about 8 GB of free disk space for the whole run (sources 0.3 GB, parallel-gs 0.2 GB, build 4-5 GB, tools 0.5 GB).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF_SIZE = 3270888
ELF_SHA256 = "805c38303d6ae79bd5e425cbcf49f3fb8fd291b9d2ea8f577c1e5b2f04095437"
STAGES = ["check", "extract", "recomp", "generate", "overlays", "gs", "build", "package"]
RUNNER = ROOT / "ps2xRuntime" / "src" / "runner"
OVERLAYS = ROOT / "ps2xRuntime" / "src" / "runner_overlays"
INCLUDE = ROOT / "ps2xRuntime" / "include"
PGS_COMMIT = "3a66c1976170cbc2cb53a3593fabbc7c4b2ccfbd"
PGS_DIR = ROOT / "third_party" / "parallel-gs"
MARK = ".generated-by-make_port"


class Ctx:
    def __init__(self, a):
        self.a = a
        self.work = Path(a.work).resolve()
        self.dist = Path(a.dist).resolve()
        self.game = self.work / "game"
        self.logs = self.work / "logs"
        self.jobs = a.jobs
        self.cmake = a.cmake or shutil.which("cmake") or self.find_cmake()
        self.recomp_exe = self.work / "recomp-build" / "ps2xRecomp" / "Release" / "ps2_recomp.exe"
        self.logs.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def find_cmake():
        for base in (os.environ.get("ProgramFiles", ""), os.environ.get("ProgramFiles(x86)", "")):
            p = Path(base) / "CMake" / "bin" / "cmake.exe"
            if p.exists():
                return str(p)
        for vsw in (Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe",):
            if vsw.exists():  # the CMake that ships with Visual Studio
                out = subprocess.run([str(vsw), "-latest", "-products", "*", "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
                p = Path(out) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
                if out and p.exists():
                    return str(p)
        return None

    def run(self, name, cmd, cwd=None, check=True):
        log = self.logs / (name + ".log")
        print("  $ " + " ".join(str(c) for c in cmd))
        t = time.time()
        with open(log, "w", encoding="utf-8", errors="replace") as f:
            r = subprocess.run([str(c) for c in cmd], cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
        print("    exit %d in %.0f s (log: %s)" % (r.returncode, time.time() - t, log))
        if check and r.returncode != 0:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-15:]
            sys.exit("failed: %s\n%s" % (name, "\n".join(tail)))
        return r.returncode


def free_gb(path):
    p = Path(path)
    while not p.exists():
        p = p.parent
    return shutil.disk_usage(p).free / 2 ** 30


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def stage_check(c):
    problems = []
    for tool in ("git",):
        if not shutil.which(tool):
            problems.append("%s not found in PATH" % tool)
    if not c.cmake:
        problems.append("CMake not found (install CMake 3.20+ or pass --cmake)")
    if sys.version_info < (3, 9):
        problems.append("Python 3.9+ needed")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        out = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        if not out:
            problems.append("Visual Studio 2022 with 'Desktop development with C++' not found")
    else:
        problems.append("Visual Studio 2022 (Build Tools) not found")
    if not (ROOT / "data" / "shining_wind_function_map.csv").exists():
        problems.append("data/shining_wind_function_map.csv missing (incomplete checkout)")
    gb = free_gb(c.work)
    print("  free disk space on the work drive: %.1f GB" % gb)
    if gb < c.a.min_free_gb:
        problems.append("less than %.0f GB free on the work drive (%.1f GB)" % (c.a.min_free_gb, gb))
    for p in problems:
        print("  PROBLEM: " + p)
    if problems:
        sys.exit("check failed")
    print("  ok (cmake: %s)" % c.cmake)


def stage_extract(c):
    sys.path.insert(0, str(ROOT / "tools" / "port"))
    import iso_extract
    iso = Path(c.a.iso)
    if not iso.is_file():
        sys.exit("--iso: file not found: %s" % iso)
    if (c.game / "SLPM_666.71").exists():
        print("  already extracted: %s" % c.game)
    else:
        c.game.mkdir(parents=True, exist_ok=True)
        # the ELF first and verified BEFORE anything else is written
        img = iso_extract.Image(str(iso))
        files = dict(img.walk())
        elf = next((e for p, e in files.items() if p.upper() == "SLPM_666.71"), None)
        if elf is None:
            sys.exit("SLPM_666.71 is not in this image: it is not the supported edition (Shining Wind, SLPM-66671)")
        if elf["size"] != ELF_SIZE:
            sys.exit("SLPM_666.71 has %d bytes, expected %d: unsupported edition" % (elf["size"], ELF_SIZE))
        iso_extract.main([str(iso), str(c.game), "--only", "SLPM_666.71", "OVERLAY", "SYSTEM.CNF"])
    h = sha256(c.game / "SLPM_666.71")
    if h != ELF_SHA256:
        sys.exit("the ELF SHA-256 is %s, expected %s: unsupported edition or a modified disc" % (h, ELF_SHA256))
    n = len(list((c.game / "OVERLAY").glob("OVL*.BIN")))
    if n != 11:
        sys.exit("expected 11 overlays, found %d" % n)
    print("  ELF verified (SLPM-66671), %d overlays" % n)


def stage_recomp(c):
    if c.recomp_exe.exists():
        print("  already built: %s" % c.recomp_exe)
        return
    bd = c.work / "recomp-build"
    c.run("recomp-configure", [c.cmake, "-S", ROOT, "-B", bd, "-G", "Visual Studio 17 2022", "-A", "x64", "-DPS2X_BUILD_RECOMP=ON", "-DPS2X_BUILD_RUNTIME=OFF",
                                "-DPS2X_BUILD_ANALYZER=OFF", "-DPS2X_BUILD_TEST=OFF", "-DPS2X_BUILD_STUDIO=OFF"])
    c.run("recomp-build", [c.cmake, "--build", bd, "--config", "Release", "--target", "ps2_recomp", "--", "-m:%d" % c.jobs, "-p:CL_MPCount=%d" % c.jobs])
    if not c.recomp_exe.exists():
        sys.exit("ps2_recomp.exe was not produced")


def fresh_dir(d):
    """An output folder this tool made earlier is replaced; a folder with other content is never touched."""
    d = Path(d)
    if d.exists() and any(d.iterdir()):
        if not (d / MARK).exists():
            sys.exit("%s is not empty and was not made by make_port.py: refusing to touch it" % d)
        for x in d.iterdir():
            shutil.rmtree(x) if x.is_dir() else x.unlink()
    d.mkdir(parents=True, exist_ok=True)


def fill_toml(c, out_dir, dest):
    t = (ROOT / "data" / "shining_wind.toml.in").read_text(encoding="utf-8")
    t = (t.replace("@ELF@", (c.game / "SLPM_666.71").as_posix()).replace("@OUT@", Path(out_dir).as_posix())
         .replace("@CSV@", (ROOT / "data" / "shining_wind_function_map.csv").as_posix()))
    Path(dest).write_text(t, encoding="utf-8", newline="\n")


def stage_generate(c):
    gen = c.work / "gen-main"
    if gen.exists():
        shutil.rmtree(gen)
    gen.mkdir(parents=True)
    toml = gen / "main.toml"
    fill_toml(c, gen / "output", toml)
    c.run("generate-main", [c.recomp_exe, toml], cwd=gen)
    out = gen / "output"
    if not (out / "register_functions.cpp").exists():
        sys.exit("the recompiler did not produce register_functions.cpp (see %s)" % (c.logs / "generate-main.log"))
    fresh_dir(RUNNER)
    n = 0
    for f in out.iterdir():
        shutil.copyfile(f, RUNNER / f.name)
        n += 1
    shutil.copyfile(out / "ps2_recompiled_functions.h", INCLUDE / "ps2_recompiled_functions.h")
    if (out / "ps2_recompiled_stubs.h").exists():
        shutil.copyfile(out / "ps2_recompiled_stubs.h", INCLUDE / "ps2_recompiled_stubs.h")
    (RUNNER / MARK).write_text("generated by tools/port/make_port.py\n")
    print("  %d files -> %s" % (n, RUNNER))


def stage_overlays(c):
    ob = c.work / "overlay-build"
    if ob.exists():
        shutil.rmtree(ob)
    ob.mkdir(parents=True)
    py = sys.executable
    tool = ROOT / "tools" / "overlay" / "ovl_tool.py"
    common = ["--elf", c.game / "SLPM_666.71", "--csv", ROOT / "data" / "shining_wind_function_map.csv", "--overlays", c.game / "OVERLAY"]
    install = [py, "-I", ROOT / "tools" / "overlay" / "ovl_install.py", "--build", ob, "--dest", OVERLAYS]
    c.run("overlays-build", [py, "-I", tool, "build", "--out", ob] + common)
    c.run("overlays-supplement", [py, "-I", tool, "supplement", "--out", ob] + common)
    names = sorted(p.name for p in ob.iterdir() if (p / (p.name + ".toml")).exists())
    for n in names:
        c.run("recomp-" + n, [c.recomp_exe, ob / n / (n + ".toml")], cwd=ob)
    fresh_dir(OVERLAYS)
    c.run("overlays-install1", install)
    # the cross-function branch targets are found by reading the generated code, which has to be in place first
    c.run("overlays-crossfix", [py, "-I", tool, "crossfix", "--out", ob, "--runner", RUNNER, "--install-dir", OVERLAYS, "--recomp", c.recomp_exe] + common)
    c.run("overlays-install2", install)
    (OVERLAYS / MARK).write_text("generated by tools/port/make_port.py\n")
    print("  %d groups: %s; %d files in %s" % (len(list(ob.glob("*/generated"))), " ".join(p.name for p in sorted(ob.iterdir()) if p.is_dir()), len(list(OVERLAYS.iterdir())), OVERLAYS))


def git(c, name, *args, cwd=None, check=True):
    return c.run(name, ["git"] + list(args), cwd=cwd, check=check)


def stage_gs(c):
    ps = shutil.which("powershell") or shutil.which("pwsh")
    if not (PGS_DIR / "gs" / "gs_interface.hpp").exists():
        if not ps:
            sys.exit("PowerShell not found (needed by tools/perf/fetch_parallel_gs.ps1)")
        c.run("gs-fetch", [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ROOT / "tools" / "perf" / "fetch_parallel_gs.ps1"])
    else:
        print("  parallel-gs already present: %s" % PGS_DIR)
    base = ROOT / "third_party_patches" / "parallel-gs"
    for p in (base / "experimental" / "0003-x4-scanout-output.patch", base / "experimental" / "0004-x4-uncommitted-work-20261027.patch"):
        if c.run("gs-check-" + p.name, ["git", "apply", "--check", "--reverse", p], cwd=PGS_DIR, check=False) == 0:
            print("  already applied: %s" % p.name)
            continue
        c.run("gs-apply-" + p.name, ["git", "apply", "--whitespace=nowarn", p], cwd=PGS_DIR)
    # shaders: slangmosh from Granite (needs two more submodules than the fetch script gets)
    gr = PGS_DIR / "Granite"
    for sub in ("third_party/spirv-cross", "third_party/shaderc"):
        if not (gr / sub).exists() or not any((gr / sub).iterdir()):
            git(c, "gs-submodule-" + sub.split("/")[-1], "submodule", "update", "--init", "--depth", "1", sub, cwd=gr)
    sb = c.work / "slangmosh-build"
    exe = sb / "Release" / "slangmosh.exe"
    if not exe.exists():
        c.run("slangmosh-configure", [c.cmake, "-S", gr, "-B", sb, "-G", "Visual Studio 17 2022", "-A", "x64", "-DGRANITE_PLATFORM=null", "-DGRANITE_RENDERER=OFF", "-DGRANITE_TOOLS=ON",
                                      "-DGRANITE_VULKAN_SPIRV_CROSS=ON", "-DGRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER=ON", "-DGRANITE_VULKAN_SYSTEM_HANDLES=OFF",
                                      "-DGRANITE_VULKAN_FOSSILIZE=OFF", "-DGRANITE_SHIPPING=ON", "-DGRANITE_AUDIO=OFF", "-DGRANITE_FFMPEG=OFF", "-DGRANITE_INSTALL_TARGETS=OFF"])
        c.run("slangmosh-build", [c.cmake, "--build", sb, "--config", "Release", "--target", "slangmosh", "--", "-m:%d" % c.jobs, "-p:CL_MPCount=%d" % c.jobs])
    c.run("slangmosh-run", [exe, "--namespace", "ParallelGS", "shaders/slangmosh.json", "--output", "shaders/slangmosh.hpp", "--output-interface", "shaders/slangmosh_iface.hpp", "-O", "--strip"],
          cwd=PGS_DIR / "gs")
    print("  parallel-gs ready (x4 patches applied, shaders regenerated)")


def stage_build(c):
    bd = c.work / "runtime-build"
    gb = free_gb(c.work)
    if gb < min(5, c.a.min_free_gb):
        sys.exit("only %.1f GB free: the build needs about 5 GB" % gb)
    c.run("build-configure", [c.cmake, "-S", ROOT / "ps2xRuntime", "-B", bd, "-G", "Visual Studio 17 2022", "-A", "x64",
                               "-DPS2X_ENABLE_RUNTIME_LOGS=OFF", "-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF", "-DPS2X_ENABLE_IOP_RPC_TRACE=OFF", "-DPS2X_SHOW_WINDOWS_CONSOLE=OFF",
                               "-DPS2X_BUILD_SIF_TESTS=OFF", "-DPS2X_IOP_BUILD_TESTS=OFF", "-DPS2X_ENABLE_FFMPEG=ON", "-DPS2X_GS_PARALLEL=ON", "-DPS2X_PARALLEL_GS_DIR=" + PGS_DIR.as_posix(),
                               "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"])
    if c.a.configure_only:
        print("  configured only (--configure-only)")
        return
    c.run("build-runner", [c.cmake, "--build", bd, "--config", "Release", "--target", "ps2EntryRunner", "--", "-m:%d" % c.jobs, "-p:CL_MPCount=%d" % c.jobs])
    if not (bd / "Release" / "ps2EntryRunner.exe").exists():
        sys.exit("ps2EntryRunner.exe was not produced")


def pe_imports(path):
    """DLL names imported by a PE file (standard library only)."""
    import struct
    d = Path(path).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rva, _ = struct.unpack_from("<II", d, dd + 8)
    if not rva:
        return []
    sec0 = opt + struct.unpack_from("<H", d, pe + 20)[0]
    table = []
    for i in range(nsec):
        vs, va, rs, ro = struct.unpack_from("<IIII", d, sec0 + 40 * i + 8)
        table.append((va, max(vs, rs), ro))

    def off(r):
        for va, size, ro in table:
            if va <= r < va + size:
                return r - va + ro
        return None

    out, o = [], off(rva)
    while o is not None:
        name_rva = struct.unpack_from("<I", d, o + 12)[0]
        if not name_rva:
            break
        n = off(name_rva)
        out.append(d[n:d.index(b"\0", n)].decode("ascii", "replace"))
        o += 20
    return out


def needed_dlls(folder, exe):
    """The DLLs of `folder` that `exe` needs, directly or through other DLLs."""
    have = {p.name.lower(): p for p in Path(folder).glob("*.dll")}
    todo, seen = [exe], set()
    while todo:
        for dep in pe_imports(todo.pop()):
            k = dep.lower()
            if k in have and k not in seen:
                seen.add(k)
                todo.append(have[k])
    return sorted(have[k] for k in seen)


def find_crt():
    """The app-local Visual C++ runtime folder of the installed Visual Studio, if any."""
    vsw = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vsw.exists():
        return None
    root = subprocess.run([str(vsw), "-latest", "-products", "*", "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
    found = sorted(Path(root).glob("VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT")) if root else []
    return found[-1] if found else None


def stage_package(c):
    rel = c.work / "runtime-build" / "Release"
    if not (rel / "ps2EntryRunner.exe").exists():
        sys.exit("build first")
    if c.dist.exists() and any(c.dist.iterdir()):
        sys.exit("%s is not empty" % c.dist)
    c.dist.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(rel / "ps2EntryRunner.exe", c.dist / "ShiningWind.exe")
    for f in needed_dlls(rel, rel / "ps2EntryRunner.exe"):
        shutil.copyfile(f, c.dist / f.name)
    crt = find_crt()
    if crt:
        for n in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
            if (crt / n).exists():
                shutil.copyfile(crt / n, c.dist / n)
    else:
        print("  note: the Visual C++ runtime DLLs were not found; users need the Microsoft Visual C++ Redistributable (x64)")
    for name in ("LICENSE",):
        if (ROOT / name).exists():
            shutil.copyfile(ROOT / name, c.dist / name)
    (c.dist / "README.txt").write_text(
        "Shining Wind (SLPM-66671) native Windows port, built from your own disc.\n"
        "Start ShiningWind.exe: it asks for your game disc image (.iso) or extracted folder, keeps a copy of the game files in %APPDATA%\\ShiningWind and starts.\n"
        "Optional add-ons (English translation, HD textures) are separate downloads: put them in %APPDATA%\\ShiningWind\\mods and \\hd_packs.\n"
        "For the HD textures run with PS2X_GS_SCALE=4 PS2X_GS_OUTPUT=4 PS2X_GS_PROMOTE=1 PS2X_GS_HD_SSAA=1 PS2X_GS_GPU_DOWNSCALE=1 (see play-x4.cmd).\n", encoding="utf-8")
    (c.dist / "play-x4.cmd").write_text(
        "@echo off\r\nrem x4 internal resolution (needed by the HD texture packs)\r\nset PS2X_GS_SCALE=4\r\nset PS2X_GS_OUTPUT=4\r\nset PS2X_GS_PROMOTE=1\r\n"
        "set PS2X_GS_HD_SSAA=1\r\nset PS2X_GS_GPU_DOWNSCALE=1\r\nstart \"\" \"%~dp0ShiningWind.exe\" %*\r\n", encoding="ascii")
    print("  %s ready" % c.dist)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", help="your disc image (.iso)")
    ap.add_argument("--work", default=str(ROOT / "port-work"))
    ap.add_argument("--dist", default=str(ROOT / "ShiningWind-port"))
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--cmake")
    ap.add_argument("--min-free-gb", type=float, default=8, help="free disk space required by the check (default 8)")
    ap.add_argument("--configure-only", action="store_true", help="build stage: configure CMake and stop (checks the settings, downloads the sources of the dependencies)")
    ap.add_argument("--stage", default="all", choices=["all"] + STAGES)
    a = ap.parse_args(argv)
    if a.stage in ("all", "extract") and not a.iso:
        ap.error("--iso is required")
    c = Ctx(a)
    for s in STAGES if a.stage == "all" else [a.stage]:
        print("== %s" % s)
        globals()["stage_" + s](c)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
