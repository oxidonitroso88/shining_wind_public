"""Extracts files from a PS2 disc image (ISO9660, 2048 byte sectors or raw 2352 byte Mode 1/2 Form 1 sectors) with the standard library only.

    python iso_extract.py <image.iso> <out_dir> [--only NAME_OR_DIR ...]   # default: everything
    python iso_extract.py <image.iso> --list

Read only on the image; refuses to overwrite files in <out_dir>. Names are the ISO9660 names without the ';1' suffix; paths that would leave <out_dir> are rejected.
"""
import argparse
import os
import struct
import sys


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, 2)
        self.size = self.f.tell()
        for ss, data_off in ((2048, 0), (2352, 16), (2352, 24)):
            self.ss, self.off = ss, data_off
            try:
                pvd = self.sector(16)
            except Exception:
                continue
            if pvd[1:6] == b"CD001" and pvd[0] == 1:
                break
        else:
            sys.exit("not an ISO9660 image")
        self.root = self.record(pvd[156:190])

    def sector(self, lba, count=1):
        self.f.seek(lba * self.ss)
        raw = self.f.read(self.ss * count)
        if len(raw) < self.ss * count:
            raise EOFError
        if self.ss == 2048:
            return raw
        return b"".join(raw[i * self.ss + self.off:i * self.ss + self.off + 2048] for i in range(count))

    @staticmethod
    def record(b):
        return dict(lba=struct.unpack_from("<I", b, 2)[0], size=struct.unpack_from("<I", b, 10)[0], flags=b[25])

    def listdir(self, d):
        data = self.sector(d["lba"], (d["size"] + 2047) // 2048)[:d["size"]]
        i, out = 0, []
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            rec = data[i:i + n]
            i += n
            name = rec[33:33 + rec[32]]
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            e = self.record(rec)
            e["name"] = name
            out.append(e)
        return out

    def walk(self, d=None, prefix=""):
        for e in self.listdir(d or self.root):
            path = prefix + e["name"]
            if e["flags"] & 2:
                yield from self.walk(e, path + "/")
            else:
                yield path, e

    def read(self, e, dest):
        left = e["size"]
        lba = e["lba"]
        while left > 0:
            n = min(512, (left + 2047) // 2048)
            dest.write(self.sector(lba, n)[:left])
            left -= n * 2048
            lba += n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args(argv)
    img = Image(a.image)
    files = list(img.walk())
    if a.list or not a.out:
        for p, e in files:
            print("%12d  %s" % (e["size"], p))
        return 0
    only = [o.strip("/").upper() for o in a.only]
    n = 0
    for p, e in files:
        up = p.upper()
        if only and not any(up == o or up.startswith(o + "/") for o in only):
            continue
        dest = os.path.normpath(os.path.join(a.out, *p.split("/")))
        if not os.path.abspath(dest).startswith(os.path.abspath(a.out) + os.sep):
            sys.exit("unsafe name: " + p)
        if os.path.exists(dest):
            sys.exit("refusing to overwrite " + dest)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as out:
            img.read(e, out)
        n += 1
    print("extracted %d files to %s" % (n, a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
