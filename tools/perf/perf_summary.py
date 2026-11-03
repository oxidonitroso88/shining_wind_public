#!/usr/bin/env python3
"""Summarises the CSV written by PS2X_PERF (see docs/perf-vega-instrumentation-2026-10-30.md).

  python tools/perf/perf_summary.py perf.csv [--skip 2] [--labels x1,x2,x4]

Per label (PS2X_PERF_LABEL) it takes the MEDIAN over the report windows (the first --skip windows are dropped: start-up, shader/pipeline warm-up), prints one
column per label and a few indicative readings. The readings are hints for where to look, not proofs: stages of different threads overlap in time.
"""
import argparse
import csv
import statistics
import sys
from collections import OrderedDict


def num(row, key):
    try:
        return float(row[key])
    except (KeyError, ValueError):
        return 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--skip", type=int, default=2, help="windows to drop at the start of every label (default 2)")
    ap.add_argument("--labels", default="", help="comma separated labels, in order (default: order of appearance)")
    ap.add_argument("--keep-stalled", action="store_true", help="keep windows where the guest advanced < 10%% of real time (dropped by default: the game was not running)")
    ap.add_argument("--timeline", default="", help="label: print one line per window (time, scene load, game speed, p99, waits) instead of the medians")
    a = ap.parse_args()
    rows = list(csv.DictReader(open(a.csv, newline="", encoding="utf-8")))
    if not rows:
        sys.exit("empty csv")
    # The same label can appear in several separate runs (perf_matrix.ps1 -Configs x1,x1, or two matrices appended to one CSV): every run restarts its window
    # counter / clock, so a new run starts where "window" or "t_end_s" goes back. The first run keeps the label, the next ones are "label#2", "label#3"...
    groups = OrderedDict()
    runs_of = {}
    prev = {}
    for r in rows:
        lab = r["label"]
        key = (num(r, "window"), num(r, "t_end_s"))
        if lab in prev and (key[0] <= prev[lab][0] or (key[1] and key[1] < prev[lab][1])):
            runs_of[lab] = runs_of.get(lab, 1) + 1
        prev[lab] = key
        n = runs_of.get(lab, 1)
        r["label"] = lab if n == 1 else "%s#%d" % (lab, n)
        groups.setdefault(r["label"], []).append(r)
    split = [(l, runs_of[l]) for l in runs_of]
    for l, n in split:
        print("note: label %s appears in %d separate runs -> reported as %s" % (l, n, ", ".join([l] + ["%s#%d" % (l, i) for i in range(2, n + 1)])))
    labels = [l for l in a.labels.split(",") if l] or list(groups)
    if a.timeline:
        def thread_pct(r, name):
            for part in r.get("threads", "").split(";"):
                if part.startswith(name + ":"):
                    try:
                        return float(part.split(":")[1])
                    except ValueError:
                        pass
            return -1.0
        print("%-7s %-7s %-9s %-8s %-8s %-9s %-9s %-9s %-9s %-10s %s" % ("window", "t_end_s", "speed%", "p99_ms", "max_ms", "proc_cpu%", "gpu_wait%", "ee_mtx%", "EEthr_cpu%", "frame", "threads (CPU% of one core)"))
        sampled = any(r.get("ee_samples") for r in groups.get(a.timeline, []))
        for r in groups.get(a.timeline, []):
            print("%-7s %-7s %-9.0f %-8.1f %-8.1f %-9.0f %-9.1f %-9.1f %-9.0f %-10s %s" % (r["window"], r.get("t_end_s", "?"), num(r, "game_speed_pct"), num(r, "p99_ms"), num(r, "max_ms"), num(r, "process_cpu_pct"),
                  num(r, "main.wait_gpu_idle_busy_pct"), num(r, "ee.wait_backend_mutex_busy_pct") + num(r, "ee.wait_frontend_mutex_busy_pct"), thread_pct(r, "GameThread"), "%sx%s" % (r["frame_w"], r["frame_h"]), r.get("threads", "")))
            if sampled:
                print("        game thread samples: %s" % r.get("ee_samples", ""))
        return
    cols = []
    for l in labels:
        g = groups.get(l, [])
        kept = g[a.skip:] if len(g) > a.skip else g
        stalled = [r for r in kept if num(r, "game_speed_pct") < 10.0]
        if stalled and not a.keep_stalled:
            kept = [r for r in kept if num(r, "game_speed_pct") >= 10.0]
        cols.append((l, kept, len(g)))
        if g and not kept:
            print("INVALID [%s]: in every window the guest advanced < 10%% of real time. The game was NOT running (disc files not found / boot stalled), so these numbers do not measure the renderer. "
                  "Launch the game the way you normally play it (see LEEME) and repeat." % l)
        elif stalled and not a.keep_stalled:
            print("note [%s]: %d window(s) dropped because the game was stalled (< 10%% speed)" % (l, len(stalled)))

    def med(kept, key):
        return statistics.median([num(r, key) for r in kept]) if kept else 0.0

    names = [k[:-len("_busy_pct")] for k in rows[0] if k.endswith("_busy_pct")]
    head = ["metric"] + [c[0] for c in cols]
    print("%-34s" % head[0] + "".join("%14s" % h for h in head[1:]))
    print("%-34s" % "windows used / total" + "".join("%14s" % ("%d/%d" % (len(c[1]), c[2])) for c in cols))
    for key, title in [("gpu", None), ("scale", None), ("output", None)]:
        print("%-34s" % key + "".join("%14s" % (c[1][0][key][:13] if c[1] else "") for c in cols))
    for key in ["frame_w", "frame_h", "host_loops_per_s", "game_speed_pct", "p50_ms", "p95_ms", "p99_ms", "max_ms", "late_gt20", "late_gt33", "process_cpu_pct"]:
        print("%-34s" % key + "".join("%14.1f" % med(c[1], key) for c in cols))
    if "sif_trace_lines_per_s" in rows[0]:
        for key in ["sif_trace_lines_per_s", "sif_trace_suppressed_per_s"]:
            print("%-34s" % key + "".join("%14.1f" % med(c[1], key) for c in cols))
    print("\nstage busy % of wall time (one thread = 100):")
    for n in names:
        vals = [med(c[1], n + "_busy_pct") for c in cols]
        if max(vals) < 0.05:
            continue
        print("%-34s" % n + "".join("%14.1f" % v for v in vals))
    print("\nms per call:")
    for n in names:
        vals = [med(c[1], n + "_ms_per_call") for c in cols]
        if max(vals) < 0.0005:
            continue
        print("%-34s" % n + "".join("%14.3f" % v for v in vals))

    print("\nReadings (indicative):")
    for l, kept, _ in cols:
        if not kept:
            continue
        sp = med(kept, "game_speed_pct")
        gpu = sum(med(kept, "gpu." + s + "_busy_pct") for s in ["shading", "binning", "triangle_setup", "vsync", "texture_upload", "copy_vram", "pgs_readback", "present_blit_copy"])
        mtx = med(kept, "ee.wait_backend_mutex_busy_pct") + med(kept, "ee.wait_frontend_mutex_busy_pct")
        wait = med(kept, "main.wait_gpu_idle_busy_pct")
        copy = med(kept, "main.host_copy_busy_pct") + med(kept, "main.upload_frame_busy_pct")
        pgs = med(kept, "ee.pgs_gif_transfer_busy_pct") + med(kept, "ee.pgs_write_register_busy_pct") + med(kept, "ee.pgs_image_upload_busy_pct")
        out = ["[%s] game speed %.0f%%" % (l, sp)]
        if gpu > 0:
            out.append("GPU executing %.0f%% of wall%s" % (gpu, " (GPU-bound likely)" if gpu > 85 else ""))
        out.append("presenter waiting for GPU %.0f%%" % wait)
        out.append("EE blocked on presenter-held mutexes %.1f%%%s" % (mtx, " (serialisation EE<->presenter)" if mtx > 8 else ""))
        out.append("EE inside parallel-gs CPU side %.0f%%" % pgs)
        out.append("presenter frame handling (upload+copy) %.0f%%" % copy)
        print("  " + "; ".join(out))


if __name__ == "__main__":
    main()
