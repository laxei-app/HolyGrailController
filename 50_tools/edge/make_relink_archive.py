#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Arduino-ESP32 を差し替えるための「再リンク材料」を作る(2026-09-27)。

【なぜ要るか】
エッジのファームは Arduino-ESP32(LGPL-2.1-or-later)を静的にリンクしている。
LGPL 2.1 の §6 は、配った相手が**そのライブラリを自分の改造版に差し替えられる**ことを
求める。ESP32 には動的リンクの仕組みが無いので §6(b) は使えない。残る道が §6(a) の
「ライブラリのソース + 自分の成果物を**オブジェクトコードまたはソースで**渡す」で、
条文が object code を認めているため、**こちらのソースは1行も出さずに済む**。

さらに §6(d) は「ダウンロードで配るなら材料も同じ場所から取れるようにせよ」と言う。
ファームは tlp-master/firmware/ に置いているので、材料も同じ所(firmware/archive/)へ置く。

【作るもの】
機種ごとに 1 つの zip。中身は

  objects/        自前コードと同梱ライブラリのオブジェクト(--strip-debug 済み)
  link/           リンカスクリプト・リンクコマンドそのもの・ブートローダ等
  arduino/        Arduino-ESP32 由来のファイルの「コンパイル方法」と、どの書庫に入るかの対応
  relink.py       改造版 Arduino-ESP32 で組み直して焼ける形にするまでを自動で行う
  BUILDING.md     手順書(英語)
  MANIFEST.txt    全ファイルの sha256

【使い方】
  python make_relink_archive.py                  # 両機種。release ビルド
  python make_relink_archive.py --only core-s3
  python make_relink_archive.py --env debug      # 公開ファームに合わせたいとき
  python make_relink_archive.py --no-build       # 既にあるビルド成果物を使う
  python make_relink_archive.py --out <dir>      # 置き場所を変える

置いたあとの push は**しない**。中身を確かめてから手で push すること。
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_REPO = os.path.abspath(os.path.join(REPO, "..", "tlp-master"))

PIO = os.path.expanduser(r"~\.platformio\penv\Scripts\platformio.exe")
PKGS = os.path.expanduser(r"~\.platformio\packages")

MODELS = [
    {"id": "stick-s3", "name": "M5StickS3",      "target": "15_M5StickS3", "flash": "8MB"},
    {"id": "core-s3",  "name": "M5Stack CoreS3", "target": "10_M5Stack",   "flash": "16MB"},
]

# リンクコマンドの中の、配る人の PC に固有の場所。zip では目印に置き換え、relink.py が戻す。
PLACEHOLDERS = [
    ("framework-arduinoespressif32-libs", "@IDF_LIBS@"),
    ("framework-arduinoespressif32",      "@ARDUINO@"),
    ("toolchain-xtensa-esp-elf",          "@TOOLCHAIN@"),
]


def run(args, cwd=None, env=None):
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run(args, cwd=cwd, capture_output=True, env=e)
    out = r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")
    if r.returncode != 0:
        sys.stdout.write(out[-4000:])
        raise SystemExit("失敗: %s" % " ".join(str(a) for a in args[:4]))
    return out


