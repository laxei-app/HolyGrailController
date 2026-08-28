#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""エッジのファームを組み立てて、公開リポジトリへ置ける形にする(2026-08-26)。

一連の手順を1つにまとめてある。手で写すと SHA256 や大きさを取り違えるため。

  1. 機種ごとにビルドする(版数はビルドのたびに上がる)
  2. 決まった番地へ名前と版数と検査値を刻む(stamp_fw.py)
  3. ブートローダ・区切り・otadata・本体を1本へ結合する(0番地へ焼く形)
  4. hgc-master/firmware/ へ置き、目録(manifest.json)を書き直す

置いたあとの push は**しない**。中身を確かめてから、手で push すること。

  python publish_fw.py --local      # 実機へ焼く用。**公開リポジトリを触らない**(既定の使い方)
  python publish_fw.py              # 公開リポジトリへ置く(リリース時だけ)
  python publish_fw.py --only core-s3
  python publish_fw.py --no-build   # 既にあるビルド成果物を使う

【公開リポジトリは指示があるまで更新しない(2026-08-28 ユーザー指示)】
公開物はその機能の検証時とリリース時にしか使わない。普段の実機書き込みは --local を使い、
hgc-master には手を触れないこと。--local はビルド成果物の場所に結合イメージを残すだけ。
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

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT_REPO = os.path.abspath(os.path.join(REPO, "..", "hgc-master"))

PIO = os.path.expanduser(r"~\.platformio\penv\Scripts\platformio.exe")
PY = os.path.expanduser(r"~\.platformio\penv\Scripts\python.exe")
ESPTOOL = os.path.expanduser(
    r"~\.platformio\packages\tool-esptoolpy@src-d6a55a0c35a704543947cf0793b9f212\esptool.py")
BOOT_APP0 = os.path.expanduser(
    r"~\.platformio\packages\framework-arduinoespressif32@src-702d0f93023d86e22d8ef62aa333f0b7"
    r"\tools\partitions\boot_app0.bin")

APP_NAME = "HolyGrailEdge"

MODELS = [
    {"id": "stick-s3", "name": "M5StickS3",      "target": "15_M5StickS3",
     "flash": "8MB",  "file": "hgc-edge-stick-s3.bin"},
    {"id": "core-s3",  "name": "M5Stack CoreS3", "target": "10_M5Stack",
     "flash": "16MB", "file": "hgc-edge-core-s3.bin"},
]


def run(args, cwd=None):
    r = subprocess.run(args, cwd=cwd, capture_output=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout.decode("utf-8", "replace"))
        sys.stdout.write(r.stderr.decode("utf-8", "replace"))
        raise SystemExit("失敗: %s" % " ".join(str(a) for a in args[:3]))
    return r.stdout.decode("utf-8", "replace")


def edge_version():
    p = os.path.join(REPO, "40_src", "10_UI", "18_M5Common", "edgeVersion.h")
    m = re.search(r'HGC_EDGE_VERSION\s+"([^"]+)"', io.open(p, encoding="utf-8-sig").read())
    return m.group(1)


def build_one(m, do_build):
    tdir = os.path.join(REPO, "40_src", "90_Target", m["target"])
    bdir = os.path.join(tdir, ".pio", "build", "debug")
    if do_build:
        print("  ビルド中…")
        run([PIO, "run", "-e", "debug"], cwd=tdir)
    ver = edge_version()

    fw = os.path.join(bdir, "firmware.bin")
    stamped = os.path.join(bdir, "firmware.stamped.bin")
    print("  版数 %s を刻む" % ver)
    run([sys.executable, os.path.join(HERE, "stamp_fw.py"), fw,
         "-o", stamped, "--name", APP_NAME, "--version", ver])

    merged = os.path.join(bdir, m["file"])
    print("  1本へ結合する(%s)" % m["flash"])
    run([PY, ESPTOOL, "--chip", "esp32s3", "merge_bin", "-o", merged,
         "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", m["flash"],
         "0x0", os.path.join(bdir, "bootloader.bin"),
         "0x8000", os.path.join(bdir, "partitions.bin"),
         "0xe000", BOOT_APP0,
         "0x10000", stamped])

    # 結合したものが本当に起動できる形か、esptool に見てもらう
    info = run([PY, ESPTOOL, "--chip", "esp32s3", "image_info", "--version", "2", stamped])
    if "(invalid" in info:
        raise SystemExit("イメージの検査に通りません。刻み方を見直すこと")

    data = open(merged, "rb").read()
    return {
        "id": m["id"], "name": m["name"], "chip": "esp32s3", "flashSize": m["flash"],
        "version": ver, "build": "debug", "file": m["file"], "offset": 0,
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "_path": merged,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="機種を1つだけ (stick-s3 / core-s3)")
    ap.add_argument("--no-build", action="store_true", help="既にあるビルド成果物を使う")
    ap.add_argument("--local", action="store_true",
                    help="公開リポジトリを触らない(実機へ焼くだけ。既定の使い方)")
    args = ap.parse_args()

    targets = [m for m in MODELS if not args.only or m["id"] == args.only]
    if not targets:
        raise SystemExit("その機種はありません")

    if args.local:
        # 公開リポジトリには一切触らない。結合イメージの場所だけ知らせて終わる。
        for m in targets:
            print("[%s]" % m["name"])
            e = build_one(m, not args.no_build)
            print("  %s" % e["_path"])
            print("  %d バイト  sha256 %s" % (e["size"], e["sha256"]))
        print("※ --local なので公開リポジトリは触っていません。")
        return

    dst = os.path.join(OUT_REPO, "firmware")
    if not os.path.isdir(dst):
        raise SystemExit("公開リポジトリが見つかりません: %s" % dst)

    # 既存の目録を読み、今回作らない機種の項目はそのまま残す
    mpath = os.path.join(dst, "manifest.json")
    old = json.load(io.open(mpath, encoding="utf-8")) if os.path.exists(mpath) else {"edge": []}
    entries = {e["id"]: e for e in old.get("edge", [])}

    for m in targets:
        print("[%s]" % m["name"])
        e = build_one(m, not args.no_build)
        shutil.copyfile(e.pop("_path"), os.path.join(dst, e["file"]))
        entries[e["id"]] = e
        print("  %s  %d バイト  %s" % (e["file"], e["size"], e["sha256"][:16] + "…"))

    out = {"schema": 1, "updated": old.get("updated", ""),
           "edge": [entries[k] for k in ("stick-s3", "core-s3") if k in entries]}
    io.open(mpath, "w", encoding="utf-8", newline="\n").write(
        json.dumps(out, ensure_ascii=False, indent=2) + "\n")
    print("目録を書き直した: %s" % mpath)
    print("※ push はしていません。中身を確かめてから手で push してください。")


if __name__ == "__main__":
    main()
