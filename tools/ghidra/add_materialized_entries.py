#!/usr/bin/env python3
"""Find function entries that the Ghidra export missed because their address is only materialised
(lui + addiu/ori, possibly scheduled several instructions apart) and stored in a function-pointer table.

It reads the game ELF read-only (flat SLPM image: file offset 0x200 == address 0x100000) and the current
function list (CSV) and prints the candidate rows. With --apply the rows are appended to the CSV (a backup
copy `<csv>.bak-materialized` is written first and never overwritten).

Accepted candidate (all must hold):
  * the address is materialised by lui + addiu/ori (window of 16 instructions, register not clobbered),
  * it is word aligned, inside the decoded code area and not inside an existing function range,
  * decoding from it reaches `jr ra` (+ delay slot) within 256 words using only known MIPS/R5900 opcodes,
  * every in-range branch lands inside [start, end) (no branch out of the unit except calls).

Usage:
  python tools/ghidra/add_materialized_entries.py --elf D:/NO_LABEL/SLPM_666.71 --csv shining_wind_ghidra.csv [--apply]
"""
import argparse
import bisect
import os
import shutil
import struct
import sys

BASE = 0x100000
FILE_OFF = 0x200
# Primary opcodes that can appear in R5900 code (LQ/SQ, MMI, COP0/1/2 included); the rest never does.
VALID_PRIMARY = set(range(0x00, 0x40)) - {0x13, 0x1D, 0x32, 0x34, 0x35, 0x3A, 0x3B, 0x3C, 0x3D}
BRANCH_I = {0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}
REGIMM = 0x01


def load(elf):
    data = open(elf, 'rb').read()
    n = (len(data) - FILE_OFF) // 4
    return struct.unpack_from('<%dI' % n, data, FILE_OFF), n


def read_csv(path):
    rows = []
    with open(path, 'r') as f:
        header = f.readline()
        for line in f:
            p = line.rstrip('\n').split(',')
            if len(p) >= 4:
                rows.append((p[0], int(p[1], 16), int(p[2], 16), int(p[3])))
    return header, rows


def writes_reg(x):
    op = x >> 26
    if op == 0:
        return (x >> 11) & 31
    if op in (0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x37, 0x1a, 0x1b):
        return (x >> 16) & 31
    if op == 3:
        return 31
    return -1


def materialized_targets(w, n, window, text_end):
    found = {}
    for i in range(n):
        x = w[i]
        if (x >> 26) != 0x0F:
            continue
        rt = (x >> 16) & 31
        if rt == 0:
            continue
        hi = x & 0xFFFF
        for j in range(i + 1, min(i + 1 + window, n)):
            y = w[j]
            op = y >> 26
            if op in (0x09, 0x0D) and ((y >> 21) & 31) == rt:
                lo = y & 0xFFFF
                if op == 0x09:
                    t = ((hi << 16) + (lo - 0x10000 if lo >= 0x8000 else lo)) & 0xFFFFFFFF
                else:
                    t = (hi << 16) | lo
                if BASE <= t < text_end and t % 4 == 0:
                    found.setdefault(t, []).append((BASE + 4 * i, BASE + 4 * j))
                break
            if writes_reg(y) == rt:
                break
            if op == 2 or (op == 0 and (y & 0x3F) == 8):
                break
    return found


