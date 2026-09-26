#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Relink the TwyLapse Edge firmware against your own build of Arduino-ESP32.

This script is part of the relink archive that accompanies every published
TwyLapse Edge firmware image, as required by section 6 of the GNU Lesser
General Public License, version 2.1, under which Arduino-ESP32 is licensed.

What it does:

  1. Recompiles every Arduino-ESP32 translation unit from the source tree you
     point it at, using the exact compiler command that was used for the
     official build (arduino/compile_commands.json).
  2. Replaces those objects inside the archives in objects/ (ar r).
  3. Runs the exact link command that produced the official firmware
     (link/link_command.txt).
  4. Converts the resulting ELF into a flashable image and merges it with the
     bootloader and the partition table.

Nothing else in the firmware is rebuilt: the rest is supplied as object code
in objects/.

Usage
-----
    python relink.py --arduino <dir> [--idf-libs <dir>] [--toolchain <dir>]

    --arduino    Your Arduino-ESP32 source tree (the directory that contains
                 cores/ and libraries/). This is the part you may modify.
    --idf-libs   The framework-arduinoespressif32-libs package (precompiled
                 ESP-IDF libraries and linker scripts). Defaults to the copy
                 in your PlatformIO installation.
    --toolchain  The toolchain-xtensa-esp-elf package. Defaults to the copy in
                 your PlatformIO installation.

    --jobs N     Parallel compiles (default: number of CPUs).
    --skip-compile   Reuse objects compiled by a previous run.

Output is written to out/:  firmware.elf, firmware.bin and firmware-merged.bin

Flashing
--------
    esptool.py --chip esp32s3 --port <PORT> --baud 921600 \
               write_flash 0x0 out/firmware-merged.bin

