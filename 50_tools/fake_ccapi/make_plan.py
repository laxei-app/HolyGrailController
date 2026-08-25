#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""測定用のパノラマ撮影計画を作る。

スマホの既存計画を土台にして、
 ・主カメラ  = 所持カメラの1台目
 ・追加カメラ = 残り(--subs 台)
 ・panorama  = true
 ・開始 = いま + --lead 秒 / 終了 = 開始 + --minutes 分
だけ差し替える。ccm(撮影制御方法)や場所はそのまま活かす。
"""
import argparse
import io
import json
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ADB = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
PLAN_DIR = "/sdcard/Android/data/app.laxei.holygrail/files/plan"
OWNED = "/sdcard/Android/data/app.laxei.holygrail/files/asset/ownedCameras.json"


def adb(*a):
    r = subprocess.run([ADB] + list(a), capture_output=True)
    if r.returncode != 0:
        raise SystemExit("adb 失敗: %s\n%s" % (a, r.stderr.decode("utf-8", "ignore")))
    return r.stdout.decode("utf-8", "ignore")


def dt(epoch):
    t = time.localtime(epoch)
    return {"year": t.tm_year, "month": t.tm_mon, "day": t.tm_mday,
            "hour": t.tm_hour, "min": t.tm_min, "sec": t.tm_sec}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--subs", type=int, default=7, help="追加カメラの台数")
    ap.add_argument("--lead", type=int, default=40, help="何秒後に開始するか")
    ap.add_argument("--minutes", type=int, default=20, help="撮影の長さ[分]")
    ap.add_argument("--interval", type=float, default=15.0)
    ap.add_argument("--out", default=os.path.join(HERE, "plan_measure.json"))
    ap.add_argument("--owned", help="所持カメラJSON(未指定ならスマホから読む)")
    ap.add_argument("--base", help="雛形にする計画ファイル名(未指定なら先頭)")
    ap.add_argument("--force-bands", action="store_true", help="4帯を強制的に有効にする")
    args = ap.parse_args()

    names = [n.strip() for n in adb("shell", "ls", PLAN_DIR).split() if n.strip()]
    if not names:
        raise SystemExit("スマホに計画が無い")
    pick = args.base if args.base else names[0]
    if pick not in names:
        raise SystemExit("雛形が見つからない: %s (候補 %s)" % (pick, names))
    base = json.loads(adb("shell", "cat", "%s/%s" % (PLAN_DIR, pick)))
    print("土台にした計画: %s (%s)" % (pick, base.get("name")))

    # --owned を渡せばスマホを触らずに手元のファイルを使う(実機の撮影を邪魔しない)
    if args.owned:
        owned = json.loads(io.open(args.owned, encoding="utf-8").read())
    else:
        owned = json.loads(adb("shell", "cat", OWNED))
    cams = [o["camera"] for o in owned]
    if len(cams) < args.subs + 1:
        raise SystemExit("所持カメラが足りない(%d台、必要%d台)" % (len(cams), args.subs + 1))

    now = int(time.time())
    base["name"] = "PanoMeasure%d" % (args.subs + 1)
    base["start"] = dt(now + args.lead)
    base["end"] = dt(now + args.lead + args.minutes * 60)
    base["interval"] = args.interval
    base["panorama"] = True
    # 【帯の有効/無効は計画のまま使う(2026-08-25)】当初はどの時刻でも窓に入るよう
    #  4帯すべてを強制ONにしていたが、そうすると窓が1つも作られないことがあった
    #  (診断ログ "no active window: windows=0" で判明)。実機で撮れている計画の
    #  設定をそのまま使うのが確実。
    if args.force_bands:
        for k in ("useNight", "useSunrise", "useSunset", "useDay"):
            base.setdefault("ccm", {})[k] = True
    base["camera"] = cams[0]
    base["subCameras"] = cams[1:1 + args.subs]
    # 窓はエッジ側の buildSchedule が start/end から組み直す
    base["ccmList"] = []
    base["boundaries"] = []
    base["events"] = []

    io.open(args.out, "w", encoding="utf-8").write(json.dumps(base, ensure_ascii=False))
    print("主カメラ  : %s (%s)" % (cams[0]["name"], cams[0]["serial"]))
    for c in base["subCameras"]:
        print("追加カメラ: %s (%s)" % (c["name"], c["serial"]))
    print("開始 %s / 終了 %s / 周期 %.1f秒"
          % (time.strftime("%H:%M:%S", time.localtime(now + args.lead)),
             time.strftime("%H:%M:%S", time.localtime(now + args.lead + args.minutes * 60)),
             args.interval))
    print("書き出し: %s (%d バイト)" % (args.out, os.path.getsize(args.out)))


if __name__ == "__main__":
    main()
