"""Copies the recompiler output of the overlays into the runtime tree (ps2xRuntime/src/runner_overlays, git-ignored).

    python -I ovl_install.py --build <out/overlay-build> [--only ovl09] [--dest ps2xRuntime/src/runner_overlays]

Each overlay is generated into its own directory (<build>/<ovl>/generated). File names are unique across overlays (function names
carry the overlay prefix, headers are ps2_<ovl>_functions.h, the registration is register_<ovl>.cpp), so the destination is flat.
Files of an overlay that was regenerated are replaced; stale files of that overlay are removed; other overlays are untouched.
"""
import argparse
import os
import shutil

ap = argparse.ArgumentParser()
ap.add_argument("--build", required=True)
ap.add_argument("--only")
ap.add_argument("--dest", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "ps2xRuntime", "src", "runner_overlays"))
a = ap.parse_args()
os.makedirs(a.dest, exist_ok=True)
for name in sorted(os.listdir(a.build)):
    gen = os.path.join(a.build, name, "generated")
    if not os.path.isdir(gen) or (a.only and name != a.only):
        continue
    prefix_cpp = name + "_"
    for fn in os.listdir(a.dest):  # stale files of this overlay
        if fn.startswith(prefix_cpp) or fn in ("register_%s.cpp" % name, "ps2_%s_functions.h" % name):
            os.remove(os.path.join(a.dest, fn))
    n = 0
    for fn in os.listdir(gen):
        shutil.copyfile(os.path.join(gen, fn), os.path.join(a.dest, fn))
        n += 1
    print("%s: %d files -> %s" % (name, n, os.path.abspath(a.dest)))
