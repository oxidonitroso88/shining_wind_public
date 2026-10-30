"""Inspect and patch RAX containers (Shining Wind resource archives) and their NUL separated text blocks. Read only on the inputs.

    python -I rax_tool.py list    <file.RAX>                     entries: type, size, name
    python -I rax_tool.py strings <file.RAX> [--entry BSS]       text block(s): offset, slot bytes, text  (TSV on stdout)
    python -I rax_tool.py patch   <file.RAX> <edits.tsv> <out>   same-size patch: every line "offset<TAB>new text" (offset in hex or
                                                                 decimal, as printed by `strings`); the new text is encoded as ASCII
                                                                 (or Shift-JIS with --sjis) and must fit the slot (NUL padded)

The RAX layout (verified on PAC_TOWN03_01, PAC_ROUTE01_01, SYSTEM, TITLE, MENUWINDOW, SYSSOUND): 'RAX\\0', u32 0x10, u32 0x20000, u32 entry
count, then entries {fourcc, u32 total size, u32 header size, u32 payload size, name[...]} one after another; a 'BSS\\0' entry is an event
script ('TNVE' header + bytecode) whose last part is a block of NUL terminated Shift-JIS lines ("\\n" separates the two lines of a dialogue
box). Fonts live in SYSTEM.RAX as LZSS compressed 'FNT'/'TXR' entries. Variable length text needs the script bytecode references rebuilt
(docs/translation-preparation-2026-10-13.md); this tool only does what is safe without that: in place replacement.
"""
import struct
import sys


def entries(d):
    count = struct.unpack_from("<I", d, 12)[0]
    pos = 0x10
    out = []
    for _ in range(count):
        if pos + 16 > len(d):
            break
        kind = d[pos:pos + 4].rstrip(b"\0").decode("ascii", "replace")
        size, header, payload = struct.unpack_from("<III", d, pos + 4)
        if size == 0 or pos + size > len(d):
            break
        name = d[pos + 16:pos + header].split(b"\0")[0].decode("ascii", "replace") if header > 16 else ""
        out.append({"pos": pos, "type": kind, "size": size, "header": header, "payload": payload, "name": name})
        pos += size
    return out


def text_block(d, entry):
    """(start, end) of the trailing text block of a BSS entry: scan back while bytes are NUL, LF or >= 0x20."""
    end = entry["pos"] + entry["size"]
    start = end
    while start > entry["pos"] + entry["header"] and (d[start - 1] in (0, 0x0A) or d[start - 1] >= 0x20):
        start -= 1
    while start < end and d[start] == 0:
        start += 1
    return start, end


def strings(d, start, end):
    out, pos = [], start
    for raw in d[start:end].split(b"\0"):
        out.append((pos, len(raw), raw))
        pos += len(raw) + 1
    return out


def decode(raw):
    try:
        return raw.decode("shift_jis")
    except UnicodeDecodeError:
        return raw.decode("shift_jis", "replace")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    d = open(path, "rb").read()
    if d[:4] != b"RAX\0":
        raise SystemExit("not a RAX file")
    if cmd == "list":
        for e in entries(d):
            print("0x%07x %-5s 0x%07x %s" % (e["pos"], e["type"], e["size"], e["name"]))
    elif cmd == "strings":
        want = sys.argv[sys.argv.index("--entry") + 1] if "--entry" in sys.argv else "BSS"
        for e in entries(d):
            if e["type"] != want:
                continue
            start, end = text_block(d, e)
            for off, n, raw in strings(d, start, end):
                if n:
                    print("0x%x\t%d\t%s" % (off, n, decode(raw).replace("\n", "\\n")))
    elif cmd == "patch":
        edits, out_path = sys.argv[3], sys.argv[4]
        codec = "shift_jis" if "--sjis" in sys.argv else "ascii"
        data = bytearray(d)
        for line in open(edits, encoding="utf-8").read().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            off_text, text = line.split("\t", 1)
            off = int(off_text, 0)
            end = data.index(0, off)
            slot = end - off
            raw = text.replace("\\n", "\n").encode(codec)
            if len(raw) > slot:
                raise SystemExit("0x%x: %d bytes do not fit the %d byte slot" % (off, len(raw), slot))
            data[off:off + slot] = raw + b"\0" * (slot - len(raw))
        open(out_path, "wb").write(bytes(data))
        print("patched ->", out_path)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
