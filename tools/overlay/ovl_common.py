"""Shared helpers for the MWo3 code-overlay tools (Shining Wind, SLPM_666.71 + OVERLAY/OVLxx.BIN).

Overlay file layout (verified on OVL00..OVL10, see docs/overlays-design-2026-10-12.md):
  +0x00 'MWo3' magic            +0x04 index (OVLnn -> nn+1)       +0x08 load base (0x452200)
  +0x0C code size               +0x10 data size                   +0x14 0
  +0x18 end address (= base + file size, twice)                   +0x20 original name ("ovl09.bin")
  +0x40 code (code size bytes)  then data (data size bytes). The whole file is loaded at `base` (header included);
  there are no relocations: the code is linked at the fixed base and calls the main ELF with absolute `jal`.
"""
import os
import struct

MAIN_ELF_DEFAULT = "D:/NO_LABEL/SLPM_666.71"
OVERLAY_DIR_DEFAULT = "D:/NO_LABEL/OVERLAY"
CSV_DEFAULT = "D:/PS2Recomp/shining_wind_ghidra.csv"
OVERLAY_REGION = (0x452200, 0x45C980)  # union of the 11 PT_LOAD segments of the ELF (same vaddr)
HEADER_SIZE = 0x40
MAGIC = 0x336F574D  # 'MWo3'


class Overlay:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0].lower()  # ovl09
        raw = open(path, "rb").read()
        self.raw = raw
        (self.magic, self.index, self.base, self.code_size, self.data_size, self.zero,
         self.end_a, self.end_b) = struct.unpack_from("<8I", raw, 0)
        self.size = len(raw)
        self.code_begin = self.base + HEADER_SIZE
        self.code_end = self.code_begin + self.code_size
        self.data_begin = self.code_end
        self.data_end = self.base + self.size
        self.words = struct.unpack_from("<%dI" % (self.size // 4), raw, 0)

    def check(self):
        errs = []
        if self.magic != MAGIC:
            errs.append("bad magic %08x" % self.magic)
        if self.base != OVERLAY_REGION[0]:
            errs.append("base %x" % self.base)
        if self.end_a != self.end_b or self.end_a != self.base + self.size:
            errs.append("end %x/%x != base+size %x" % (self.end_a, self.end_b, self.base + self.size))
        if HEADER_SIZE + self.code_size + self.data_size != self.size:
            errs.append("header+code+data %x != size %x" % (HEADER_SIZE + self.code_size + self.data_size, self.size))
        if self.base + self.size > OVERLAY_REGION[1]:
            errs.append("exceeds region")
        return errs

    def word_at(self, addr):
        o = addr - self.base
        if 0 <= o < self.size - 3 and (addr & 3) == 0:
            return self.words[o >> 2]
        return None

    def in_code(self, addr):
        return self.code_begin <= addr < self.code_end


def load_overlays(directory=OVERLAY_DIR_DEFAULT):
    out = []
    for fn in sorted(os.listdir(directory)):
        if fn.upper().startswith("OVL") and fn.upper().endswith(".BIN"):
            out.append(Overlay(os.path.join(directory, fn)))
    return out


class MainElf:
    """Flat image of the main ELF (one RWX section 0x100000..0x41e380) + code ranges from the Ghidra CSV."""

    def __init__(self, path=MAIN_ELF_DEFAULT, csv_path=CSV_DEFAULT):
        b = open(path, "rb").read()
        phoff, = struct.unpack_from("<I", b, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", b, 0x2A)
        self.segments = []
        for i in range(phnum):
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from("<8I", b, phoff + i * phentsize)
            if t == 1 and fsz:
                self.segments.append((va, b[off:off + fsz]))
        self.mem = {}
        for va, data in self.segments:
            self.mem[va] = struct.unpack_from("<%dI" % (len(data) // 4), data, 0)
        self.base, self.words = next(iter(self.mem.items()))
        self.end = self.base + len(self.words) * 4
        self.funcs = []  # (start, end)
        for line in open(csv_path, encoding="utf-8", errors="replace").read().splitlines()[1:]:
            p = line.split(",")
            if len(p) >= 4:
                try:
                    self.funcs.append((int(p[1], 0), int(p[2], 0)))
                except ValueError:
                    pass
        self.funcs.sort()
        self.code_mask = bytearray((self.end - self.base) // 4)
        for s, e in self.funcs:
            for a in range(max(s, self.base), min(e, self.end), 4):
                self.code_mask[(a - self.base) >> 2] = 1

    def is_code(self, addr):
        return self.base <= addr < self.end and self.code_mask[(addr - self.base) >> 2] == 1

    def word_at(self, addr):
        if self.base <= addr < self.end and (addr & 3) == 0:
            return self.words[(addr - self.base) >> 2]
        return None


# --- minimal MIPS control-flow decoding -----------------------------------------------------------------------------
def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def branch_target(pc, w):
    """Target of a conditional/unconditional PC-relative branch word, or None."""
    op = w >> 26
    rs = (w >> 21) & 31
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or op == 1 or (op in (0x10, 0x11, 0x12) and rs == 8):
        if op == 1 and ((w >> 16) & 31) in (0x10, 0x11, 0x12, 0x13):  # bltzal/bgezal(l): calls
            return pc + 4 + (sext16(w & 0xFFFF) << 2)
        if op == 1 and ((w >> 16) & 31) not in (0, 1, 2, 3):
            return None
        return pc + 4 + (sext16(w & 0xFFFF) << 2)
    return None


def jump_target(pc, w):
    op = w >> 26
    if op in (2, 3):
        return ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2), op == 3
    return None


def is_jr(w):
    return (w >> 26) == 0 and (w & 0x3F) == 8


def is_jalr(w):
    return (w >> 26) == 0 and (w & 0x3F) == 9


JR_RA = 0x03E00008


def lui_constants(getword, begin, end, window=40):
    """Yields (address of the lui, value) for `lui rt,hi` followed (within `window` instructions, schedulers interleave a lot)
    by `addiu/ori rt,rt,lo` on the same register, stopping if the register is loaded again or the code ends (jr/j)."""
    for a in range(begin, end, 4):
        x = getword(a)
        if x is None or (x >> 26) != 0xF:
            continue
        rt = (x >> 16) & 31
        if rt == 0:
            continue
        for j in range(1, window):
            y = getword(a + 4 * j)
            if y is None:
                break
            op = y >> 26
            if op in (9, 0xD) and ((y >> 21) & 31) == rt and ((y >> 16) & 31) == rt:
                lo = sext16(y & 0xFFFF) if op == 9 else (y & 0xFFFF)
                yield a, (((x & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                break
            if op == 0xF and ((y >> 16) & 31) == rt:
                break
            if (op == 0 and (y & 0x3F) in (8, 9) ) or op == 2:
                break
