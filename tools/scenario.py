#!/usr/bin/env python3
"""Repeatable scripted runs of a port, with everything an unattended session needs to judge them.

    python tools/scenario.py fight                       # Soulcalibur, default port
    python tools/scenario.py fight --set aspect=4:3 --set hud_fix=false --env DREAM_AICA_BATCH=1
    python tools/scenario.py menus --out work/sc/menus-a --shots 600,1200
    python tools/scenario.py list

A scenario is a button script (--press) and a frame count. Each run gets a fresh scratch settings
file, memory card and bindings file (the player's own are never touched), a fixed RTC seed, and
writes into its output folder:

    run.log / run.err   the launcher's report
    audio.wav           the mixer output; audio.txt: tools/audio_check.py on it
    shot_<frame>.png    screenshots at the listed presented frames, and sheet.png (contact sheet)
    summary.json        speed, audio problems, render counters: compare two runs with --compare

    python tools/scenario.py fight --compare work/sc/fight-a work/sc/fight-b

The fight scenario mashes A through Arcade into Kilik vs Voldo (stage 1) and keeps fighting; at
--speed real (default) it runs paced like play, --speed max unthrottled (audio drops then: judge
the wav, not the device).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fight_press():
    p = ["start@600", "start@900"]
    p += [f"a@{f}" for f in range(1100, 20000, 120)]
    return ",".join(p)


MAINMENU_MOVES = [("down", 1000), ("down", 1080), ("down", 1160), ("up", 1240), ("up", 1320),
                  ("up", 1400), ("down", 1480)]


def mainmenu_press():
    # Soulcalibur: through the title to the main menu, then move through the items (the menu
    # scrolls and the NEW badges flash: HUD anchoring must hold while they move).
    return ",".join(["start@600", "start@900"] + [f"{b}@{f}" for b, f in MAINMENU_MOVES])


def mainmenu_shots():
    return ",".join(str(f + d) for _, f in MAINMENU_MOVES for d in (2, 6, 10, 14, 20, 30))


def jgr_play_press():
    # Through the boot notices, the title menu (Start is only taken once the menu is up: pressed
    # every 100 frames), the story intro and Gum's tutorial dialogue (A every 90 frames), then
    # skating and jumping on the first street.
    p = ["start@600", "start@900"] + [f"start@{f}" for f in range(2100, 3100, 100)]
    p += [f"a@{f}" for f in range(3600, 20000, 90)]
    return ",".join(p)


ECCO_NEW_GAME = ("start@1300,a@1500,a@1700,a@1900,a@1960,a@2020,start@2100,a@2600")


def ecco_play_press():
    # Scenario runs get a fresh memory card, so this is the new-game path: title, Select VM (A),
    # Select a Game -> New Game (A), initials AAA (A x3, Start = END), main menu -> Start Game.
    # On the player's own card AAA already exists and the game refuses a second one: there,
    # pick the AAA entry on Select a Game instead. Then the prologue film (~frame 2300-11000)
    # and the first scene's tutorial dialogue (A every 120 frames).
    p = [ECCO_NEW_GAME] + [f"a@{f}" for f in range(12000, 20000, 120)]
    return ",".join(p)


# Per port id: name -> (frames, press, default shots, what it covers).
PORT_SCENARIOS = {
    "ecco": {
        "boot": (2000, "start@1300", "200,400,700,1000,1400,1600,1900",
                 "SEGA and Appaloosa logos, title screen, Select VM"),
        "menus": (3000, ECCO_NEW_GAME.rsplit(",", 1)[0], "1600,1800,2050,2500,2900",
                  "fresh card: Select VM, New Game, initials AAA, main menu"),
        "play": (20000, ecco_play_press(), "2700,4500,8000,10000,11500,13000,16000,19500",
                 "new game, prologue film, first controllable scene and its tutorial dialogue"),
        "attract": (9000, "", "1400,3000,4500,6000,7500,8900",
                    "no input: logos, title and whatever the game shows when left alone"),
    },
    "jetgrindradio": {
        "boot": (3000, "start@600,start@900", "300,900,1200,1500,1800,2100,2700",
                 "VMU notice, SEGA and CRI logos, graffiti notice, loading, title flyby"),
        "play": (9600, jgr_play_press(), "2500,2900,3500,4000,5500,7000,8500,9500",
                 "title menu, new game, story intro, Gum's tutorial, skating the first street"),
        "attract": (6000, "start@600,start@900", "2400,3000,3600,4200,4800,5400",
                    "no input after the notices: title flyby and the demo loop"),
    },
}

SCENARIOS = {
    # name: (frames, press, default shots, what it covers)
    "boot": (900, "", "300,600,880", "boot logos, title screen"),
    "menus": (2400, "start@600,start@900,a@1100,a@1300,start@1500,a@1700", "700,1000,1250,1450,1800,2300",
              "title, main menu, mode select, character select"),
    "fight": (4900, fight_press(), "1500,2500,3500,4500", "arcade: Kilik vs Voldo, stage 1"),
    "fight-long": (12000, fight_press(), "2500,4500,6500,8500,10500,11800", "several arcade fights"),
    "attract": (9000, "start@600", "900,1500,2100,2700,3300,3900,4500,5100,5700,6300,6900,7500,8100,8700",
                "no input: title, intro and the demo loop"),
    "mainmenu": (1600, mainmenu_press(), mainmenu_shots(),
                 "main menu, moving down and up through the items (shots just after each move)"),
}


def resolve_port(port):
    """(port dir, port id, exe, recorded disc) for a port id ("jetgrindradio"), slug
    ("jet-grind-radio", "jet-grind-radio-recomp") or folder."""
    import glob
    import json
    cands = [port] if os.path.isdir(port) else []
    cands += [os.path.join(ROOT, "ports", f"{port}-recomp"), os.path.join(ROOT, "ports", port)]
    for d in glob.glob(os.path.join(ROOT, "ports", "*")):
        if glob.glob(os.path.join(d, "game", f"{port}.toml")):
            cands.append(d)
    for d in cands:
        tomls = [t for t in glob.glob(os.path.join(d, "game", "*.toml")) if not t.endswith("suggested.toml")]
        if len(tomls) != 1:
            continue
        pid = os.path.splitext(os.path.basename(tomls[0]))[0]
        name = os.path.basename(os.path.normpath(d))
        exe = os.path.join(d, "build", "game", f"{name}.exe")
        if not os.path.exists(exe):
            exe = exe[:-4]
        disc = None
        local = os.path.join(d, ".dreamcomp", "local.json")
        if os.path.exists(local):
            disc = json.load(open(local, encoding="utf-8")).get("disc")
        return os.path.abspath(d), pid, exe, disc
    sys.exit(f"no port {port!r} under ports/")


def scenarios_for(pid):
    return PORT_SCENARIOS.get(pid, SCENARIOS)


def port_paths(port):
    pdir, _, exe, _ = resolve_port(port)
    return pdir, exe


def find_disc(port):
    games = os.path.join(ROOT, "game files")
    for d in os.listdir(games) if os.path.isdir(games) else []:
        if d.lower().replace(" ", "").startswith(port.lower()):
            for f in os.listdir(os.path.join(games, d)):
                if f.lower().endswith((".cue", ".gdi", ".chd")):
                    return os.path.join(games, d, f)
    return None


def run_scenario(a):
    _, pid, _, recorded_disc = resolve_port(a.port)
    table = scenarios_for(pid)
    if a.scenario not in table:
        sys.exit(f"{pid}: no scenario {a.scenario!r} ({', '.join(table)})")
    frames, press, shots, _ = table[a.scenario]
    frames = a.frames or frames
    shots = a.shots if a.shots is not None else shots
    pdir, exe = port_paths(a.port)
    if a.exe:
        exe = os.path.abspath(a.exe)  # another build of the same port (tools/abtest.py)
    disc = a.disc or recorded_disc or find_disc(a.port)
    out = os.path.abspath(a.out or os.path.join(ROOT, "work", "sc", f"{a.scenario}-{time.strftime('%H%M%S')}"))
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    with open(os.path.join(out, "settings.ini"), "w") as f:
        for kv in a.set:
            k, v = kv.split("=", 1)
            f.write(f"{k.strip()} = {v.strip()}\n")
    args = [exe, "--window", "--disc", disc, "--settings", os.path.join(out, "settings.ini"),
            "--vmu", os.path.join(out, "vmu.bin"), "--bindings", os.path.join(out, "bindings.txt"),
            "--rtc-seed", "1000000", "--max-frames", str(frames), "--wav", os.path.join(out, "audio.wav")]
    if a.exe:
        # A copied executable cannot find the config beside the build: name it.
        args += ["--config", os.path.join(pdir, "game", f"{pid}.toml")]
    # The play path (double-click) turns `scale` into --scale; flags runs do not, so do it here,
    # with the game's default of 2.
    scale = next((kv.split("=", 1)[1] for kv in a.set if kv.split("=", 1)[0].strip() == "scale"), "2")
    args += ["--scale", scale.strip()]
    if a.speed == "max":
        args += ["--unthrottled", "--present-mode", "immediate"]
    if press:
        args += ["--press", press]
    if shots:
        args += ["--screenshot-at", shots]
    # The game runs in the output folder: file paths after `--` are made absolute against the
    # caller's directory first (as tools/dc.py does), or a relative --profile/--wav lands nowhere.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from dc import absolute_file_args
    args += absolute_file_args(a.extra)
    env = dict(os.environ)
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    t0 = time.time()
    with open(os.path.join(out, "run.log"), "w") as lo, open(os.path.join(out, "run.err"), "w") as le:
        rc = subprocess.call(args, cwd=out, stdout=lo, stderr=le, env=env)
    wall = time.time() - t0
    log = open(os.path.join(out, "run.log"), encoding="utf-8", errors="replace").read()
    summary = {"scenario": a.scenario, "rc": rc, "wall_s": round(wall, 1), "set": a.set, "env": a.env}
    m = re.search(r"host: ([\d.]+) s \(([\d.]+)x real time\)", log)
    if m:
        summary["host_s"], summary["speed_x"] = float(m.group(1)), float(m.group(2))
    # CPU cycles of the game thread: compares builds without the laptop's clock-speed noise.
    m = re.search(r"game thread: ([\d.]+) G CPU cycles", log)
    if m:
        summary["thread_gcycles"] = float(m.group(1))
    m = re.search(r"audio: (\d+) samples played in \d+ blocks, (\d+) dropped, (\d+) underruns", log)
    if m:
        summary["audio_dropped"], summary["audio_underruns"] = int(m.group(2)), int(m.group(3))
    m = re.search(r"window: (\d+) frames drawn .*?(\d+) presented.*?textures (\d+) decoded, (\d+) failed", log)
    if m:
        summary["drawn"], summary["presented"] = int(m.group(1)), int(m.group(2))
        summary["tex_decoded"], summary["tex_failed"] = int(m.group(3)), int(m.group(4))
    for key, pat in (("fault", r"^fault: (.*)$"), ("stop", r"^stop: (.*)$")):
        m = re.search(pat, log, re.M)
        if m:
            summary[key] = m.group(1)
    # Audio.
    wav = os.path.join(out, "audio.wav")
    if os.path.exists(wav):
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "audio_check.py"), wav, "--per-second",
                            "--top", "5"], capture_output=True, text=True)
        open(os.path.join(out, "audio.txt"), "w").write(r.stdout)
        clicks = [int(x) for x in re.findall(r"(\d+) clicks", r.stdout)]
        clips = [int(x) for x in re.findall(r"clipped (\d+) samples", r.stdout)]
        summary["audio_clicks"] = sum(clicks)
        summary["audio_clipped"] = sum(clips)
    # Screenshots.
    # DREAM_SHOT_INTERP=1 also writes screenshot-NNN-interp.ppm (the blended frame shown just
    # before frame NNN): saved as shot_<frame>_interp.png, kept out of the sheet and the compare.
    ppms = sorted(f for f in os.listdir(out) if f.startswith("screenshot-") and f.endswith(".ppm"))
    interp = {f.replace("-interp", "") for f in ppms if f.endswith("-interp.ppm")}
    ppms = [f for f in ppms if not f.endswith("-interp.ppm")]
    want = [int(x) for x in shots.split(",")] if shots else []
    pngs = []
    try:
        from PIL import Image, ImageDraw
        for n, f in zip(want, ppms):
            dst = os.path.join(out, f"shot_{n:05d}.png")
            Image.open(os.path.join(out, f)).save(dst)
            os.remove(os.path.join(out, f))
            pngs.append((n, dst))
            if f in interp:
                fi = f.replace(".ppm", "-interp.ppm")
                Image.open(os.path.join(out, fi)).save(os.path.join(out, f"shot_{n:05d}_interp.png"))
                os.remove(os.path.join(out, fi))
        if pngs:
            w, h = 427, 240
            cols = min(3, len(pngs))
            rows = (len(pngs) + cols - 1) // cols
            sheet = Image.new("RGB", (cols * w, rows * h))
            for i, (n, p) in enumerate(pngs):
                cell = Image.open(p).convert("RGB").resize((w, h))
                ImageDraw.Draw(cell).text((4, 4), f"f{n}", fill=(255, 255, 0))
                sheet.paste(cell, ((i % cols) * w, (i // cols) * h))
            sheet.save(os.path.join(out, "sheet.png"))
    except ImportError:
        pass
    summary["shots"] = len(pngs)
    json.dump(summary, open(os.path.join(out, "summary.json"), "w"), indent=1)
    print(json.dumps(summary))
    print(f"output: {out}")
    return 0 if rc == 0 else 1


def compare(dirs):
    rows = [json.load(open(os.path.join(d, "summary.json"))) for d in dirs]
    keys = ["thread_gcycles", "speed_x", "audio_clicks", "audio_clipped", "audio_dropped", "audio_underruns", "drawn",
            "presented", "tex_decoded", "tex_failed", "stop", "fault"]
    print("key".ljust(18) + "".join(os.path.basename(d.rstrip("/\\"))[:22].ljust(24) for d in dirs))
    for k in keys:
        print(k.ljust(18) + "".join(str(r.get(k, "-")).ljust(24) for r in rows))
    # Screenshots both runs took at the same frame: how much changed, and where (diff_*.png in
    # the second folder; changed pixels bright on a dimmed copy of the first run's frame).
    if len(dirs) == 2:
        try:
            from PIL import Image, ImageChops
        except ImportError:
            return 0
        a_dir, b_dir = dirs
        shots = sorted(f for f in os.listdir(a_dir)
                       if f.startswith("shot_") and f.endswith(".png") and "_interp" not in f)
        for f in shots:
            pb = os.path.join(b_dir, f)
            if not os.path.exists(pb):
                continue
            ia = Image.open(os.path.join(a_dir, f)).convert("RGB")
            ib = Image.open(pb).convert("RGB")
            if ia.size != ib.size:
                print(f"{f}: sizes differ {ia.size} vs {ib.size}")
                continue
            diff = ImageChops.difference(ia, ib).convert("L")
            hist = diff.histogram()
            total = ia.size[0] * ia.size[1]
            changed = total - sum(hist[:9])  # more than 8 levels apart
            mean = sum(i * n for i, n in enumerate(hist)) / total
            print(f"{f}: {100.0 * changed / total:.2f} % of pixels changed, mean difference {mean:.2f}")
            if changed:
                mask = diff.point(lambda v: 255 if v > 8 else 0)
                out = Image.blend(ia, Image.new("RGB", ia.size), 0.6)
                out.paste(Image.new("RGB", ia.size, (255, 40, 40)), mask=mask)
                out.save(os.path.join(b_dir, "diff_" + f[5:]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("scenario", help="a scenario of the port (`list` shows them)")
    ap.add_argument("--port", default="soulcalibur", help="port id, slug or folder")
    ap.add_argument("--disc")
    ap.add_argument("--out")
    ap.add_argument("--frames", type=int)
    ap.add_argument("--shots", help="presented frames to screenshot, comma separated ('' for none)")
    ap.add_argument("--set", action="append", default=[], help="settings key=value (scratch file)")
    ap.add_argument("--env", action="append", default=[], help="environment KEY=VALUE for the run")
    ap.add_argument("--speed", choices=["real", "max"], default="max")
    ap.add_argument("--compare", nargs="+", help="print summary.json of these output folders side by side")
    ap.add_argument("--exe", help="run this executable instead of the port's build (A/B timing)")
    ap.add_argument("extra", nargs="*", help="extra launcher flags after --")
    a = ap.parse_args()
    if a.compare:
        return compare(a.compare)
    if a.scenario == "list":
        for k, (frames, _, shots, what) in scenarios_for(resolve_port(a.port)[1]).items():
            print(f"{k:11} {frames:6} frames  {what}")
        return 0
    return run_scenario(a)


if __name__ == "__main__":
    sys.exit(main())
