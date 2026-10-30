"""Static analysis + recompiler inputs for the MWo3 code overlays.

    python -I ovl_tool.py analyze  [--only ovl09]                 # discovery report (stdout)
    python -I ovl_tool.py build    --out <dir> [--only ovl09]     # <dir>/<ovl>/{<ovl>.elf,<ovl>.csv,<ovl>.toml,analysis.json}

Nothing is written next to the game data (D:\\NO_LABEL is read only). The outputs are derived from the game and must stay
out of git (they live under out/).

Function discovery (no Ghidra needed, validated by the checks at the end of `discover`):
  entries = overlay-internal `jal` targets
          + `jal` targets from the main ELF code and pointers in the main ELF data that land in this overlay's code
          + aligned words in the overlay data that point into its code (callback tables, vtables)
          + `lui/addiu|ori` address constants (overlay code and main code) that land in its code
          + every `addiu sp,sp,-N` that follows a function boundary (jr ra / j / b + delay slot) or the start of the code
  a candidate that is not a plausible function start (not a prologue and not right after a boundary) is only kept when the
  overlay itself calls it.
The end of each function is the next entry (minus trailing padding).
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ovl_common import (lui_constants, CSV_DEFAULT, HEADER_SIZE, JR_RA, MAIN_ELF_DEFAULT, OVERLAY_DIR_DEFAULT, OVERLAY_REGION, MainElf,
                        branch_target, is_jalr, is_jr, jump_target, load_overlays, sext16)

PROLOGUE_MASK = 0xFFFF8000
PROLOGUE = 0x27BD8000  # addiu sp,sp,-N


def is_prologue(w):
    return w is not None and (w & PROLOGUE_MASK) == PROLOGUE


def is_unconditional_transfer(w):
    op = w >> 26
    if op == 2:  # j
        return True
    if w == JR_RA:
        return True
    if op == 4 and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 0:  # beq zero,zero (b)
        return True
    return False


def main_references(main):
    """Overlay-region addresses referenced from the main ELF: direct jal/j, data pointers, lui/addiu constants."""
    direct, ptrs, consts = {}, {}, {}
    lo, hi = OVERLAY_REGION
    for a in range(main.base, main.end, 4):
        w = main.word_at(a)
        if main.is_code(a):
            jt = jump_target(a, w)
            if jt and lo <= jt[0] < hi:
                direct.setdefault(jt[0], []).append(a)
        elif lo <= w < hi and (w & 3) == 0:
            ptrs.setdefault(w, []).append(a)
    for at, v in lui_constants(main.word_at, main.base, main.end):
        if lo <= v < hi and (v & 3) == 0 and main.is_code(at):
            consts.setdefault(v, []).append(at)
    return direct, ptrs, consts


def discover(ov, mainrefs):
    direct, ptrs, consts = mainrefs
    cb, ce = ov.code_begin, ov.code_end
    words = {a: ov.word_at(a) for a in range(cb, ce, 4)}

    def w(a):
        return words.get(a)

    def boundary(a):
        if a == cb:
            return True
        prev2 = w(a - 8)
        prev1 = w(a - 4)
        if prev2 is not None and is_unconditional_transfer(prev2):
            return True
        # padding between functions
        return prev1 == 0 and (prev2 == 0 or is_unconditional_transfer(w(a - 12) or 0))

    reasons = {}

    def add(a, why, strong):
        if not (cb <= a < ce) or (a & 3):
            return
        if strong or boundary(a) or is_prologue(w(a)):
            reasons.setdefault(a, set()).add(why)

    if w(cb):  # the first word is padding in most overlays
        add(cb, "start", True)
    # internal calls
    calls = {}
    for a in range(cb, ce, 4):
        x = w(a)
        jt = jump_target(a, x)
        if jt and cb <= jt[0] < ce:
            if jt[1]:
                calls.setdefault(jt[0], []).append(a)
                add(jt[0], "jal-internal", True)
    for at, v in lui_constants(w, cb, ce):
        if cb <= v < ce and (v & 3) == 0:
            add(v, "const-internal", False)
    for t in direct:
        add(t, "main-jal", False)
    for t in ptrs:
        add(t, "main-ptr", False)
    for t in consts:
        add(t, "main-const", False)
    # pointers in the overlay data section
    for a in range(ov.data_begin, ov.data_end, 4):
        v = ov.word_at(a)
        if v is not None and cb <= v < ce and (v & 3) == 0:
            add(v, "data-ptr", False)
    # prologues after boundaries
    for a in range(cb, ce, 4):
        if is_prologue(w(a)) and boundary(a):
            add(a, "prologue", True)

    # Every instruction that follows `jr ra` + delay slot (past zero padding) starts a function unless a branch proves otherwise
    # (the merge pass below). Leaf functions without a prologue that are only reached through pointers built at run time
    # (callbacks) are found this way.
    for a in range(cb, ce, 4):
        if w(a) == JR_RA:
            n = a + 8
            while n < ce and w(n) == 0:
                n += 4
            if n < ce:
                add(n, "after-return", True)

    starts = sorted(reasons)

    def trim_end(start, nxt):
        e = nxt
        while e > start and w(e - 4) == 0:
            e -= 4
        if e > start:
            last = w(e - 4)
            # a branch/jump needs its delay slot
            if (jump_target(e - 4, last) or branch_target(e - 4, last) is not None or is_jr(last) or is_jalr(last)) and e < nxt:
                e += 4
        return max(e, start + 4)

    def build_funcs(sts):
        fs = []
        for i, s in enumerate(sts):
            nxt = sts[i + 1] if i + 1 < len(sts) else ce
            fs.append([s, trim_end(s, nxt)])
        return fs

    # merge functions whose conditional branches (or fall-through targets) leave their range
    anomalies = []
    for _ in range(50):
        funcs = build_funcs(starts)
        bad = None
        for s, e in funcs:
            for a in range(s, e, 4):
                t = branch_target(a, w(a))
                if t is not None and not (s <= t < e):
                    if cb <= t < ce:
                        bad = (s, e, a, t)
                        break
            if bad:
                break
        if not bad:
            break
        s, e, a, t = bad
        anomalies.append("branch %x->%x leaves function %x-%x: merged" % (a, t, s, e))
        if t < s:
            starts = [x for x in starts if not (t < x <= s)]
        else:
            starts = [x for x in starts if not (e <= x <= t)]
    funcs = build_funcs(starts)
    return funcs, reasons, anomalies, calls


def data_islands(ov):
    """Functions the linker left after the data of the overlay (static constructors: OVL10 0x454B80, OVL06 0x45C7E0...). The header's
    code size does not cover them, but a pointer in the data (the constructor list walked at start-up) does: aligned words in the data
    that point into the data and land on valid code ending with `jr ra` + delay slot."""
    out = {}
    for a in range(ov.data_begin, ov.data_end, 4):
        v = ov.word_at(a)
        if v is None or not (ov.data_begin <= v < ov.data_end) or (v & 3):
            continue
        x = v
        while x < ov.data_end and x < v + 0x400:
            wd = ov.word_at(x)
            if (wd >> 26) not in VALID_OPS or ((wd >> 26) == 0 and (wd & 0x3F) in BAD_SPECIAL):
                break
            if wd == JR_RA:
                if x - v >= 8:
                    out[v] = x + 8
                break
            x += 4
    return sorted(out.items())


def find_jump_tables(ov, funcs):
    """Report `jr $r` sites; the recompiler resolves the table itself from the section data."""
    out = []
    for s, e in funcs:
        for a in range(s, e, 4):
            x = ov.word_at(a)
            if is_jr(x) and x != JR_RA:
                out.append({"jr": a, "function": s, "reg": (x >> 21) & 31})
    return out


def validate(ov, funcs, mainrefs, calls):
    errs = []
    starts = {s for s, _ in funcs}
    direct, ptrs, consts = mainrefs
    cb, ce = ov.code_begin, ov.code_end
    # every internal jal must land on an entry
    for t, srcs in calls.items():
        if t not in starts:
            errs.append("internal jal target %x is not a function start (from %x)" % (t, srcs[0]))
    # j/jr tail calls inside the code range must hit entries or stay in the function
    for s, e in funcs:
        for a in range(s, e, 4):
            jt = jump_target(a, ov.word_at(a))
            if jt and not jt[1] and cb <= jt[0] < ce and not (s <= jt[0] < e) and jt[0] not in starts:
                errs.append("j %x at %x leaves function %x-%x and is not an entry" % (jt[0], a, s, e))
    return errs


def write_elf(path, ov, islands=()):
    """Synthetic ELF32-LE MIPS: .text (AX) = code, .data (WA) = header+data are loaded together at base. `islands` (code after the data)
    become one more executable section (.text2) that splits the data (the recompiler registers it as the overlay's extra range)."""
    base = ov.base
    img = ov.raw
    code_off, code_len = HEADER_SIZE, ov.code_size
    names = b"\0.text\0.data\0.hdr\0.shstrtab\0"
    n_text, n_data, n_hdr, n_str = 1, 7, 13, 18
    ph_off = 0x34
    ph_size = 32
    data_off = 0x100
    blob = img  # whole file at data_off, so offsets are file_offset + data_off
    isl = None
    if islands:
        isl = (min(s for s, _ in islands), max(e for _, e in islands))
        names += b".text2\0.data2\0"
    n_text2, n_data2 = len(names) - 14, len(names) - 7
    shstr_off = data_off + len(blob)
    sh_off = (shstr_off + len(names) + 3) & ~3
    nsec = 5 + (2 if isl else 0)
    elf = bytearray(sh_off + nsec * 40)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    # e_type=2 (EXEC), e_machine=8 (MIPS), version 1, entry 0 (no bootstrap), phoff, shoff, flags, ehsize, phentsize, phnum, shentsize, shnum, shstrndx
    struct.pack_into("<16sHHIIIIIHHHHHH", elf, 0, ident, 2, 8, 1, 0, ph_off, sh_off, 0x20924001, 52, ph_size, 1, 40, nsec, nsec - 1)
    struct.pack_into("<8I", elf, ph_off, 1, data_off, base, base, len(blob), len(blob), 7, 0x10)
    elf[data_off:data_off + len(blob)] = blob
    elf[shstr_off:shstr_off + len(names)] = names

    def sh(i, name, typ, flags, addr, off, size):
        struct.pack_into("<10I", elf, sh_off + i * 40, name, typ, flags, addr, off, size, 0, 0, 4, 0)

    sh(0, 0, 0, 0, 0, 0, 0)
    sh(1, n_hdr, 1, 2, base, data_off, HEADER_SIZE)  # .hdr (A)
    sh(2, n_text, 1, 6, base + code_off, data_off + code_off, code_len)  # .text (AX)
    d_begin = base + code_off + code_len
    if isl:
        sh(3, n_data, 1, 3, d_begin, data_off + code_off + code_len, isl[0] - d_begin)  # .data (WA) up to the island
        sh(4, n_text2, 1, 6, isl[0], data_off + (isl[0] - base), isl[1] - isl[0])  # .text2 (AX): the island
        tail = (base + ov.size) - isl[1]
        if tail:
            sh(5, n_data2, 1, 3, isl[1], data_off + (isl[1] - base), tail)  # .data2 (WA)
        else:
            nsec_last = 5
        sh(nsec - 1, n_str, 3, 0, 0, shstr_off, len(names))
    else:
        sh(3, n_data, 1, 3, d_begin, data_off + code_off + code_len, ov.data_size)  # .data (WA)
        sh(4, n_str, 3, 0, 0, shstr_off, len(names))
    open(path, "wb").write(bytes(elf))


def write_inputs(outdir, ov, funcs, main_path, islands=()):
    d = os.path.join(outdir, ov.name)
    os.makedirs(d, exist_ok=True)
    elf = os.path.join(d, ov.name + ".elf")
    write_elf(elf, ov, islands)
    funcs = list(funcs) + [list(i) for i in islands]
    csv = os.path.join(d, ov.name + ".csv")
    with open(csv, "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for s, e in funcs:
            f.write("%s_FUN_%08x,0x%08X,0x%08X,%d\n" % (ov.name, s, s, e, e - s)) if False else f.write(
                "%s_FUN_%08x,0x%08X,0x%08X,%d\n" % (ov.name, s, s, e, e - s))
    toml = os.path.join(d, ov.name + ".toml")
    gen = os.path.join(d, "generated").replace("\\", "/")
    with open(toml, "w", newline="\n") as f:
        f.write("# Generated by tools/overlay/ovl_tool.py (derived from game data: keep out of git)\n[general]\n")
        f.write('input = "%s"\n' % elf.replace("\\", "/"))
        f.write('output = "%s"\n' % gen)
        f.write('ghidra_output = "%s"\n' % csv.replace("\\", "/"))
        f.write('overlay_name = "%s"\n' % ov.name)
        f.write("single_file_output = false\npatch_syscalls = false\npatch_cop0 = true\npatch_cache = true\nstubs = []\nskip = []\n")
    return d


def supplement_candidates(main, overlays):
    """Main-program addresses the overlays call (jal) or take the address of, that the function map does not list as a function
    start: the main program has no code that reaches them, so they were never recompiled."""
    starts = {s for s, _ in main.funcs}
    lo, hi = OVERLAY_REGION
    found = {}
    for o in overlays:
        for a in range(o.code_begin, o.code_end, 4):
            x = o.word_at(a)
            jt = jump_target(a, x)
            if jt and jt[1] and main.base <= jt[0] < main.end and not (lo <= jt[0] < hi):
                found.setdefault(jt[0], set()).add(o.name + ":jal")
    return {t: why for t, why in found.items() if t not in starts and (t & 3) == 0}


def main_function_end(main, start, other_starts, limit=0x1000):
    """End of a function that starts at `start`: the first `jr ra` (plus its delay slot) at or after the farthest forward branch
    target seen so far; never beyond the next known function start."""
    maxfwd = start
    a = start
    while a < start + limit:
        if a != start and a in other_starts:
            return a
        w = main.word_at(a)
        if w is None:
            break
        t = branch_target(a, w)
        if t is not None and t > maxfwd and t < start + limit:
            maxfwd = t
        jt = jump_target(a, w)
        if jt and not jt[1] and jt[0] > maxfwd and start <= jt[0] < start + limit:
            maxfwd = jt[0]
        if w == JR_RA and a >= maxfwd:
            return a + 8
        a += 4
    return None


VALID_OPS = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
             0x1A, 0x1B, 0x1C, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D,
             0x2E, 0x2F, 0x31, 0x33, 0x35, 0x36, 0x37, 0x39, 0x3D, 0x3E, 0x3F}


BAD_SPECIAL = {0x01, 0x05, 0x0E, 0x15, 0x1C, 0x1D, 0x1E, 0x1F, 0x28, 0x29}  # undefined on the R5900 (data that looks like code)


def gap_function_end(main, start, gap_end):
    """First `jr ra` (+ delay slot) at/after the farthest forward branch that stays inside the gap; the end of the gap if the
    function continues into the next mapped piece."""
    maxfwd = start
    a = start
    while a < gap_end:
        w = main.word_at(a)
        t = branch_target(a, w)
        if t is not None and maxfwd < t < gap_end:
            maxfwd = t
        if w == JR_RA and a >= maxfwd:
            return min(a + 8, gap_end)
        a += 4
    return gap_end


def orphan_functions(main, code_end=0x3FED74):
    """Code the function map (Ghidra CSV) left out: functions inside the gaps between mapped functions that look like code
    (all words decode, branches stay inside, they end with `jr ra` + delay slot) and start at a boundary. They are only ever
    reached through run-time pointers, which is why the main program never needed them... until an overlay or a rare path does."""
    fs = sorted(main.funcs)
    gaps, cur = [], main.base
    for st, en in fs:
        if st > cur:
            gaps.append((cur, st))
        cur = max(cur, en)
    out = []
    for g0, g1 in gaps:
        if g0 >= code_end:
            break
        g1 = min(g1, code_end)
        a = g0
        while a < g1:
            while a < g1 and main.word_at(a) == 0:
                a += 4
            if a >= g1:
                break
            e = None
            w0 = main.word_at(a)
            if (w0 >> 26) in VALID_OPS:
                e = main_function_end(main, a, set(), limit=0x4000)
            ok = e is not None
            if ok:
                for x in range(a, e, 4):
                    w = main.word_at(x)
                    if (w >> 26) not in VALID_OPS or ((w >> 26) == 0 and (w & 0x3F) in BAD_SPECIAL):
                        ok = False
                        break
                    t = branch_target(x, w)
                    # the function map splits big functions into pieces: follow the whole function (its branches must stay inside it,
                    # otherwise the exit target would need an entry of its own)
                    if t is not None and not (a <= t < e):
                        ok = False
                        break
            if ok and e - a >= 8:
                out.append((a, e))
                a = e
            else:
                a += 4
                while a < g1 and main.word_at(a) != 0 and main.word_at(a - 8) != JR_RA:
                    a += 4
    return out


def build_supplement(outdir, main, cands, main_path, name="mainx"):
    all_starts = {s for s, _ in main.funcs} | set(cands)
    funcs, bad = [], []
    for t in sorted(cands):
        e = main_function_end(main, t, set(), limit=0x4000)  # long functions (printf format parser: 0x1997a0-0x19ac9c)
        if e is None:
            bad.append(t)
            continue
        funcs.append((t, e))
    d = os.path.join(outdir, name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".csv"), "w", newline="\n") as f:
        f.write("Name,Start,End,Size\n")
        for s, e in funcs:
            f.write("%s_FUN_%08x,0x%08X,0x%08X,%d\n" % (name, s, s, e, e - s))
    gen = os.path.join(d, "generated").replace("\\", "/")
    with open(os.path.join(d, name + ".toml"), "w", newline="\n") as f:
        f.write("# Generated by tools/overlay/ovl_tool.py supplement (derived from game data: keep out of git)\n[general]\n")
        f.write('input = "%s"\n' % main_path.replace("\\", "/"))
        f.write('output = "%s"\n' % gen)
        f.write('ghidra_output = "%s"\n' % os.path.join(d, name + ".csv").replace("\\", "/"))
        f.write('supplement_name = "%s"\n' % name)
        f.write("single_file_output = false\npatch_syscalls = false\npatch_cop0 = true\npatch_cache = true\nstubs = []\nskip = []\n")
    json.dump({"functions": funcs, "reasons": {hex(t): sorted(v) for t, v in cands.items()}, "no_end_found": bad},
              open(os.path.join(d, "analysis.json"), "w"), indent=1)
    print("%s: %d supplementary main functions (%d without a determinable end)" % (name, len(funcs), len(bad)))
    for s, e in funcs:
        print("   %x-%x" % (s, e), end="")
    print()