def decode_leaf(w, n, start, max_words=2048):
    """Follow the control flow from `start` and return the end address (exclusive) of the unit, or None.

    Paths end at `jr ra` (+ delay slot). Conditional branches add both successors, calls (jal/jalr) continue
    after their delay slot, a plain `j` into the unit is followed and a `j` leaving it ends the path (tail
    call). Indirect jumps other than `jr ra`, undecodable opcodes and runaway code reject the candidate."""
    i0 = (start - BASE) // 4
    seen = set()
    work = [i0]
    top = i0
    first = True
    while work:
        i = work.pop()
        while True:
            if i in seen:
                break
            if i < 0 or i >= n or i - i0 > max_words:
                return None
            seen.add(i)
            x = w[i]
            op = x >> 26
            if op not in VALID_PRIMARY:
                return None
            if x == 0 and i == i0:
                return None
            top = max(top, i)
            pc = BASE + 4 * i
            if op in BRANCH_I or op == REGIMM or (op in (0x11, 0x12) and ((x >> 21) & 31) == 8):
                off = x & 0xFFFF
                if off & 0x8000:
                    off -= 0x10000
                tgt = (pc + 4 + off * 4 - BASE) // 4
                if tgt < i0:
                    return None
                seen.add(i + 1)
                top = max(top, i + 1)
                work.append(tgt)
                i += 2
                continue
            if op == 0 and (x & 0x3F) == 8:  # jr
                # jr $ra returns; jr $t9 is the indirect tail call of a virtual-method thunk (switch tables use other registers)
                if ((x >> 21) & 31) not in (31, 25):
                    return None
                seen.add(i + 1)
                top = max(top, i + 1)
                break
            if op == 0 and (x & 0x3F) == 9:  # jalr
                seen.add(i + 1)
                top = max(top, i + 1)
                i += 2
                continue
            if op == 3:  # jal
                seen.add(i + 1)
                top = max(top, i + 1)
                i += 2
                continue
            if op == 2:  # j
                tgt_addr = ((pc + 4) & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
                seen.add(i + 1)
                top = max(top, i + 1)
                if start <= tgt_addr < start + 4 * max_words:
                    work.append((tgt_addr - BASE) // 4)
                break
            i += 1
    return BASE + 4 * (top + 1)


def follows_return(w, n, target):
    """True if the code just before `target` is `jr $ra` + delay slot (+ nop padding): `target` starts a new function even when
    the exporter merged it into the previous unit."""
    i = (target - BASE) // 4 - 1
    pad = 0
    while i >= 0 and w[i] == 0 and pad < 8:
        i -= 1
        pad += 1
    # w[i] is the delay slot, w[i-1] must be an unconditional transfer: jr <reg> (return or jump-table/thunk) or j <target>
    # (tail call). Code behind either cannot fall into `target`.
    if i < 1:
        return False
    x = w[i - 1]
    return x == 0x03E00008 or (x >> 26 == 0 and (x & 0x3F) == 8 and ((x >> 11) & 0x3FF) == 0) or (x >> 26) == 2


def head_branches_into(w, n, start, cut, unit_end):
    """True if code in [start, cut) branches/jumps into [cut, unit_end): splitting there would cut a shared tail off its users."""
    for i in range((start - BASE) // 4, (cut - BASE) // 4):
        x = w[i]
        op = x >> 26
        pc = BASE + 4 * i
        tgt = None
        if op in BRANCH_I or op == REGIMM or (op in (0x11, 0x12) and ((x >> 21) & 31) == 8):
            off = x & 0xFFFF
            if off & 0x8000:
                off -= 0x10000
            tgt = pc + 4 + off * 4
        elif op == 2:
            tgt = ((pc + 4) & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
        if tgt is not None and cut <= tgt < unit_end:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--csv', required=True)
    ap.add_argument('--window', type=int, default=16)
    ap.add_argument('--data-end', type=lambda s: int(s, 16), default=None,
                    help='end of the scanned data area (default: end of the file image; was a fixed 0x3FED7C)')
    # The game's code spans 0x100000..0x379EC8 (end of the last exported function before the data area); the previous default
    # 0x2F0000 hid every vtable method / callback in 0x2F0000..0x37A000 (e.g. 0x329F70, a missing-target in the New Game flow).
    ap.add_argument('--text-end', type=lambda s: int(s, 16), default=0x37A000)
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()

    w, n = load(a.elf)
    header, rows = read_csv(a.csv)
    rows.sort(key=lambda r: r[1])
    starts = [r[1] for r in rows]
    found = materialized_targets(w, n, a.window, a.text_end)

    def containing(t):
        i = bisect.bisect_right(starts, t) - 1
        if i >= 0 and rows[i][1] <= t < rows[i][2]:
            return rows[i]
        return None

    # Raw code pointers stored in the data area (function-pointer tables, vtables, callback descriptors).
    data_lo = a.text_end
    data_hi = min(BASE + 4 * n, a.data_end if a.data_end is not None else BASE + 4 * n)
    for i in range((data_lo - BASE) // 4, (data_hi - BASE) // 4):
        v = w[i]
        if BASE <= v < a.text_end and v % 4 == 0:
            found.setdefault(v, []).append((BASE + 4 * i, BASE + 4 * i))

    new_rows = []
    splits = []
    for t in sorted(found):
        row = containing(t)
        if row is not None:
            if t == row[1] or not follows_return(w, n, t):
                continue
            end = decode_leaf(w, n, t)
            if end is None or end > row[2]:
                print('reject %08x (inside %s, tail does not decode as a unit)' % (t, row[0]))
                continue
            if head_branches_into(w, n, row[1], t, row[2]):
                print('reject %08x (code before it in %s branches into it)' % (t, row[0]))
                continue
            splits.append((row, t, end))
            print('split  %08x-%08x out of %s %08x-%08x  (preceded by jr/j + padding; referenced at %s)'
                  % (t, end, row[0], row[1], row[2], ' '.join('%x/%x' % q for q in found[t][:2])))
            continue
        end = decode_leaf(w, n, t)
        if end is None:
            print('reject %08x (not a self-contained leaf)' % t)
            continue
        # must not overlap the next existing function
        j = bisect.bisect_right(starts, t)
        if j < len(rows) and rows[j][1] < end:
            print('reject %08x (overlaps %s)' % (t, rows[j][0]))
            continue
        if new_rows and t < new_rows[-1][2]:
            print('skip   %08x (inside %s)' % (t, new_rows[-1][0]))
            continue
        new_rows.append(('FUN_%08x' % t, t, end, end - t))
        print('accept %08x-%08x size=%d  materialised at %s' % (t, end, end - t, ' '.join('%x/%x' % p for p in found[t][:2])))

    if a.apply and splits:
        bak = a.csv + '.bak-split'
        if not os.path.exists(bak):
            shutil.copyfile(a.csv, bak)
        lines = open(a.csv, 'r').read().split('\n')
        out = []
        by_name = {}
        for row, tstart, tend in splits:
            by_name.setdefault((row[0], row[1], row[2]), []).append((tstart, tend))
        for line in lines:
            p_ = line.split(',')
            if len(p_) >= 4:
                try:
                    key = (p_[0], int(p_[1], 16), int(p_[2], 16))
                except ValueError:
                    key = None
                if key in by_name:
                    cuts = sorted(by_name[key])
                    start = key[1]
                    # keep the head [start, first cut) under the old name, then one row per cut (to the next cut or the unit end)
                    bounds = [c[0] for c in cuts] + [key[2]]
                    out.append('%s,0x%08X,0x%08X,%d' % (key[0], start, bounds[0], bounds[0] - start))
                    for i_, (cs, ce) in enumerate(cuts):
                        e_ = min(ce, bounds[i_ + 1])
                        out.append('FUN_%08x,0x%08X,0x%08X,%d' % (cs, cs, bounds[i_ + 1], bounds[i_ + 1] - cs))
                    continue
            out.append(line)
        open(a.csv, 'w', newline='').write('\n'.join(out))
        print('split %d unit(s) in %s (backup %s)' % (len(splits), a.csv, bak))

    if a.apply and new_rows:
        bak = a.csv + '.bak-materialized'
        if not os.path.exists(bak):
            shutil.copyfile(a.csv, bak)
        with open(a.csv, 'a', newline='') as f:
            for name, s, e, size in new_rows:
                f.write('%s,0x%08X,0x%08X,%d\n' % (name, s, e, size))
        print('appended %d rows to %s (backup %s)' % (len(new_rows), a.csv, bak))
    elif a.apply:
        print('nothing to append')


if __name__ == '__main__':
    sys.exit(main())