See BUILDING.md for the details and for the license terms.
"""
import argparse
import glob
import json
import multiprocessing
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
TMP = os.path.join(HERE, "out", "arduino-objects")


def default_package(name):
    """Find a PlatformIO package directory, tolerating the @version suffix."""
    root = os.path.expanduser(os.path.join("~", ".platformio", "packages"))
    if not os.path.isdir(root):
        return None
    exact = os.path.join(root, name)
    if os.path.isdir(exact):
        return exact
    hits = sorted(glob.glob(os.path.join(root, name + "@*")))
    return hits[-1] if hits else None


def exe(directory, *names):
    for n in names:
        for cand in (n, n + ".exe"):
            p = os.path.join(directory, "bin", cand)
            if os.path.exists(p):
                return p
    return None


def substitute(text, paths):
    for key, value in paths.items():
        text = text.replace(key, value.replace("\\", "/"))
    return text


def run_long(cmd, rsp_path, cwd=None):
    """Run a compiler/linker command that is too long for the shell.

    Windows refuses command lines over 8191 characters and these are far
    longer, so everything after the program name goes into a GCC response
    file, which has no such limit.

    Everything after the program name is written out verbatim, quotes and all.
    It must not be re-quoted: some arguments carry quotes that are part of
    their value (-DMBEDTLS_CONFIG_FILE="mbedtls/esp_config.h"), and others
    contain spaces inside a quoted string (-DARDUINO_VARIANT="M5Stack CoreS3").
    GCC parses a response file with the same rules as the shell, so passing the
    original text through keeps both cases intact.
    """
    cmd = cmd.strip()
    if cmd.startswith('"'):
        end = cmd.index('"', 1)
        prog, rest = cmd[1:end], cmd[end + 1:]
    else:
        i = cmd.find(" ")
        prog, rest = cmd[:i], cmd[i + 1:]
    os.makedirs(os.path.dirname(rsp_path) or ".", exist_ok=True)
    with open(rsp_path, "w", encoding="utf-8") as f:
        f.write(rest)      # 原文のまま。\" を潰すと -DX=\"y\" が壊れる
    return subprocess.run([prog, "@" + rsp_path], cwd=cwd, capture_output=True)


def compile_one(job):
    cmd, out_path = job
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    r = run_long(cmd, out_path + ".rsp")
    if r.returncode != 0:
        return (out_path, r.stdout.decode("utf-8", "replace") +
                r.stderr.decode("utf-8", "replace"))
    return (out_path, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arduino", required=True,
                    help="your Arduino-ESP32 source tree (contains cores/ and libraries/)")
    ap.add_argument("--idf-libs", default=None)
    ap.add_argument("--toolchain", default=None)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--skip-compile", action="store_true")
    a = ap.parse_args()

    arduino = os.path.abspath(a.arduino)
    idf_libs = a.idf_libs or default_package("framework-arduinoespressif32-libs")
    toolchain = a.toolchain or default_package("toolchain-xtensa-esp-elf")
    for label, path in (("Arduino-ESP32", arduino), ("ESP-IDF libs", idf_libs),
                        ("toolchain", toolchain)):
        if not path or not os.path.isdir(path):
            sys.exit("Cannot find the %s directory: %s" % (label, path))
    if not os.path.isdir(os.path.join(arduino, "cores")):
        sys.exit("--arduino must point at the tree that contains cores/ and libraries/")

    paths = {"@ARDUINO@": arduino, "@IDF_LIBS@": idf_libs, "@TOOLCHAIN@": toolchain}
    os.makedirs(OUT, exist_ok=True)

    # PATH に通しておく。リンクコマンドはコンパイラを名前だけで呼ぶ。
    os.environ["PATH"] = os.path.join(toolchain, "bin") + os.pathsep + os.environ["PATH"]

    db = json.load(open(os.path.join(HERE, "arduino", "compile_commands.json"),
                        encoding="utf-8"))
    members = json.load(open(os.path.join(HERE, "arduino", "archive_members.json"),
                             encoding="utf-8"))

    # --- 1. recompile Arduino-ESP32 -------------------------------------
    jobs = []
    for e in db:
        out_path = os.path.join(TMP, e["object"])
        cmd = substitute(e["command"], paths)
        cmd = re.sub(r'-o\s+("[^"]+"|\S+)', '-o "%s"' % out_path.replace("\\", "/"), cmd, count=1)
        jobs.append((cmd, out_path))

    if not a.skip_compile:
        if os.path.isdir(TMP):
            shutil.rmtree(TMP)
        n = a.jobs or multiprocessing.cpu_count()
        print("Compiling %d Arduino-ESP32 files with %d jobs..." % (len(jobs), n))
        pool = multiprocessing.Pool(n)
        bad = 0
        for out_path, err in pool.imap_unordered(compile_one, jobs):
            if err:
                bad += 1
                print("FAILED: %s\n%s" % (os.path.basename(out_path), err[-2000:]))
        pool.close()
        pool.join()
        if bad:
            sys.exit("%d file(s) failed to compile." % bad)
    print("Compiled %d objects." % len(jobs))

    # --- 2. put them back into the archives ------------------------------
    ar = exe(toolchain, "xtensa-esp-elf-ar", "xtensa-esp32s3-elf-ar")
    if not ar:
        sys.exit("Cannot find ar in the toolchain.")
    by_name = {os.path.basename(p): p for _c, p in jobs}
    replaced = 0
    for archive, names in members.items():
        apath = os.path.join(HERE, archive.replace("/", os.sep))
        if not os.path.exists(apath):
            continue
        todo = [by_name[n] for n in names if n in by_name]
        if not todo:
            continue
        r = subprocess.run([ar, "r", apath] + todo, capture_output=True)
        if r.returncode != 0:
            sys.exit("ar failed on %s:\n%s" % (archive, r.stderr.decode("utf-8", "replace")))
        replaced += len(todo)
        print("  %-42s %3d object(s) replaced" % (archive, len(todo)))
    print("Replaced %d objects in the archives." % replaced)
    if replaced == 0:
        print("WARNING: nothing was replaced - check that --arduino is the right tree.")

    # --- 3. link ---------------------------------------------------------
    cmd = open(os.path.join(HERE, "link", "link_command.txt"), encoding="utf-8").read().strip()
    cmd = substitute(cmd, paths)
    print("Linking...")
    r = run_long(cmd, os.path.join(OUT, "link.rsp"), cwd=HERE)
    sys.stdout.write(r.stdout.decode("utf-8", "replace"))
    if r.returncode != 0:
        sys.stdout.write(r.stderr.decode("utf-8", "replace")[-8000:])
        sys.exit("Link failed.")
    elf = os.path.join(OUT, "firmware.elf")
    if not os.path.exists(elf):
        sys.exit("Link reported success but out/firmware.elf is missing.")
    print("Linked: out/firmware.elf (%d bytes)" % os.path.getsize(elf))

    # --- 4. make a flashable image ---------------------------------------
    ref = json.load(open(os.path.join(HERE, "reference", "reference.json"), encoding="utf-8"))
    try:
        import esptool                                     # noqa: F401
        esptool_cmd = [sys.executable, "-m", "esptool"]
    except ImportError:
        cand = sorted(glob.glob(os.path.expanduser(
            os.path.join("~", ".platformio", "packages", "tool-esptoolpy*", "esptool.py"))))
        if not cand:
            print("\nesptool was not found. Install it with:  pip install esptool")
            print("Then run these two commands from this directory:")
            print("  esptool.py --chip esp32s3 elf2image --flash_mode dio --flash_freq 80m"
                  " --flash_size %s --elf-sha256-offset 0xb0 -o out/firmware.bin"
                  " out/firmware.elf" % ref["flashSize"])
            print("  esptool.py --chip esp32s3 merge_bin -o out/firmware-merged.bin"
                  " --flash_mode dio --flash_freq 80m --flash_size %s"
                  " 0x0 link/bootloader.bin 0x8000 link/partitions.bin"
                  " 0xe000 link/boot_app0.bin 0x10000 out/firmware.bin" % ref["flashSize"])
            return
        esptool_cmd = [sys.executable, cand[-1]]

    common = ["--chip", "esp32s3"]
    flash = ["--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", ref["flashSize"]]
    subprocess.check_call(esptool_cmd + common + ["elf2image"] + flash +
                          ["--elf-sha256-offset", "0xb0",
                           "-o", os.path.join(OUT, "firmware.bin"), elf])
    subprocess.check_call(esptool_cmd + common + ["merge_bin", "-o",
                          os.path.join(OUT, "firmware-merged.bin")] + flash +
                          ["0x0", os.path.join(HERE, "link", "bootloader.bin"),
                           "0x8000", os.path.join(HERE, "link", "partitions.bin"),
                           "0xe000", os.path.join(HERE, "link", "boot_app0.bin"),
                           "0x10000", os.path.join(OUT, "firmware.bin")])

    print("\nDone.")
    print("  out/firmware.bin          %d bytes" % os.path.getsize(os.path.join(OUT, "firmware.bin")))
    print("  out/firmware-merged.bin   %d bytes" % os.path.getsize(os.path.join(OUT, "firmware-merged.bin")))
    print("\nFlash it with:")
    print("  esptool.py --chip esp32s3 --port <PORT> --baud 921600 \\")
    print("             write_flash 0x0 out/firmware-merged.bin")
    print("\nNote: writing from address 0x0 erases the stored settings (device name,")
    print("Wi-Fi credentials, owner). The device recreates its access point on the")
    print("next boot and has to be set up again from the app.")


if __name__ == "__main__":
    main()
