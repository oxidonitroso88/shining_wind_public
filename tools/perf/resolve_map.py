#!/usr/bin/env python3
"""Resolve module-relative offsets (as written by PS2X_GS_VKLOG in vk_diag.log) to functions using the MSVC linker .map file.

  python resolve_map.py ps2EntryRunner.map 77e6ece 77cced0 0x77e1087

The map must come from the SAME build as the exe that crashed. Offsets are RVAs; the map lists "Rva+Base" addresses, so the preferred load
base (printed in the map as "Preferred load address is ...") is subtracted. Output: the nearest preceding symbol, the distance into it and
the object/library that defines it. Names are undecorated when `undname` is available (Visual Studio developer prompt), else mangled.
"""
import bisect
import re
import subprocess
import sys


def load(path):
    base = None
    syms = []
    pat = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s+(?:f\s+)?(?:i\s+)?(\S+)?\s*$")
    with open(path, "r", encoding="latin-1", errors="replace") as f:
        for line in f:
            if base is None:
                m = re.search(r"Preferred load address is ([0-9A-Fa-f]+)", line)
                if m:
                    base = int(m.group(1), 16)
                    continue
            m = pat.match(line)
            if m and base is not None:
                addr = int(m.group(4), 16)
                if addr >= base:
                    syms.append((addr - base, m.group(3), m.group(5) or ""))
    if base is None:
        sys.exit("no 'Preferred load address' line: is this an MSVC link map?")
    syms.sort()
    return syms


def undname(name):
    try:
        out = subprocess.run(["undname", name], capture_output=True, text=True, timeout=10).stdout
        for l in out.splitlines():
            if l.startswith('is :-'):
                return l.split('"')[1]
    except Exception:
        pass
    return name


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    syms = load(sys.argv[1])
    keys = [s[0] for s in syms]
    for arg in sys.argv[2:]:
        off = int(arg, 16)
        i = bisect.bisect_right(keys, off) - 1
        if i < 0:
            print("0x%x: before the first symbol" % off)
            continue
        rva, name, obj = syms[i]
        print("0x%x: %s +0x%x   [%s]" % (off, undname(name), off - rva, obj))


if __name__ == "__main__":
    main()
