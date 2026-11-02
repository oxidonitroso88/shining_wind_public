#!/usr/bin/env python3
"""Shining Wind launcher: pick the internal resolution, see which mods are installed, open the mods guide, start the game.

Double-click Launcher.cmd (or this file) next to ShiningWind.exe. It only sets the same environment variables that play-x4.cmd sets and starts ShiningWind.exe;
nothing is written next to the game except %APPDATA%\\ShiningWind\\launcher.json (remembered choices). Needs the Python that was used to build the port (tkinter ships with it).

  launcher.pyw --selftest [--res 1|2|4] [--no-mods] [--unlock]   prints the command and environment that would be used, starts nothing (no window)
"""
import json
import os
import subprocess
import sys
import webbrowser
from pathlib import Path

GUIDE_URL = "https://github.com/oxidonitroso88/shining_wind_public/blob/main/docs/MODS.es.md"
EXE_NAME = "ShiningWind.exe"

# The same variables as play-x4.cmd / the perf configurations; nothing else is touched.
RESOLUTIONS = {
    1: ("Original (x1) - 640x448", {}, "La resolucion del PS2. La mas ligera: cualquier PC con Vulkan."),
    2: ("Mejorada (x2)", {"PS2X_GS_SCALE": "2", "PS2X_GS_OUTPUT": "2"}, "Doble resolucion interna. Recomendada para PCs con GPU integrada."),
    4: ("HD (x4) - necesaria para los paquetes de texturas HD",
        {"PS2X_GS_SCALE": "4", "PS2X_GS_OUTPUT": "4", "PS2X_GS_PROMOTE": "1", "PS2X_GS_HD_SSAA": "1", "PS2X_GS_GPU_DOWNSCALE": "1"},
        "Cuatro veces la resolucion interna. Exige una GPU potente; en una GPU integrada puede ir por debajo de la velocidad real."),
}
UNLOCK = {"PS2X_GS_PRESENT_UNLOCK": "1"}


def default_data_dir():
    return Path(os.environ.get("APPDATA") or Path.home()) / "ShiningWind"


def config_path():
    return default_data_dir() / "launcher.json"


def load_config():
    try:
        return json.loads(config_path().read_text(encoding="utf-8"))
    except Exception:
        return {}


def save_config(cfg):
    try:
        config_path().parent.mkdir(parents=True, exist_ok=True)
        config_path().write_text(json.dumps(cfg, indent=1), encoding="utf-8")
    except Exception:
        pass  # remembering the choice is a convenience only


def mods_status(data_dir):
    """(has_translation, number of ST files, has_story, [hd pack names])"""
    mods = Path(data_dir) / "mods"
    st = mods / "ST"
    n_st = sum(1 for f in st.rglob("*") if f.is_file()) if st.is_dir() else 0
    story = (mods / "story_en.tsv").is_file()
    packs = []
    hd = Path(data_dir) / "hd_packs"
    if hd.is_dir():
        packs = sorted(d.name for d in hd.iterdir() if d.is_dir() and (d / "manifest.json").is_file())
    return n_st, story, packs


def build_launch(exe, res, data_dir, mods_off=False, unlock=False):
    """Command line and environment (a copy of os.environ with the chosen variables) for the game."""
    env = dict(os.environ)
    for k in ("PS2X_GS_SCALE", "PS2X_GS_OUTPUT", "PS2X_GS_PROMOTE", "PS2X_GS_HD_SSAA", "PS2X_GS_GPU_DOWNSCALE", "PS2X_GS_PRESENT_UNLOCK", "PS2X_MODS", "PS2X_STORY_EN"):
        env.pop(k, None)  # no leftovers from the user's own shell
    env.update(RESOLUTIONS[res][1])
    if unlock:
        env.update(UNLOCK)
    if mods_off:
        env["PS2X_MODS"] = "off"
        env["PS2X_STORY_EN"] = "off"
    cmd = [str(exe)]
    if Path(data_dir) != default_data_dir():
        cmd += ["--data-dir", str(data_dir)]
    return cmd, env


def selftest(argv):
    res = 1
    if "--res" in argv:
        res = int(argv[argv.index("--res") + 1])
    exe = Path(__file__).resolve().parent / EXE_NAME
    cmd, env = build_launch(exe, res, default_data_dir(), "--no-mods" in argv, "--unlock" in argv)
    print("command:", cmd)
    for k in sorted(env):
        if k.startswith("PS2X_"):
            print("  %s=%s" % (k, env[k]))
    print("mods:", mods_status(default_data_dir()))
    return 0


