"""Lists direct branch targets in the recompiled main program that leave the function (`ctx->pc = X; return;`) but
have no slot in the function table (they would end as guest-branch:missing-target at run time)."""
import re, sys, glob, os
runner = sys.argv[1] if len(sys.argv) > 1 else 'D:/PS2Recomp/ps2xRuntime/src/runner'
reg = open(os.path.join(runner, 'register_functions.cpp'), encoding='utf-8', errors='replace').read()
registered = set(int(m, 16) for m in re.findall(r'\]\s*=\s*\w+;\s*//\s*0x([0-9a-fA-F]+)', reg))
pat = re.compile(r'ctx->pc = 0x([0-9A-Fa-f]+)u;\s*return;')
hdr = re.compile(r'// Address: 0x([0-9a-f]+) - 0x([0-9a-f]+)')
found = {}
for f in glob.glob(os.path.join(runner, '*.cpp')):
    if f.endswith('register_functions.cpp'): continue
    s = open(f, encoding='utf-8', errors='replace').read()
    h = hdr.search(s)
    if not h: continue
    lo, hi = int(h.group(1), 16), int(h.group(2), 16)
    for m in pat.finditer(s):
        t = int(m.group(1), 16)
        if lo <= t < hi: continue          # inside the same function
        if t not in registered:
            found.setdefault(t, set()).add(os.path.basename(f))
for t in sorted(found):
    print('0x%06x' % t, ' '.join(sorted(found[t]))[:100])
print('registered:', len(registered), 'unregistered cross-function targets:', len(found), file=sys.stderr)
