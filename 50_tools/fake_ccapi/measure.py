#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""台数を変えてエッジの内部RAMを測る。

やること
--------
1. エッジのシリアルを記録し始める(RAM/heap の行を拾う)
2. 偽カメラを N 台立てる
3. 主カメラ1 + 追加カメラ(N-1) のパノラマ計画を ETP で送って開始
4. 一定時間まわして、内部RAM の推移(free / largest / min)を拾う
5. 停止して片付け、要約を出す

内部RAM の見方([[edge-internal-ram-limits]] と同じ):
  free    = 内部RAMの空き合計
  largest = 連続で取れる最大(断片化の度合い。スレッド生成はここが効く)
  min     = 起動してからの空きの最小値(水位。ここが薄いと一時的に危なかった)
"""
import argparse
import io
import json
import os
import re
import subprocess
import sys
import threading
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import etp_client as etp  # noqa: E402


class SerialTap(threading.Thread):
    """エッジのシリアルを読み続けてファイルへ落とす。"""
    daemon = True

    def __init__(self, port, path):
        super().__init__(name="tap")
        self.port = port
        self.path = path
        self.stop_flag = threading.Event()
        self.lines = []

    def run(self):
        try:
            # DTR/RTS を触らずに開く(触るとエッジがリセットする)
            sp = serial.Serial()
            sp.port = self.port
            sp.baudrate = 115200
            sp.timeout = 0.5
            sp.dtr = False
            sp.rts = False
            sp.open()
        except Exception as e:
            print("シリアルを開けない(%s): %s" % (self.port, e))
            return
        buf = b""
        with io.open(self.path, "w", encoding="utf-8", errors="replace") as f:
            while not self.stop_flag.is_set():
                try:
                    b = sp.read(4096)
                except Exception:
                    break
                if not b:
                    continue
                buf += b
                while b"\n" in buf:
                    ln, buf = buf.split(b"\n", 1)
                    t = ln.decode("utf-8", "replace").rstrip("\r")
                    f.write(t + "\n")
                    f.flush()
                    self.lines.append(t)
        sp.close()


RAM_RE = re.compile(r"free=(\d+).*?min=(\d+)")
HEAP_RE = re.compile(r"free=(\d+) largest=(\d+) min=(\d+)")


def summarize(lines):
    """RAM/heap 行から free/largest/min の推移を拾う。"""
    pts = []
    for t in lines:
        m = HEAP_RE.search(t)
        if m:
            pts.append(("heap", t.strip(), int(m.group(1)), int(m.group(2)), int(m.group(3))))
            continue
        if "|RAM" in t or "RAM " in t:
            m = RAM_RE.search(t)
            if m:
                pts.append(("ram", t.strip(), int(m.group(1)), None, int(m.group(2))))
    return pts


def _stale_fakes():
    """まだ動いている fake_ccapi.py の pid を拾う。"""
    r = subprocess.run(["wmic", "process", "where",
                        "name='python.exe'", "get", "ProcessId,CommandLine"],
                       capture_output=True)
    out = r.stdout.decode("utf-8", "ignore")
    pids = []
    for ln in out.splitlines():
        if "fake_ccapi.py" in ln:
            parts = ln.split()
            if parts and parts[-1].isdigit():
                pids.append(int(parts[-1]))
    return pids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cams", type=int, required=True, help="使うカメラ台数(主1+追加N-1)")
    ap.add_argument("--edge", default="192.168.1.10")
    ap.add_argument("--com", default="COM4")
    ap.add_argument("--host", default="192.168.1.4")
    ap.add_argument("--minutes", type=int, default=6, help="回す長さ[分]")
    ap.add_argument("--id", default=None)
    ap.add_argument("--ssdp-gap", type=float, default=0.035)
    ap.add_argument("--one-st", action="store_true")
    args = ap.parse_args()

    plan_id = args.id or ("meas%02d" % args.cams)
    log = os.path.join(HERE, "measure_%02d.log" % args.cams)
    fakelog = os.path.join(HERE, "fake_%02d.log" % args.cams)

    # 前回の偽カメラが残っているとポートを掴んだままになり、新しい方が bind に失敗して
    # 「応答はするのに統計が取れない」状態になる(2026-08-25 実測)。確実に落とす。
    subprocess.run(["taskkill", "/F", "/IM", "python.exe", "/FI",
                    "WINDOWTITLE eq fake_ccapi*"], capture_output=True)
    for pid in _stale_fakes():
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        print("残っていた偽カメラ pid=%d を落とした" % pid)

    print("=== %d 台で測定 ===" % args.cams)

    # 【測定のたびにエッジを再起動する(2026-08-26)】水位(min)は起動してからの最小値なので、
    #  前の測定の値を引き継いでしまい台数ごとの比較にならない。ここだけは意図的に
    #  DTR/RTS を触ってリセットする(通常の監視では触らないこと)。
    try:
        rs = serial.Serial(args.com, 115200, timeout=0.5)
        rs.dtr = False; rs.rts = True; time.sleep(0.2); rs.rts = False
        t0 = time.time()
        while time.time() - t0 < 20:
            rs.read(4096)
        rs.close()
        print("エッジを再起動して水位をリセットしました")
        time.sleep(12)
    except Exception as ex:
        print("再起動できませんでした(そのまま続行): %s" % ex)

    tap = SerialTap(args.com, log)
    tap.start()
    time.sleep(2)

    # 偽カメラ
    fk = io.open(fakelog, "w", encoding="utf-8")
    fake = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "fake_ccapi.py"),
         "--count", str(args.cams), "--host", args.host,
         "--ssdp-gap", str(args.ssdp_gap)]
        + (["--ssdp-one-st"] if args.one_st else [])
        + ["--spec", os.path.join(HERE, "spec.json")],
        stdout=fk, stderr=subprocess.STDOUT, cwd=HERE)
    print("偽カメラ %d 台を起動 (pid %d)" % (args.cams, fake.pid))
    time.sleep(4)

    # 計画
    subprocess.run([sys.executable, os.path.join(HERE, "make_plan.py"),
                    "--subs", str(args.cams - 1), "--lead", "40",
                    "--owned", os.path.join(HERE, "ownedCameras.json"),
                    "--base", "plan_20260823-145453.json",
                    "--minutes", str(max(args.minutes + 5, 150))], cwd=HERE, check=True)

    try:
        e = etp.Edge(args.edge, timeout=30.0)
        m, _ = e.xchg(etp.C_TIME, etp.M_PUT,
                      json.dumps({"datetime": etp.now_iso(), "utcOffsetMin": 540}))
        print("C_TIME ->", "ACK" if m == etp.M_ACK else m)
        plan = io.open(os.path.join(HERE, "plan_measure.json"), encoding="utf-8").read()
        m, _ = e.xchg(etp.C_CAPTURE_PLAN, etp.M_PUT, plan_id + "\t" + plan)
        print("C_CAPTURE_PLAN ->", "ACK" if m == etp.M_ACK else m)
        m, _ = e.xchg(etp.C_ACTION, etp.M_POST, plan_id)
        print("C_ACTION ->", "ACK" if m == etp.M_ACK else m)
        e.close()
    except Exception as ex:
        print("ETP でこけた: %s" % ex)

    print("%d 分まわします..." % args.minutes)
    t0 = time.time()
    while time.time() - t0 < args.minutes * 60:
        time.sleep(15)
        pts = summarize(tap.lines)
        if pts:
            k, t, f, lg, mn = pts[-1]
            print("  %4ds  free=%-7d largest=%-7s min=%-7d"
                  % (int(time.time() - t0), f, lg if lg else "-", mn))

    # 片付け
    print("停止します...")
    try:
        e = etp.Edge(args.edge, timeout=30.0)
        e.xchg(etp.C_STOP, etp.M_POST, plan_id)
        e.xchg(etp.C_DELETE_PLAN, etp.M_DELETE, plan_id)
        e.close()
    except Exception as ex:
        print("停止でこけた(あとで手当て): %s" % ex)

    fake.terminate()
    try:
        fake.wait(timeout=10)
    except Exception:
        fake.kill()
    fk.close()
    time.sleep(2)
    tap.stop_flag.set()
    tap.join(timeout=5)

    # エッジ本体のログを吸い出す。RAM/NET/HEAP は logEvent 経由なのでシリアルには流れず、
    # 't' コマンドでログファイルの末尾を取る必要がある(2026-08-25 実測で判明)。
    elog = os.path.join(HERE, "edgelog_%02d.txt" % args.cams)
    print("エッジのログを吸い出します...")
    r = subprocess.run([sys.executable, os.path.join(HERE, "dumplog.py"),
                        "--com", args.com, "--out", elog],
                       capture_output=True, cwd=HERE)
    if r.returncode != 0:
        print("  取れなかった: %s" % r.stderr.decode("utf-8", "ignore")[:200])
        elines = []
    else:
        elines = io.open(elog, encoding="utf-8", errors="replace").read().splitlines()

    # 要約
    pts = summarize(elines) or summarize(tap.lines)
    print("\n=== %d 台 の内部RAM ===" % args.cams)
    if not pts:
        print("(RAM/heap の行が採れなかった)")
    else:
        frees = [p[2] for p in pts]
        mins = [p[4] for p in pts]
        larg = [p[3] for p in pts if p[3] is not None]
        print("free   : 最大 %d / 最小 %d" % (max(frees), min(frees)))
        if larg:
            print("largest: 最大 %d / 最小 %d" % (max(larg), min(larg)))
        print("min(水位): %d" % min(mins))
        print("--- 節目 ---")
        for k, t, f, lg, mn in pts[:20]:
            print("  " + t[:150])
    shots = [t for t in elines if "|SHOT" in t]
    print("撮影コマ数(SHOTログ): %d" % len(shots))
    st = [t for t in elines if "CAPTURING" in t or "session stop" in t]
    for t in st[:6]:
        print("  " + t[:150])
    print("ログ: %s / %s" % (log, fakelog))


if __name__ == "__main__":
    main()