def edge_version():
    p = os.path.join(REPO, "40_src", "10_UI", "18_M5Common", "edgeVersion.h")
    m = re.search(r'HGC_EDGE_VERSION\s+"([^"]+)"', io.open(p, encoding="utf-8-sig").read())
    return m.group(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def strip_debug(path):
    """デバッグ情報を落とす。リンクに要るのはシンボル表だけで、情報量は 1/4 になる。"""
    tool = None
    for name in ("xtensa-esp-elf-strip.exe", "xtensa-esp32s3-elf-strip.exe"):
        c = os.path.join(PKGS, "toolchain-xtensa-esp-elf", "bin", name)
        if os.path.exists(c):
            tool = c
            break
    if tool:
        subprocess.run([tool, "--strip-debug", path], capture_output=True)


def build(tdir, env, do_build):
    """ビルドして、リンクコマンドそのものを採る。-v でないとコマンドが出てこない。"""
    bdir = os.path.join(tdir, ".pio", "build", env)
    if do_build:
        elf = os.path.join(bdir, "firmware.elf")
        if os.path.exists(elf):
            os.remove(elf)          # 消さないとリンクが走らずコマンドが採れない
        print("  ビルド中(%s)…" % env)
        log = run([PIO, "run", "-e", env, "-v"], cwd=tdir, env={"HGC_NO_BUMP": "1"})
        io.open(os.path.join(bdir, "link_build.log"), "w",
                encoding="utf-8", newline="\n").write(log)
        print("  コンパイル方法の一覧を作る…")
        run([PIO, "run", "-e", env, "-t", "compiledb"], cwd=tdir, env={"HGC_NO_BUMP": "1"})
    log = io.open(os.path.join(bdir, "link_build.log"), encoding="utf-8").read()
    for line in log.splitlines():
        if line.startswith("xtensa-") and "-o " in line and "firmware.elf" in line:
            return bdir, line.strip()
    raise SystemExit("リンクコマンドが見つかりません(-v で作り直すこと)")


def rewrite(tok, tdir, mapping):
    """リンクコマンドの 1 トークンを、zip の中の配置に合わせて書き換える。"""
    # -Wl,-Map=... はこちらの絶対パスなので出力先へ向け直す
    if tok.startswith("-Wl,-Map="):
        return '-Wl,-Map=out/firmware.map'
    # 【接頭辞を先に見る】"" を先に試すと "-LC:/…" の -L ごと置き換わって落ちる。
    for pre in ("-L", "-I", "-T", ""):
        if pre and not tok.startswith(pre):
            continue
        b = tok[len(pre):].strip('"').replace("\\", "/")
        if not b:
            continue
        new = None
        if b.startswith(".pio/"):
            if b in mapping:
                new = mapping[b]
            elif b.endswith("firmware.elf"):
                new = "out/firmware.elf"
            elif b.count("/") == 2:             # -L.pio/build/<env>
                new = "objects"
        else:
            for needle, ph in PLACEHOLDERS:
                i = b.find(needle)
                if i < 0:
                    continue
                rest = b[i + len(needle):]
                rest = rest[rest.find("/"):] if "/" in rest else ""
                new = ph + rest
                break
        if new is not None:
            return pre + new
        if pre == "":
            break
    return tok


def collect(bdir, tdir, link_cmd, stage):
    """リンクコマンドが参照している .o/.a を集め、書き換えたコマンドを返す。"""
    toks = link_cmd.split()
    mapping = {}
    objdir = os.path.join(stage, "objects")
    for t in toks:
        b = t.strip('"').replace("\\", "/")
        if not (b.endswith(".o") or b.endswith(".a")) or not b.startswith(".pio/"):
            continue
        src = os.path.join(tdir, b.replace("/", os.sep))
        if not os.path.exists(src):
            raise SystemExit("参照されているのに見つかりません: " + src)
        # .pio/build/<env>/X → objects/X   /  .pio/build/20_HolyGrailEntity/X → objects/_entity/X
        parts = b.split("/")
        rel = ("_entity/" + "/".join(parts[3:])) if parts[2] != os.path.basename(bdir) \
            else "/".join(parts[3:])
        dst = os.path.join(objdir, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        strip_debug(dst)
        mapping[b] = "objects/" + rel
    out = " ".join(rewrite(t, tdir, mapping) for t in toks)
    return out, mapping


def arduino_parts(tdir, bdir, stage, mapping):
    """Arduino-ESP32 由来のファイルについて「どう作ったか」「どの書庫に入るか」を書き出す。
    これがあれば、改造版のソースから同じ .o を作り直して差し替えられる。"""
    cdb = os.path.join(tdir, "compile_commands.json")
    if not os.path.exists(cdb):
        cdb = os.path.join(bdir, "compile_commands.json")
    db = json.load(io.open(cdb, encoding="utf-8"))
    ard = []
    for e in db:
        f = e["file"].replace("\\", "/")
        if "framework-arduinoespressif32" not in f or "framework-arduinoespressif32-libs" in f:
            continue
        i = f.find("framework-arduinoespressif32")
        rest = f[i + len("framework-arduinoespressif32"):]
        rest = rest[rest.find("/"):] if "/" in rest else rest
        cmd = e["command"] if "command" in e else " ".join(e.get("arguments", []))
        # 【一括置換は禁物】コンパイル行には -DX=\"y\" のようにエスケープされた引用符が
        #  混ざっている。パス区切りの \ だけを直し、引用符の前の \ は残す。
        cmd = re.sub(r'\\(?!")', "/", cmd)
        # 【順番が要る】"framework-arduinoespressif32" は "…-libs" の前置きでもあるので、
        #  長い方から先に置き換えないと "@ARDUINO@-libs" という壊れた名前になる。
        pkg = r"[A-Za-z]:/Users/[^/]+/\.platformio/packages/"
        cmd = re.sub(pkg + r"framework-arduinoespressif32-libs", "@IDF_LIBS@", cmd)
        cmd = re.sub(pkg + r"framework-arduinoespressif32(@[^/\\\" ]*)?", "@ARDUINO@", cmd)
        cmd = re.sub(pkg + r"toolchain-xtensa-esp-elf(@[^/\\\" ]*)?", "@TOOLCHAIN@", cmd)
        # 【こちらの -I を落とす】PlatformIO は全てのコンパイルに自分の include 先を足すので、
        #  Arduino-ESP32 のファイルにもこちらのソースの場所(絶対パス)が付いてくる。
        #  arduino-esp32 はこちらのヘッダを読まないので外して構わない。配る物に開発機の
        #  パスと内部の構成を載せない(残っていないことは下で数えて確かめる)。
        cmd = re.sub(r'\s-I"?' + re.escape(REPO.replace("\\", "/")) + r'[^\s"]*"?', " ", cmd)
        ard.append({"source": "@ARDUINO@" + rest, "command": cmd,
                    "object": os.path.basename(f) + ".o"})

    # どの .a にどの .o が入っているか(改造後に ar で入れ替えるため)
    ar = None
    for name in ("xtensa-esp-elf-ar.exe", "xtensa-esp32s3-elf-ar.exe"):
        c = os.path.join(PKGS, "toolchain-xtensa-esp-elf", "bin", name)
        if os.path.exists(c):
            ar = c
            break
    members = {}
    for src, rel in mapping.items():
        if not rel.endswith(".a"):
            continue
        p = os.path.join(stage, rel.replace("/", os.sep))
        if ar:
            r = subprocess.run([ar, "t", p], capture_output=True)
            members[rel] = r.stdout.decode("utf-8", "replace").split()
    # 開発機のパスが1つでも残っていたら作らない(公開物なので)
    leaked = [a["source"] for a in ard
              if re.search(re.escape(REPO.replace("\\", "/")), a["command"], re.I)]
    if leaked:
        raise SystemExit("こちらのパスがコンパイル行に残っています(%d件): %s" %
                         (len(leaked), leaked[0]))

    d = os.path.join(stage, "arduino")
    os.makedirs(d, exist_ok=True)
    io.open(os.path.join(d, "compile_commands.json"), "w", encoding="utf-8", newline="\n").write(
        json.dumps(ard, ensure_ascii=False, indent=1) + "\n")
    io.open(os.path.join(d, "archive_members.json"), "w", encoding="utf-8", newline="\n").write(
        json.dumps(members, ensure_ascii=False, indent=1) + "\n")
    return len(ard)


def copy_link_assets(bdir, stage, link_cmd):
    d = os.path.join(stage, "link")
    os.makedirs(d, exist_ok=True)
    for n in ("bootloader.bin", "partitions.bin"):
        p = os.path.join(bdir, n)
        if os.path.exists(p):
            shutil.copyfile(p, os.path.join(d, n))
    boot0 = None
    for root, _dirs, files in os.walk(os.path.join(PKGS)):
        if "boot_app0.bin" in files and "framework-arduinoespressif32" in root:
            boot0 = os.path.join(root, "boot_app0.bin")
            break
    if boot0:
        shutil.copyfile(boot0, os.path.join(d, "boot_app0.bin"))
    io.open(os.path.join(d, "link_command.txt"), "w", encoding="utf-8", newline="\n").write(
        link_cmd + "\n")


def write_manifest(stage):
    lines = []
    for root, _d, files in os.walk(stage):
        for f in sorted(files):
            p = os.path.join(root, f)
            rel = os.path.relpath(p, stage).replace("\\", "/")
            if rel == "MANIFEST.txt":
                continue
            lines.append("%s  %12d  %s" % (sha256(p), os.path.getsize(p), rel))
    io.open(os.path.join(stage, "MANIFEST.txt"), "w", encoding="utf-8", newline="\n").write(
        "\n".join(sorted(lines, key=lambda x: x.split("  ")[-1])) + "\n")


def make_zip(stage, zpath):
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for root, _d, files in os.walk(stage):
            for f in sorted(files):
                p = os.path.join(root, f)
                z.write(p, os.path.relpath(p, stage).replace("\\", "/"))
    return os.path.getsize(zpath)


def one(m, env, do_build, outdir):
    print("[%s]" % m["name"])
    tdir = os.path.join(REPO, "40_src", "90_Target", m["target"])
    bdir, link_cmd = build(tdir, env, do_build)
    ver = edge_version()
    stage = os.path.join(bdir, "relink_stage")
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    print("  オブジェクトを集める…")
    new_cmd, mapping = collect(bdir, tdir, link_cmd, stage)
    copy_link_assets(bdir, stage, new_cmd)
    n = arduino_parts(tdir, bdir, stage, mapping)
    print("  Arduino-ESP32 由来 %d ファイル / 書庫とオブジェクト %d 個" % (n, len(mapping)))

    # 作った本物の検査値。組み直した結果と見比べられるように残す。
    ref = os.path.join(stage, "reference")
    os.makedirs(ref, exist_ok=True)
    info = {"model": m["id"], "name": m["name"], "version": ver, "env": env,
            "chip": "esp32s3", "flashSize": m["flash"]}
    for n2 in ("firmware.elf", "firmware.bin"):
        p = os.path.join(bdir, n2)
        if os.path.exists(p):
            info[n2] = {"size": os.path.getsize(p), "sha256": sha256(p)}
    io.open(os.path.join(ref, "reference.json"), "w", encoding="utf-8", newline="\n").write(
        json.dumps(info, ensure_ascii=False, indent=2) + "\n")

    shutil.copyfile(os.path.join(HERE, "relink_kit", "relink.py"),
                    os.path.join(stage, "relink.py"))
    shutil.copyfile(os.path.join(HERE, "relink_kit", "BUILDING.md"),
                    os.path.join(stage, "BUILDING.md"))
    write_manifest(stage)

    os.makedirs(outdir, exist_ok=True)
    zpath = os.path.join(outdir, "tlp-edge-%s-%s-relink.zip" % (m["id"], ver))
    size = make_zip(stage, zpath)
    print("  %s  %.1f MB" % (os.path.basename(zpath), size / 1024.0 / 1024.0))
    return {"id": m["id"], "name": m["name"], "version": ver, "build": env,
            "file": os.path.basename(zpath), "size": size, "sha256": sha256(zpath)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="機種を1つだけ (stick-s3 / core-s3)")
    ap.add_argument("--env", default="release", help="ビルドの種類 (release / debug)")
    ap.add_argument("--no-build", action="store_true", help="既にあるビルド成果物を使う")
    ap.add_argument("--out", help="置き場所(既定 = tlp-master/firmware/archive)")
    args = ap.parse_args()

    targets = [m for m in MODELS if not args.only or m["id"] == args.only]
    if not targets:
        raise SystemExit("その機種はありません")
    outdir = args.out or os.path.join(OUT_REPO, "firmware", "archive")

    entries = []
    for m in targets:
        entries.append(one(m, args.env, not args.no_build, outdir))

    mpath = os.path.join(outdir, "manifest.json")
    old = json.load(io.open(mpath, encoding="utf-8")) if os.path.exists(mpath) else {}
    keep = {e["id"]: e for e in old.get("archive", [])}
    for e in entries:
        keep[e["id"]] = e
    out = {"schema": 1, "archive": [keep[k] for k in ("stick-s3", "core-s3") if k in keep]}
    io.open(mpath, "w", encoding="utf-8", newline="\n").write(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    print("目録を書いた: %s" % mpath)
    print("※ push はしていません。中身を確かめてから手で push してください。")


if __name__ == "__main__":
    main()
