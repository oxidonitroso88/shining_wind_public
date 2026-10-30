"""List / extract files of a CRI CVM container (header + plain ISO9660) without modifying it.

    python -I cvm_tool.py list    <ST.CVM> [--filter TEXT]
    python -I cvm_tool.py extract <ST.CVM> <PATH inside> <output file>
    python -I cvm_tool.py check   <ST.CVM> <PATH inside> <mod file>      # what the game will see if <mod file> is dropped in mods/ST/<PATH>

Used to prepare the files of the resource replacement folder (docs/resource-replacement-2026-10-13.md). Read only.
"""
import struct
import sys

SECTOR = 2048


class Cvm:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.header = None
        for candidate in (0x1800, 0, 0x800, 0x1000, 0x2000):
            self.f.seek(candidate + 16 * SECTOR)
            d = self.f.read(SECTOR)
            if len(d) == SECTOR and d[0] == 1 and d[1:6] == b"CD001":
                self.header = candidate
                break
        if self.header is None:
            raise SystemExit("not a CVM/ISO9660 container: %s" % path)
        self.volume = struct.unpack_from("<I", d, 80)[0]
        self.root = (struct.unpack_from("<I", d, 158)[0], struct.unpack_from("<I", d, 166)[0])
        self.files = {}
        self._walk(self.root[0], self.root[1], "")

    def _read(self, lsn, size):
        self.f.seek(self.header + lsn * SECTOR)
        return self.f.read(size)

    def _walk(self, lsn, size, prefix):
        data = self._read(lsn, size)
        o = 0
        while o < len(data):
            n = data[o]
            if n == 0:
                o = (o // SECTOR + 1) * SECTOR
                continue
            nl = data[o + 32]
            name = data[o + 33:o + 33 + nl]
            elsn = struct.unpack_from("<I", data, o + 2)[0]
            esize = struct.unpack_from("<I", data, o + 10)[0]
            if not (nl == 1 and name in (b"\0", b"\1")):
                text = name.decode("ascii", "replace")
                if text.endswith(";1"):
                    text = text[:-2]
                path = (prefix + "/" + text) if prefix else text
                if data[o + 25] & 2:
                    self._walk(elsn, esize, path)
                else:
                    self.files[path.upper()] = (elsn, esize)
            o += n

    def extract(self, path):
        lsn, size = self.files[path.upper().replace("\\", "/")]
        return self._read(lsn, size)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, container = sys.argv[1], sys.argv[2]
    cvm = Cvm(container)
    if cmd == "list":
        flt = sys.argv[4].upper() if len(sys.argv) > 4 and sys.argv[3] == "--filter" else ""
        for path, (lsn, size) in sorted(cvm.files.items()):
            if flt in path:
                print("%-48s lsn %7d  %9d bytes" % (path, lsn, size))
        print("%d files, header 0x%x, volume %d sectors" % (len(cvm.files), cvm.header, cvm.volume), file=sys.stderr)
    elif cmd == "extract":
        open(sys.argv[4], "wb").write(cvm.extract(sys.argv[3]))
    elif cmd == "check":
        path = sys.argv[3].upper().replace("\\", "/")
        lsn, size = cvm.files[path]
        import os
        mod = os.path.getsize(sys.argv[4])
        need = (mod + SECTOR - 1) // SECTOR
        have = max((size + SECTOR - 1) // SECTOR, 1)
        print("%s: original %d bytes (%d sectors) -> mod %d bytes (%d sectors): %s" % (
            path, size, have, mod, need, "in place" if need <= have else "appended after the original data"))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
