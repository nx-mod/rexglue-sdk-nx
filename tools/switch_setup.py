#!/usr/bin/env python3
"""Populate thirdparty/ for a Nintendo Switch (deko3d) build.

thirdparty/ is kept as git submodules. This inits only the ones a Switch build
needs, fetches the two deko3d-only sources (SPIRV-Cross, uam) at fixed commits,
and lays the Switch FFmpeg config (switch/overlay/) over the FFmpeg submodule.
It never touches the Vulkan / SDL / desktop submodules.

Run from the SDK root after cloning:  python tools/switch_setup.py
"""
import os, shutil, subprocess, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUBMODULES = ["cli11", "fmt", "spdlog", "simde", "xxHash", "imgui",
              "tomlplusplus", "utfcpp", "libmspack", "o1heap", "inja", "FFmpeg"]
# (url, commit, subfolder or '', dest under thirdparty/)
DEKO3D = [
    ("https://github.com/KhronosGroup/SPIRV-Cross.git",
     "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7", "", "SPIRV-Cross"),
    ("https://github.com/centollOS/SwitchWakerHD.git",
     "2d147aaaa27b1ad0f9b390eff515609af9fe24a3", "runtime/third_party/uam", "uam"),
]

def run(*a, cwd=None):
    print(">", *a); subprocess.run(a, cwd=cwd, check=True)

def fetch(url, commit, folder, dest):
    out = os.path.join(ROOT, "thirdparty", dest)
    if os.path.isdir(out) and os.listdir(out):
        print(f"thirdparty/{dest} present, skipping"); return
    work = tempfile.mkdtemp(prefix="dekodep-")
    try:
        run("git", "init", "-q", work)
        run("git", "remote", "add", "origin", url, cwd=work)
        if folder:
            run("git", "sparse-checkout", "set", folder, cwd=work)
            run("git", "fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit, cwd=work)
        else:
            run("git", "fetch", "-q", "--depth", "1", "origin", commit, cwd=work)
        run("git", "checkout", "-q", "FETCH_HEAD", cwd=work)
        src = os.path.join(work, folder) if folder else work
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(src):
            if name == ".git":
                continue
            s, d = os.path.join(src, name), os.path.join(out, name)
            (shutil.copytree if os.path.isdir(s) else shutil.copy2)(s, d)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print(f"fetched thirdparty/{dest}")

def overlay():
    base = os.path.join(ROOT, "switch", "overlay")
    for dirpath, _, files in os.walk(base):
        rel = os.path.relpath(dirpath, base)
        dst = os.path.join(ROOT, "thirdparty", rel)
        for f in files:
            os.makedirs(dst, exist_ok=True)
            shutil.copy2(os.path.join(dirpath, f), os.path.join(dst, f))
            print(f"overlay thirdparty/{os.path.normpath(os.path.join(rel, f))}")

if __name__ == "__main__":
    run("git", "submodule", "update", "--init", "--depth", "1",
        *[f"thirdparty/{m}" for m in SUBMODULES], cwd=ROOT)
    for t in DEKO3D:
        fetch(*t)
    overlay()
    print("Switch thirdparty setup complete — ready to configure with REXGLUE_PLATFORM_SWITCH=ON.")