def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    here = Path(__file__).resolve().parent
    exe = here / EXE_NAME
    cfg = load_config()

    root = tk.Tk()
    root.title("Shining Wind")
    root.resizable(False, False)
    frm = ttk.Frame(root, padding=14)
    frm.grid(sticky="nsew")

    ttk.Label(frm, text="Shining Wind", font=("Segoe UI", 16, "bold")).grid(row=0, column=0, columnspan=2, sticky="w")
    ttk.Label(frm, text="Resolucion de render").grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 2))

    res_var = tk.IntVar(value=cfg.get("res", 2) if cfg.get("res", 2) in RESOLUTIONS else 2)
    hint = tk.StringVar()
    for i, (r, (label, _env, _h)) in enumerate(RESOLUTIONS.items()):
        ttk.Radiobutton(frm, text=label, value=r, variable=res_var, command=lambda: hint.set(RESOLUTIONS[res_var.get()][2])).grid(row=2 + i, column=0, columnspan=2, sticky="w")
    hint.set(RESOLUTIONS[res_var.get()][2])
    ttk.Label(frm, textvariable=hint, wraplength=420, foreground="#555").grid(row=5, column=0, columnspan=2, sticky="w", pady=(4, 8))

    data_var = tk.StringVar(value=cfg.get("data_dir") or str(default_data_dir()))
    ttk.Label(frm, text="Carpeta de datos (partidas, mods)").grid(row=6, column=0, columnspan=2, sticky="w")
    ttk.Entry(frm, textvariable=data_var, width=52).grid(row=7, column=0, sticky="we")

    def browse():
        d = filedialog.askdirectory(initialdir=data_var.get() or str(default_data_dir()))
        if d:
            data_var.set(os.path.normpath(d))
            refresh()
    ttk.Button(frm, text="Elegir...", command=browse).grid(row=7, column=1, padx=(6, 0))

    mods_off = tk.BooleanVar(value=bool(cfg.get("mods_off", False)))
    unlock = tk.BooleanVar(value=bool(cfg.get("unlock", False)))
    ttk.Checkbutton(frm, text="Jugar sin mods (juego original en japones)", variable=mods_off).grid(row=8, column=0, columnspan=2, sticky="w", pady=(10, 0))
    ttk.Checkbutton(frm, text="Presentacion sin bloqueo (EXPERIMENTAL: puede mejorar x2/x4 en GPUs lentas)", variable=unlock).grid(row=9, column=0, columnspan=2, sticky="w")

    status = tk.StringVar()
    ttk.Label(frm, textvariable=status, wraplength=420, justify="left").grid(row=10, column=0, columnspan=2, sticky="w", pady=(10, 0))

    def refresh():
        n_st, story, packs = mods_status(data_var.get())
        parts = []
        parts.append("Traduccion: %s" % ("instalada (%d archivos%s)" % (n_st, " + dialogos" if story else "") if n_st or story else "no instalada"))
        parts.append("Texturas HD: %s" % (", ".join(packs) if packs else "ninguna"))
        msg = "\n".join(parts)
        if packs and res_var.get() != 4:
            msg += "\nAviso: los paquetes HD solo se ven en la resolucion x4."
        if not (Path(data_var.get()) / "disc").exists() and not exe.exists():
            msg += "\nAviso: no se encuentra %s junto al launcher." % EXE_NAME
        status.set(msg)
    refresh()

    def play():
        if not exe.exists():
            messagebox.showerror("Shining Wind", "No encuentro %s en %s" % (EXE_NAME, here))
            return
        d = data_var.get().strip() or str(default_data_dir())
        save_config({"res": res_var.get(), "data_dir": d if Path(d) != default_data_dir() else "", "mods_off": mods_off.get(), "unlock": unlock.get()})
        cmd, env = build_launch(exe, res_var.get(), d, mods_off.get(), unlock.get())
        try:
            subprocess.Popen(cmd, env=env, cwd=str(here))
        except OSError as e:
            messagebox.showerror("Shining Wind", "No se pudo iniciar el juego:\n%s" % e)
            return
        root.destroy()

    def open_dir(sub):
        p = Path(data_var.get()) / sub
        p.mkdir(parents=True, exist_ok=True)
        os.startfile(str(p))  # noqa: windows only, like the game

    btn = ttk.Frame(frm)
    btn.grid(row=11, column=0, columnspan=2, sticky="we", pady=(12, 0))
    ttk.Button(btn, text="Jugar", command=play).grid(row=0, column=0, padx=(0, 6))
    ttk.Button(btn, text="Como instalar mods...", command=lambda: webbrowser.open(GUIDE_URL)).grid(row=0, column=1, padx=6)
    ttk.Button(btn, text="Carpeta mods", command=lambda: open_dir("mods")).grid(row=0, column=2, padx=6)
    ttk.Button(btn, text="Carpeta HD", command=lambda: open_dir("hd_packs")).grid(row=0, column=3, padx=6)
    ttk.Button(btn, text="Salir", command=root.destroy).grid(row=0, column=4, padx=(6, 0))

    res_var.trace_add("write", lambda *_: refresh())
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(selftest(sys.argv[1:]) if "--selftest" in sys.argv else run_gui())
