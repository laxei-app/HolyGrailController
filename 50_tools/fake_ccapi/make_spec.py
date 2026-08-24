#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""所持カメラを N 台へ増やし、偽カメラの仕様書(spec.json)を書き出す。

なぜ要るか
----------
偽カメラをエッジに掴ませるには、**撮影計画のカメラと一致**しないといけない。
一致の判定は dataManager::cameraModelMatches(機種名) と、計画にシリアルがある
場合はシリアル一致(holyGrailEntity.cpp:1155)。
だから偽カメラ側を「スマホの所持カメラの控え」に合わせて作るのが確実。

やること
--------
1. スマホの ownedCameras.json を読む(既存の実カメラはそのまま活かす)
2. 機材マスタから別機種を足して N 台にする(シリアルはこの道具が振る)
3. ownedCameras.json を書き戻す(--push でスマホへ)
4. spec.json を書き出す(fake_ccapi.py --spec がこれを読んで N 台を装う)

使い方
------
  python make_spec.py --count 8            # spec.json だけ作る(確認用)
  python make_spec.py --count 8 --push     # スマホの所持カメラも書き換える
"""
import argparse
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MASTER = os.path.join(REPO, "30_refer", "10_機材マスター", "camera_body_list.json")
ADB = os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe")
PHONE_OWNED = "/sdcard/Android/data/app.laxei.holygrail/files/asset/ownedCameras.json"

# この道具が足したカメラだと後から分かるようにする(片付けに使う)
FAKE_SERIAL_PREFIX = "9900"


def adb(*args, binary=False):
    r = subprocess.run([ADB] + list(args), capture_output=True)
    if r.returncode != 0:
        raise SystemExit("adb 失敗: %s\n%s" % (args, r.stderr.decode("utf-8", "ignore")))
    return r.stdout if binary else r.stdout.decode("utf-8", "ignore")


def load_master():
    j = json.load(io.open(MASTER, encoding="utf-8-sig"))
    if isinstance(j, dict):
        for k in j:
            if isinstance(j[k], list):
                j = j[k]
                break
    return j


def ss_list_default():
    """機材マスタは ss を持たないことがあるので既定を用意する(CCAPI 生表記ではなく内部表記)。"""
    fast = ["1/4000", "1/3200", "1/2500", "1/2000", "1/1600", "1/1250", "1/1000",
            "1/800", "1/640", "1/500", "1/400", "1/320", "1/250", "1/200",
            "1/160", "1/125", "1/100", "1/80", "1/60", "1/50", "1/40", "1/30",
            "1/25", "1/20", "1/15", "1/13", "1/10", "1/8", "1/6", "1/5", "1/4"]
    slow = ["0.3", "0.4", "0.5", "0.6", "0.8", "1", "1.3", "1.6", "2", "2.5",
            "3.2", "4", "5", "6", "8", "10", "13", "15", "20", "25", "30"]
    return fast + slow


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=8, help="所持カメラを何台にするか")
    ap.add_argument("--push", action="store_true", help="スマホへ書き戻す")
    ap.add_argument("--restore", action="store_true",
                    help="この道具が足したカメラを取り除いて書き戻す")
    args = ap.parse_args()

    print("スマホから所持カメラを読み出し中...")
    raw = adb("shell", "cat", PHONE_OWNED)
    owned = json.loads(raw.strip())
    print("  現在 %d 台: %s" % (len(owned), ", ".join(o["camera"]["name"] for o in owned)))

    if args.restore:
        keep = [o for o in owned
                if not o["camera"].get("serial", "").startswith(FAKE_SERIAL_PREFIX)]
        print("  片付け後 %d 台" % len(keep))
        owned = keep
    else:
        master = load_master()
        have = set(o["camera"]["name"] for o in owned)
        # 既存に無い EOS 機を、センサーの大きい順ではなく一覧順に足す
        cand = [m for m in master
                if (m.get("name") or "").startswith("EOS") and m.get("name") not in have]
        n = 1
        while len(owned) < args.count and cand:
            m = cand.pop(0)
            iso = [str(x) for x in m.get("iso", [])] or ["100", "200", "400", "800",
                                                         "1600", "3200", "6400", "12800"]
            owned.append({
                "autoInsert": False,
                "camera": {
                    "assignedName": "FAKE%02d" % n,
                    "authPass": "", "authUser": "",
                    "isoList": iso,
                    "maker": m.get("manufacture", "Canon"),
                    "meterLv": False,
                    "model": m["name"],
                    "name": m["name"],
                    "sensorPixel": int(m.get("pixel_w", 6000)),
                    "sensorSize": float(m.get("sensor_w", 22.3)),
                    "sensorSizeV": float(m.get("sensor_h", 15.0)),
                    "serial": "%s%08d" % (FAKE_SERIAL_PREFIX, n),
                    "ssList": ss_list_default(),
                },
                "lensList": [],
            })
            print("  追加: %-16s serial=%s" % (m["name"], owned[-1]["camera"]["serial"]))
            n += 1
        if len(owned) < args.count:
            print("  ※ 機材マスタに足せる機種が尽きた(%d 台どまり)" % len(owned))

    out = os.path.join(HERE, "ownedCameras.json")
    io.open(out, "w", encoding="utf-8").write(json.dumps(owned, ensure_ascii=False, indent=1))
    print("所持カメラを書き出した: %s (%d 台)" % (out, len(owned)))

    # ---- 偽カメラの仕様書 ----
    spec = []
    for o in owned:
        c = o["camera"]
        spec.append({
            "model": ("%s %s" % (c.get("maker", "Canon"), c["model"])).strip(),
            "serial": c.get("serial", ""),
            "nickname": c.get("assignedName", "") or c["name"],
            "isoList": c.get("isoList", []),
            "ssList": c.get("ssList", []),
        })
    sp = os.path.join(HERE, "spec.json")
    io.open(sp, "w", encoding="utf-8").write(json.dumps(spec, ensure_ascii=False, indent=1))
    print("偽カメラ仕様書を書き出した: %s (%d 台)" % (sp, len(spec)))

    if args.push:
        adb("push", out, PHONE_OWNED)
        print("スマホへ書き戻した。アプリを再起動して読み直させます...")
        adb("shell", "am", "force-stop", "app.laxei.holygrail")
        adb("shell", "am", "start", "-n", "app.laxei.holygrail/.MainActivity")
        print("完了。")
    else:
        print("(--push を付けるとスマホへ書き戻します)")


if __name__ == "__main__":
    main()
