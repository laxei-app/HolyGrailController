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
    args = ap.parse_args()

    names = [n.strip() for n in adb("shell", "ls", PLAN_DIR).split() if n.strip()]
    if not names:
        raise SystemExit("スマホに計画が無い")
    base = json.loads(adb("shell", "cat", "%s/%s" % (PLAN_DIR, names[0])))
    print("土台にした計画: %s (%s)" % (names[0], base.get("name")))

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
    # 測定はいつ走らせても撮影窓に入るよう、4帯すべてを有効にする。
    #  既定の計画は朝日/夕日が「使わない」で、日の出直後だと窓が1つも作られず
    #  WAITING のまま撮影に入らない(2026-08-25 実測)。
    for k in ("useNight", "useSunrise", "useSunset", "useDay"):
        base.setdefault("ccm", {})[k] = True
    base["camera"] = cams[0]
    base["subCameras"] = cams[1:1 + args.subs]
    # 窓はエッジ側の buildSchedule が組み直すので空でよい
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
