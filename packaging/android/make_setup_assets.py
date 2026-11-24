#!/usr/bin/env python3
"""The Android APK's first-start build kit (agent L, 2026-11-23) - what SetupActivity needs to build the game ON THE
DEVICE from the player's own disc. The APK holds no game code: the same player package as the PC release, an
Android engine build's libraries and the port's own objects, a compiler and a Python.

    python3 make_setup_assets.py --payload <a PC release's payload tools folder> --tree <the Android engine build>
        --device-toolchain <agent B's device_toolchain> --ndk <NDK> --python <python.org Android prefix>
        --native <the app's jni build> --out <folder> [--overlay <file>=<payload path> ...]

--payload: the PC release's payload (unpacked; what its launcher unpacks to data/tools): engine/ (the translator),
  game/ (the player package), tools/ (the port's step scripts), include/ (the headers the translated code is compiled
  against), lift.txt, LICENSES/ - taken as they are (the PC and Android translate the same).
--tree: the port's Android CMake build (NDK, arm64): its engine libraries and its own objects (main, overrides, dub)
  go in; the compile flags are read from its build.ninja (the port's target), the paths mapped into the kit.
--overlay: a payload file replaced (e.g. engine/tools/translate.py with a fix not in that release yet).

Out:
  <out>/assets/setup/python.zip     the stdlib (+ lib-dynload) for libpython_exe.so (XR_PYTHON_HOME)
  <out>/assets/setup/tools.zip      the payload + lib/ (Android) + build.txt
  <out>/assets/setup/toolchain.zip  sysroot (+ libc++, API 30) and clang's resource dir
  <out>/assets/setup/version.txt    one line: the kit's id (the setup's stamp: a new kit -> the game is built again)
  <out>/libs/arm64-v8a/             the programs (lib*_exe.so) and libraries the APK carries - never libmain.so
  <out>/assets/licenses/            the third-party licence texts (About and licences)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import zipfile

API = 30            # the engine's (pthread_cond_clockwait); B's device toolchain sysroot is API 29 - 30 added from the NDK


def ninja_vars(text: str, want) -> dict:
    flat = re.sub(r"\$\r?\n\s*", "", text)
    for m in re.finditer(r"^build ([^\n]*)\n((?:  [^\n]*\n)*)", flat, re.M):
        outs = m.group(1).split(":")[0]
        if want(outs):
            v = {}
            for line in m.group(2).splitlines():
                k, _, val = line.strip().partition(" = ")
                v[k] = val
            return v
    raise SystemExit("build.ninja: no matching build statement")


def zip_tree(zf: zipfile.ZipFile, src: str, arc: str, skip=()) -> None:
    for root, dirs, files in os.walk(src):
        dirs[:] = sorted(d for d in dirs if d not in skip and d != "__pycache__")
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, src)
            zf.write(p, os.path.join(arc, rel) if arc else rel)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--payload", required=True)
    ap.add_argument("--tree", required=True)
    ap.add_argument("--device-toolchain", required=True)
    ap.add_argument("--ndk", required=True)
    ap.add_argument("--python", required=True)
    ap.add_argument("--native", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--overlay", action="append", default=[])
    a = ap.parse_args(argv)

    host = os.path.join(a.ndk, "toolchains/llvm/prebuilt/linux-x86_64")
    ndk_sys = os.path.join(host, "sysroot")
    strip = os.path.join(host, "bin/llvm-strip")
    shutil.rmtree(a.out, ignore_errors=True)
    setup = os.path.join(a.out, "assets/setup")
    libs = os.path.join(a.out, "libs/arm64-v8a")
    lic = os.path.join(a.out, "assets/licenses")
    for d in (setup, libs, lic):
        os.makedirs(d)

    # ---- the compile + link lines (build.txt), from the Android build's ninja file
    ninja = open(os.path.join(a.tree, "build.ninja"), encoding="utf-8").read()
    v = ninja_vars(ninja, lambda outs: "zoe_us.dir" in outs and "/lifted_000.cpp.o" in outs)
    defines = shlex.split(v["DEFINES"])     # (ninja's \" = a quote in the value: shlex keeps it)
    defines = [d for d in defines if not d.endswith("_EXPORTS")]
    flags = [f for f in shlex.split(v["FLAGS"]) if f not in ("-g",)]
    drv = ["--driver-mode=g++", "--target=aarch64-linux-android%d" % API, "--sysroot={TC}/sysroot",
           "-resource-dir={TC}/lib/clang/23"]
    includes = ["-I{TOOLS}/include/%d" % i for i in range(7)]
    cc = drv + flags + ["-Wno-#warnings"] + defines + includes
    libs_order = ["libps2host.a", "libchdr-static.a", "libchdr-lzma.a", "libminiz.a", "libzstd.a", "libps2rt.a",
                  "libps2gpu.a", "libps2platform.a"]
    link = drv + ["--ld-path={NATIVE}/libld_exe.so", "-shared", "-static-libstdc++", "-O3", "-Wl,--build-id=sha1",
                  "-Wl,--no-undefined", "-Wl,--gc-sections", "-Wl,--strip-debug", "-Wl,-z,max-page-size=16384",
                  "-Wl,-soname,libZOE_game.so", "-o", "{OUT}", "{OBJECTS}"] \
        + ["{TOOLS}/lib/%s" % l for l in libs_order] \
        + ["-llog", "-landroid", "{NATIVE}/libSDL3.so", "{TOOLS}/lib/libvolk.a", "-ldl", "-pthread", "-latomic", "-lm"]
    build_txt = "\n".join([
        "# the port's compile and link on the device (make_setup_assets.py, from the Android build's build.ninja);",
        "# {TOOLS} the kit, {TC} the toolchain, {WORK} the translation, {NATIVE} nativeLibraryDir, {OUT} the game library",
        "\t".join(["cc"] + cc),
        "glob\t{WORK}/game/src/lifted\t.cpp",
        "glob\t{WORK}/game/build/port/generated\t.cpp",
        "obj\t{TOOLS}/lib/main.o", "obj\t{TOOLS}/lib/overrides.o", "obj\t{TOOLS}/lib/dub.o",
        "\t".join(["link"] + link), ""])

    # ---- tools.zip: the payload + Android lib/ + build.txt
    objs = {}
    for name, rx in (("main.o", r"/host/src/main\.cpp\.o$"), ("overrides.o", r"/overrides\.cpp\.o$"),
                     ("dub.o", r"/dub\.cpp\.o$")):
        found = [p for p in re.findall(r"^build (CMakeFiles/zoe_us\.dir/\S+\.o):", ninja, re.M) if re.search(rx, p)]
        if len(found) != 1:
            raise SystemExit("build.ninja: %s: %r" % (name, found))
        objs[name] = os.path.join(a.tree, found[0])
    tree_libs = {"libps2host.a": "ps2host/libps2host.a", "libchdr-static.a": "libchdr/libchdr-static.a",
                 "libchdr-lzma.a": "libchdr/deps/lzma-26.02/libchdr-lzma.a",
                 "libminiz.a": "libchdr/deps/miniz-3.1.2/libminiz.a", "libzstd.a": "libchdr/deps/zstd-1.5.7/libzstd.a",
                 "libps2rt.a": "ps2host/libps2rt.a", "libps2gpu.a": "ps2gpu/libps2gpu.a",
                 "libps2platform.a": "ps2platform/libps2platform.a", "libvolk.a": "_deps/volk-build/libvolk.a"}
    overlays = {}
    for o in a.overlay:
        src, _, dst = o.partition("=")
        overlays[dst] = src
    with zipfile.ZipFile(os.path.join(setup, "tools.zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for part in ("engine", "game", "tools", "include", "LICENSES"):
            for root, dirs, files in os.walk(os.path.join(a.payload, part)):
                dirs[:] = sorted(d for d in dirs if d != "__pycache__")
                for f in sorted(files):
                    rel = os.path.relpath(os.path.join(root, f), a.payload)
                    z.write(overlays.pop(rel, os.path.join(root, f)), rel)
        z.write(os.path.join(a.payload, "lift.txt"), "lift.txt")
        z.write(os.path.join(a.payload, "release.txt"), "release.txt")
        # (debug info stripped: it names the build machine's folders, and the device build does not need it)
        stripped = os.path.join(a.out, "stripped")
        os.makedirs(stripped, exist_ok=True)
        for name, p in list(objs.items()) + [(n, os.path.join(a.tree, r)) for n, r in tree_libs.items()]:
            dst = os.path.join(stripped, name)
            subprocess.run([strip, "--strip-debug", "-o", dst, p], check=True)
            z.write(dst, "lib/" + name)
        shutil.rmtree(stripped)
        z.writestr("build.txt", build_txt)
    if overlays:
        raise SystemExit("--overlay: not in the payload: %s" % ", ".join(overlays))

    # ---- toolchain.zip: B's sysroot (API 29) + the NDK's libc++ (headers, static) + API 30, clang's resource dir
    dtc = a.device_toolchain
    ndk_lib = os.path.join(ndk_sys, "usr/lib/aarch64-linux-android")
    with zipfile.ZipFile(os.path.join(setup, "toolchain.zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        zip_tree(z, os.path.join(dtc, "sysroot"), "sysroot")
        zip_tree(z, os.path.join(dtc, "lib"), "lib")
        zip_tree(z, os.path.join(ndk_sys, "usr/include/c++"), "sysroot/usr/include/c++")
        for f in ("libc++_static.a", "libc++abi.a"):
            z.write(os.path.join(ndk_lib, f), "sysroot/usr/lib/aarch64-linux-android/" + f)
        for f in ("libc++.a", "libc++.so"):
            z.write(os.path.join(ndk_lib, "29", f), "sysroot/usr/lib/aarch64-linux-android/29/" + f)
        zip_tree(z, os.path.join(ndk_lib, str(API)), "sysroot/usr/lib/aarch64-linux-android/%d" % API)

    # ---- python.zip: the stdlib (no tests, no GUI)
    with zipfile.ZipFile(os.path.join(setup, "python.zip"), "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        zip_tree(z, os.path.join(a.python, "lib/python3.15"), "lib/python3.15",
                 skip=("test", "idlelib", "tkinter", "turtledemo", "ensurepip"))

    # ---- the kit's id: the three zips' hashes
    h = hashlib.sha256()
    for f in ("tools.zip", "toolchain.zip", "python.zip"):
        with open(os.path.join(setup, f), "rb") as fh:
            for block in iter(lambda: fh.read(1 << 20), b""):
                h.update(block)
    release = open(os.path.join(a.payload, "release.txt"), encoding="utf-8").read().split()[0]
    with open(os.path.join(setup, "version.txt"), "w") as fh:
        fh.write("%s %s\n" % (release, h.hexdigest()[:16]))

    # ---- the APK's native programs and libraries (never libmain.so: no game code in the APK)
    def put(src, name, do_strip=True):
        dst = os.path.join(libs, name)
        shutil.copy2(src, dst)
        if do_strip:
            subprocess.run([strip, "--strip-unneeded", dst], check=True)
    put(os.path.join(a.tree, "_deps/sdl3-build/libSDL3.so"), "libSDL3.so")
    def native(f):                      # (the jni build: its own targets at the top, libadrenotools' hooks in out/)
        for d in ("", "out"):
            p = os.path.join(a.native, d, f)
            if os.path.isfile(p):
                return p
        raise SystemExit("--native: no %s" % f)
    for f in ("libzoe_app.so", "libvulkan_zoe.so", "libpython_exe.so", "libld_exe.so", "libzoe_run_exe.so",
              "libchd2iso_exe.so", "libmain_hook.so", "libhook_impl.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so"):
        put(native(f), f)
    put(os.path.join(a.python, "lib/libpython3.15.so"), "libpython3.15.so")
    put(os.path.join(a.python, "lib/libcrypto_python.so"), "libcrypto_python.so")
    put(os.path.join(dtc, "bin/clang"), "libclang_exe.so", do_strip=False)
    put(os.path.join(dtc, "bin/ld.lld"), "liblld_exe.so", do_strip=False)

    # ---- licences
    def lic_copy(src, name):
        if os.path.isfile(src):
            shutil.copy2(src, os.path.join(lic, name))
    for root, _, files in os.walk(os.path.join(a.payload, "LICENSES")):
        for f in files:
            if f.lower().endswith((".txt", ".md")):
                lic_copy(os.path.join(root, f), f)
    lic_copy(os.path.join(a.python, "lib/python3.15/LICENSE.txt"), "CPython-PSF.txt")
    for root, _, files in os.walk(os.path.join(dtc, "LICENSES")):
        for f in files:
            lic_copy(os.path.join(root, f), "toolchain_" + f)
    with open(os.path.join(lic, "README.txt"), "w") as fh:
        fh.write("Zone of the Enders Recompiled for Android - third-party software\n\n"
                 "The app builds the game on this device from your own disc; it ships no game code or data.\n\n"
                 "ps2recomp option-p engine            MIT                 the port's engine\n"
                 "SDL 3                                zlib                libsdl.org\n"
                 "libadrenotools                       BSD-2-Clause        github.com/bylaws/libadrenotools\n"
                 "CPython 3.15 (embedded, translation) PSF License         python.org\n"
                 "LLVM clang + lld (device compiler)   Apache-2.0 WITH LLVM-exception   llvm.org\n"
                 "Android NDK sysroot, libc++          see toolchain_* and NOTICE texts\n"
                 "libchdr, miniz, zstd, lzma, volk     BSD / MIT / public domain\n"
                 "sse2neon, pl_mpeg                    MIT\n"
                 "Liberation fonts                     OFL\n\n"
                 "Licence texts: the files in this folder.\n")

    sizes = {f: os.path.getsize(os.path.join(setup, f)) >> 20 for f in os.listdir(setup)}
    print("setup kit: %s; libs: %s" % (sizes, sorted(os.listdir(libs))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