def cross_targets(a, main):
    """Entry points the main program can reach at run time but the generated function table has no slot for (`maincross`).

    The recompiler only registers function starts and its own resume points, and the function map splits functions and omits
    jump-table case bodies, so two kinds of target end as guest-branch:missing-target (the thread dies):
      * direct branches that leave their function (`ctx->pc = X; return;`) into the inside of another mapped function
        (0x25ADF0 -> 0x25B1AC, found in the world-map menu);
      * targets of jump tables whose `jr` the recompiler could not resolve (0x25B1D0 -> 0x25AE20 ...): aligned code pointers in runs of
        three or more words in the data.
    Each target becomes a supplementary function (compiled completely, like mainx/mainorph), written to the table only where the slot
    is empty. Targets of the supplementary code itself are added until nothing is missing (fixed point)."""
    import glob
    import re
    import subprocess
    here = os.path.dirname(os.path.abspath(__file__))
    runner = a.runner or os.path.join(here, "..", "..", "ps2xRuntime", "src", "runner")
    overlays = a.install_dir or os.path.join(here, "..", "..", "ps2xRuntime", "src", "runner_overlays")
    reg_re = re.compile(r"\]\s*=\s*\w+;\s*//\s*0x([0-9a-fA-F]+)")
    registered = {int(m, 16) for m in reg_re.findall(open(os.path.join(runner, "register_functions.cpp"), encoding="utf-8", errors="replace").read())}
    for fn in ("register_mainx.cpp", "register_mainorph.cpp"):
        f = os.path.join(overlays, fn)
        if os.path.exists(f):
            registered |= {int(m, 16) for m in re.findall(r"\{0x([0-9a-f]+)u,", open(f).read())}
    mapped = sorted(main.funcs)

    def known_code(t):
        return any(s0 <= t < e0 for s0, e0 in mapped) or main.is_code(t) and t < 0x3D0000 and any(s0 <= t < e0 for s0, e0 in supp_ranges)

    supp_ranges = []
    for fn in glob.glob(os.path.join(overlays, "mainx_*.cpp")) + glob.glob(os.path.join(overlays, "mainorph_*.cpp")):
        m = re.search(r"_0x([0-9a-f]+)\.cpp$", fn)
        h = re.search(r"// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)", open(fn, encoding="utf-8", errors="replace").read(4000))
        if h:
            supp_ranges.append((int(h.group(1), 16), int(h.group(2), 16)))

    def valid_code(start, end):
        for x in range(start, end, 4):
            w = main.word_at(x)
            if w is None or (w >> 26) not in VALID_OPS or ((w >> 26) == 0 and (w & 0x3F) in BAD_SPECIAL):
                return False
        return True

    # (1) jump-table targets
    jt = {}
    run = []
    for addr in range(0x3D0000, main.end, 4):
        w = main.word_at(addr)
        if w is not None and 0x100000 <= w < 0x3D0000 and (w & 3) == 0 and any(s0 <= w < e0 for s0, e0 in mapped):
            run.append((addr, w))
        else:
            if len(run) >= 3:
                for at, t in run:
                    jt.setdefault(t, set()).add("jump-table@%x" % at)
            run = []
    # (2) direct cross-function branches of the generated code (main + the supplements produced so far)
    found = {t: why for t, why in jt.items() if t not in registered}
    sources = [f for f in glob.glob(os.path.join(runner, "*.cpp")) if not os.path.basename(f).startswith(("caseD_", "register_"))]
    sources += glob.glob(os.path.join(overlays, "mainx_*.cpp")) + glob.glob(os.path.join(overlays, "mainorph_*.cpp"))
    pat = re.compile(r"ctx->pc = 0x([0-9A-Fa-f]+)u;\s*return;")
    hdr = re.compile(r"// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)")
    gen_dir = os.path.join(a.out, "maincross", "generated")
    cands = {}
    for it in range(8):
        for f in sources + glob.glob(os.path.join(gen_dir, "maincross_*.cpp")):
            text = open(f, encoding="utf-8", errors="replace").read()
            h = hdr.search(text)
            if not h:
                continue
            lo, hi = int(h.group(1), 16), int(h.group(2), 16)
            for m in pat.finditer(text):
                t = int(m.group(1), 16)
                if lo <= t < hi or t in registered or (t & 3) or not known_code(t):
                    continue
                found.setdefault(t, set()).add("branch:" + os.path.basename(f))
        sources = [f for f in sources if "maincross" not in f]
        pending = {}
        covered = set()
        for t in sorted(found):
            if t in cands or t in covered:
                continue
            e = main_function_end(main, t, set(), limit=0x2000)
            if e is None:
                for s0, e0 in mapped:
                    if s0 <= t < e0:
                        e = e0
            if e is None or not valid_code(t, e):
                print("  skipped %x (no usable function body)" % t)
                continue
            pending[t] = (found[t], e)
        if not pending:
            break
        for t, (why, e) in pending.items():
            cands[t] = (why, e)
        print("iteration %d: %d new functions (%d in total)" % (it, len(pending), len(cands)))
        build_supplement(a.out, main, {t: v[0] for t, v in cands.items()}, a.elf, "maincross")
        exe = a.recomp or os.path.join(here, "..", "..", "out", "build", "ps2xRecomp", "ps2_recomp.exe")
        subprocess.run([exe, os.path.join(a.out, "maincross", "maincross.toml")], check=True,
                       stdout=open(os.path.join(a.out, "maincross", "recomp.log"), "w"), stderr=subprocess.STDOUT)
        # what the generated supplement registers counts as covered; its own exits are scanned in the next iteration
        regtxt = open(os.path.join(gen_dir, "register_maincross.cpp")).read()
        registered |= {int(m, 16) for m in re.findall(r"\{0x([0-9a-f]+)u,", regtxt)}
        found = {t: w for t, w in found.items() if t not in registered}
    missing = sorted(t for t in found if t not in registered)
    print("maincross: %d functions; targets still without a slot: %s" % (len(cands), " ".join("%x" % t for t in missing)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["analyze", "build", "supplement", "crossfix"])
    ap.add_argument("--only")
    ap.add_argument("--out")
    ap.add_argument("--elf", default=MAIN_ELF_DEFAULT)
    ap.add_argument("--csv", default=CSV_DEFAULT)
    ap.add_argument("--overlays", default=OVERLAY_DIR_DEFAULT)
    ap.add_argument("--runner", help="crossfix: folder with the generated main program (default ps2xRuntime/src/runner)")
    ap.add_argument("--install-dir", help="crossfix: folder where ovl_install.py put the supplements (default ps2xRuntime/src/runner_overlays)")
    ap.add_argument("--recomp", help="crossfix: ps2_recomp executable (default out/build/ps2xRecomp/ps2_recomp.exe)")
    a = ap.parse_args()
    main_elf = MainElf(a.elf, a.csv)
    if a.cmd == "crossfix":
        if not a.out:
            ap.error("--out required")
        cross_targets(a, main_elf)
        return
    if a.cmd == "supplement":
        if not a.out:
            ap.error("--out required")
        ovs = load_overlays(a.overlays)
        build_supplement(a.out, main_elf, supplement_candidates(main_elf, ovs), a.elf, "mainx")
        orph = orphan_functions(main_elf)
        known = set(supplement_candidates(main_elf, ovs))
        build_supplement(a.out, main_elf, {t: {"orphan"} for t, e in orph if t not in known}, a.elf, "mainorph")
        return
    refs = main_references(main_elf)
    total = 0
    entry_sets = {}
    for ov in load_overlays(a.overlays):
        if a.only and ov.name != a.only:
            continue
        funcs, reasons, anomalies, calls = discover(ov, refs)
        jts = find_jump_tables(ov, funcs)
        errs = validate(ov, funcs, refs, calls)
        total += len(funcs)
        entry_sets[ov.name] = {s for s, _ in funcs}
        by = {}
        for s in reasons.values():
            for r in s:
                by[r] = by.get(r, 0) + 1
        print("%s idx=%d code=%x-%x data=%x-%x functions=%d jr_tables=%d anomalies=%d errors=%d reasons=%s" % (
            ov.name, ov.index, ov.code_begin, ov.code_end, ov.data_begin, ov.data_end, len(funcs), len(jts),
            len(anomalies), len(errs), by))
        for x in anomalies + errs:
            print("   !", x)
        if a.cmd == "build":
            if not a.out:
                ap.error("--out required")
            islands = data_islands(ov)
            if islands:
                print("   data islands (code after the data):", " ".join("%x-%x" % i for i in islands))
            d = write_inputs(a.out, ov, funcs, a.elf, islands)
            json.dump({"name": ov.name, "index": ov.index, "base": ov.base, "code": [ov.code_begin, ov.code_end],
                       "data": [ov.data_begin, ov.data_end], "functions": funcs, "jump_tables": jts,
                       "anomalies": anomalies, "errors": errs}, open(os.path.join(d, "analysis.json"), "w"), indent=1)
    print("total functions", total)
    if not a.only:
        allstarts = set().union(*entry_sets.values())
        direct, ptrs, consts = refs
        for label, d in (("main-jal", direct), ("main-ptr", ptrs), ("main-const", consts)):
            missing = sorted(t for t in d if t not in allstarts)
            print("%s: %d targets, %d not an entry in any overlay: %s" % (label, len(d), len(missing), " ".join("%x" % t for t in missing)))


if __name__ == "__main__":
    main()
