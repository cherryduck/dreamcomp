#!/usr/bin/env python3
"""dreamcomp workflow CLI: one entry point for humans and for Claude.

    python tools/dc.py doctor                         what this machine has / lacks
    python tools/dc.py deps                           fetch SDL3 into .deps/ (Windows: VC devel zip)
    python tools/dc.py setup  <port> --disc <image>   verify the disc, extract the boot binary into
                                                      <port>/game/extracted/, remember the image path
    python tools/dc.py build  <port> [--target T] [--fresh] [--headless]
    python tools/dc.py run    <port> [--window] [-- launcher args...]
    python tools/dc.py shots  <port> --at 120,300,900 [--press start@600] [--out DIR] [--scale N]
                                                      windowed screenshots at presented frames, plus a
                                                      contact sheet (sheet.png) -- the cheapest way to
                                                      see where a boot gets to
    python tools/dc.py report <port> [--frames N] [--press ...]   headless run report (stdout)

<port> is a port repository directory (ports/<slug>). Builds go to <port>/build. On Windows every
compiler call runs inside the Visual Studio developer environment, found with vswhere.
Machine-local state (the disc path) lives in <port>/.dreamcomp/local.json, which is git-ignored.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, ".deps")
SDL3_VERSION = "3.4.18"
IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""


# ---------------------------------------------------------------------------------------------
# environment

def vcvars() -> str | None:
    if not IS_WIN:
        return None
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    if os.path.exists(vswhere):
        out = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
        for line in out.splitlines():
            p = os.path.join(line, r"VC\Auxiliary\Build\vcvars64.bat")
            if os.path.exists(p):
                return p
    return None


_ENV_CACHE: dict | None = None


def build_env() -> dict:
    """os.environ plus the MSVC developer environment on Windows (cached per process)."""
    global _ENV_CACHE
    if _ENV_CACHE is not None:
        return _ENV_CACHE
    env = dict(os.environ)
    vc = vcvars()
    if vc:
        out = subprocess.run(f'cmd /s /c ""{vc}" >nul && set"', capture_output=True, text=True, shell=True).stdout
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
    vk = glob.glob(r"C:\VulkanSDK\*") if IS_WIN else []
    if vk and "VULKAN_SDK" not in env:
        env["VULKAN_SDK"] = sorted(vk)[-1]
    _ENV_CACHE = env
    return env


def run(cmd: list[str], cwd: str | None = None, check: bool = True, env: dict | None = None) -> int:
    print("+ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env or build_env())
    if check and r.returncode != 0:
        sys.exit(f"command failed with exit code {r.returncode}")
    return r.returncode


# ---------------------------------------------------------------------------------------------
# port helpers

def port_info(port: str) -> dict:
    port = os.path.abspath(port)
    tomls = glob.glob(os.path.join(port, "game", "*.toml"))
    tomls = [t for t in tomls if not t.endswith("suggested.toml")]
    if len(tomls) != 1:
        sys.exit(f"{port}: expected exactly one game/<id>.toml, found {len(tomls)}")
    gid = os.path.splitext(os.path.basename(tomls[0]))[0]
    local_path = os.path.join(port, ".dreamcomp", "local.json")
    local = json.load(open(local_path)) if os.path.exists(local_path) else {}
    exe_candidates = glob.glob(os.path.join(port, "build", "game", f"*{EXE}"))
    exe = next((e for e in exe_candidates if os.path.isfile(e) and (not IS_WIN or e.endswith(".exe"))), None)
    return {"dir": port, "id": gid, "toml": tomls[0], "local": local, "local_path": local_path, "exe": exe}


def save_local(info: dict) -> None:
    os.makedirs(os.path.dirname(info["local_path"]), exist_ok=True)
    with open(info["local_path"], "w") as f:
        json.dump(info["local"], f, indent=2)


def dcdisc_env() -> dict:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.path.join(ROOT, "engine", "tools", "dcdisc") + os.pathsep + env.get("PYTHONPATH", "")
    return env


# ---------------------------------------------------------------------------------------------
# commands

def cmd_doctor(_a) -> int:
    env = build_env()
    rows = []

    def have(name, ok, note=""):
        rows.append((name, "ok" if ok else "MISSING", note))

    for tool in ("git", "cmake", "ninja", "python"):
        have(tool, shutil.which(tool, path=env.get("PATH")) is not None)
    cc = "clang-cl" if IS_WIN else "clang++"
    have(cc, shutil.which(cc, path=env.get("PATH")) is not None, "C++20 compiler")
    if IS_WIN:
        have("MSVC environment (vcvars64)", vcvars() is not None, "Visual Studio 2022 Build Tools: Windows SDK + linker")
    glslc = shutil.which("glslc", path=env.get("PATH")) or (
        os.path.join(env["VULKAN_SDK"], "Bin", "glslc.exe") if env.get("VULKAN_SDK") else None)
    have("Vulkan SDK / glslc", bool(glslc and os.path.exists(glslc)), "winget install KhronosGroup.VulkanSDK")
    have("SDL3", bool(glob.glob(os.path.join(DEPS, "SDL3-*"))) or not IS_WIN, "python tools/dc.py deps")
    try:
        import pycdlib  # noqa: F401  (dcdisc tests)
        have("pycdlib (dcdisc tests)", True)
    except ImportError:
        have("pycdlib (dcdisc tests)", False, "pip install -e engine/tools/dcdisc[test]")
    w = max(len(r[0]) for r in rows)
    for name, st, note in rows:
        print(f"{name:<{w}}  {st:<7}  {note}")
    return 0 if all(r[1] == "ok" for r in rows) else 1


def cmd_deps(_a) -> int:
    os.makedirs(DEPS, exist_ok=True)
    if IS_WIN:
        dst = os.path.join(DEPS, f"SDL3-{SDL3_VERSION}")
        if os.path.isdir(dst):
            print(f"SDL3 {SDL3_VERSION} already in {dst}")
            return 0
        url = (f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL3_VERSION}/"
               f"SDL3-devel-{SDL3_VERSION}-VC.zip")
        zpath = os.path.join(DEPS, "sdl3.zip")
        print(f"downloading {url}")
        urllib.request.urlretrieve(url, zpath)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(DEPS)
        os.remove(zpath)
        print(f"SDL3 -> {dst}")
        return 0
    print("Linux/macOS: install SDL3 from your package manager (e.g. `brew install sdl3`, "
          "`apt install libsdl3-dev`) or build it from source; CMake finds it on the default prefix.")
    return 0


def cmd_setup(a) -> int:
    info = port_info(a.port)
    disc = os.path.abspath(a.disc)
    from_toml = open(info["toml"], encoding="utf-8").read()
    want = None
    for line in from_toml.splitlines():
        if line.strip().startswith("sha1_1st_read"):
            want = line.split("=", 1)[1].split("#")[0].strip().strip('"')
    # Only the boot executable is extracted: the translator needs it at build time, and at run
    # time everything else is read off the disc image itself.
    sys.path.insert(0, os.path.join(ROOT, "engine", "tools", "dcdisc"))
    from dcdisc import open_image
    from dcdisc.ipbin import parse_ipbin, read_ipbin
    from dcdisc.iso9660 import Iso9660
    from dcdisc.scramble import descramble
    with open_image(disc) as img:
        ip = parse_ipbin(read_ipbin(img))
        data = Iso9660(img).read_path(ip.boot_filename)
    if ip.hardware_id.startswith("SEGA SEGAKATANA") is False:
        print(f"warning: IP.BIN hardware id is {ip.hardware_id!r}")
    got = hashlib.sha1(data).hexdigest()
    if want and got != want and hashlib.sha1(descramble(data)).hexdigest() == want:
        data, got = descramble(data), want  # MIL-CD style scrambled boot file
    out = os.path.join(info["dir"], "game", "extracted", "fs")
    os.makedirs(out, exist_ok=True)
    boot = os.path.join(out, "1ST_READ.BIN")
    with open(boot, "wb") as f:
        f.write(data)
    print(f"{ip.title} {ip.product_number} {ip.product_version}: boot file {ip.boot_filename}, "
          f"{len(data)} bytes -> {boot}")
    if want and got != want:
        sys.exit(f"boot binary SHA-1 {got} does not match this port ({want}): wrong release or revision")
    print(f"boot binary verified: sha1 {got}")
    info["local"]["disc"] = disc
    save_local(info)
    print(f"disc path remembered in {info['local_path']}")
    return 0


def cmd_build(a) -> int:
    info = port_info(a.port)
    bdir = os.path.join(info["dir"], "build")
    if a.fresh and os.path.isdir(bdir):
        shutil.rmtree(bdir)
    if not os.path.exists(os.path.join(bdir, "CMakeCache.txt")):
        cfg = ["cmake", "-S", info["dir"], "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
               f"-DDREAMCOMP_DIR={ROOT}"]
        if IS_WIN:
            cfg += ["-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"]
        if a.headless:
            cfg += ["-DDREAM_RENDERER=OFF"]
        run(cfg)
    target = a.target or f"{info['id']}_boot"
    run(["cmake", "--build", bdir, "--target", target, "--parallel"])
    return 0


def launcher_args(info: dict, extra: list[str]) -> list[str]:
    exe = info["exe"]
    if not exe:
        sys.exit("no executable yet: python tools/dc.py build <port>")
    args = [exe, "--config", info["toml"]]
    if info["local"].get("disc") and "--disc" not in extra:
        args += ["--disc", info["local"]["disc"]]
    return args + extra


def cmd_run(a) -> int:
    info = port_info(a.port)
    extra = list(a.args)
    if extra and extra[0] == "--":
        extra = extra[1:]
    if a.window:
        extra = ["--window"] + extra
    return run(launcher_args(info, extra), cwd=a.cwd or info["dir"], check=False, env=dict(os.environ))


def cmd_report(a) -> int:
    info = port_info(a.port)
    extra = ["--max-frames", str(a.frames), "--rtc-seed", "1000000", "--no-audio"]
    if a.press:
        extra += ["--press", a.press]
    extra += a.extra
    return run(launcher_args(info, extra), cwd=info["dir"], check=False, env=dict(os.environ))


# Launcher flags whose value is a file or folder. `run` and `report` start the game in the port's
# folder, so a relative path written from the dreamcomp checkout (as the skills do) would land
# somewhere that does not exist, and the engine's "cannot write" is easy to miss: make such paths
# absolute against the caller's directory first.
FILE_FLAGS = {"--suggest-config", "--screenshot-dir", "--profile", "--wav", "--report", "--dump-ta", "--dump-vram",
              "--dump-flash", "--dump-aram", "--sample-file", "--settings", "--vmu", "--bindings",
              "--texture-pack", "--dump-textures", "--mod", "--disc", "--flash", "--capture-dir"}


def absolute_file_args(extra):
    out = list(extra)
    for i in range(len(out) - 1):
        if out[i] in FILE_FLAGS and not os.path.isabs(out[i + 1]):
            out[i + 1] = os.path.abspath(out[i + 1])
    return out


def cmd_shots(a) -> int:
    info = port_info(a.port)
    out = os.path.abspath(a.out or os.path.join(info["dir"], "shots"))
    os.makedirs(out, exist_ok=True)
    frames = sorted({int(x) for x in a.at.split(",")})
    made = []
    with tempfile.TemporaryDirectory() as tmp:
        # One run, every shot (--screenshot-at takes a list). --max-frames counts guest frames and
        # --screenshot-at presented ones; the guest runs ahead of presentation, so leave headroom.
        extra = ["--window", "--no-audio", "--unthrottled", "--present-mode", "immediate",
                 "--rtc-seed", "1000000", "--screenshot-at", ",".join(map(str, frames)),
                 "--max-frames", str(int(frames[-1] * 1.25) + 120), "--scale", str(a.scale)]
        if a.press:
            extra += ["--press", a.press]
        extra += a.extra
        run(launcher_args(info, extra), cwd=tmp, check=False, env=dict(os.environ))
        shots = sorted(glob.glob(os.path.join(tmp, "screenshot-*.ppm")))
        if len(shots) < len(frames):
            print(f"only {len(shots)} of {len(frames)} screenshots (run ended first?)")
        for n, src in zip(frames, shots):
            dst = os.path.join(out, f"frame_{n:05d}.png")
            try:
                from PIL import Image
                Image.open(src).save(dst)
            except ImportError:
                dst = dst[:-4] + ".ppm"
                shutil.copy(src, dst)
            made.append((n, dst))
    try:
        from PIL import Image, ImageDraw
        ims = [(n, Image.open(p).convert("RGB")) for n, p in made]
        if ims:
            w, h = 320, 240
            cols = min(4, len(ims))
            rows = (len(ims) + cols - 1) // cols
            sheet = Image.new("RGB", (cols * w, rows * h))
            for i, (n, im) in enumerate(ims):
                cell = im.resize((w, h))
                ImageDraw.Draw(cell).text((4, 4), f"f{n}", fill=(255, 255, 0))
                sheet.paste(cell, ((i % cols) * w, (i // cols) * h))
            sheet.save(os.path.join(out, "sheet.png"))
            print(f"contact sheet: {os.path.join(out, 'sheet.png')}")
    except ImportError:
        print("pip install pillow for PNG output and the contact sheet")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor").set_defaults(fn=cmd_doctor)
    sub.add_parser("deps").set_defaults(fn=cmd_deps)
    s = sub.add_parser("setup"); s.add_argument("port"); s.add_argument("--disc", required=True)
    s.set_defaults(fn=cmd_setup)
    s = sub.add_parser("build"); s.add_argument("port"); s.add_argument("--target")
    s.add_argument("--fresh", action="store_true"); s.add_argument("--headless", action="store_true")
    s.set_defaults(fn=cmd_build)
    s = sub.add_parser("run"); s.add_argument("port"); s.add_argument("--window", action="store_true")
    s.add_argument("--cwd"); s.add_argument("args", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_run)
    s = sub.add_parser("report"); s.add_argument("port"); s.add_argument("--frames", type=int, default=1800)
    s.add_argument("--press"); s.set_defaults(fn=cmd_report)
    s = sub.add_parser("shots"); s.add_argument("port"); s.add_argument("--at", required=True)
    s.add_argument("--press"); s.add_argument("--out"); s.add_argument("--scale", type=int, default=1)
    s.set_defaults(fn=cmd_shots)
    # Everything after a bare `--` goes to the launcher untouched (shots, report).
    argv = list(sys.argv[1:] if argv is None else argv)
    extra = []
    if "--" in argv and argv[0] != "run":
        cut = argv.index("--")
        argv, extra = argv[:cut], argv[cut + 1:]
    a = ap.parse_args(argv)
    a.extra = absolute_file_args(extra)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
