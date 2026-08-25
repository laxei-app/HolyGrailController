#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""エッジのシリアルを取り続ける(再起動/パニックの現行犯を押さえる)。

・DTR/RTS を触らずに開く。触ると ESP32-S3 の USB-CDC がリセットしてしまい、
  「見に行くと再起動する」ことになる(2026-08-25 実測)。
・リセット直後に出る Guru Meditation / Backtrace / rst:0x.. をそのまま残す。

  python monitor.py --com COM4 --out mon.txt --minutes 15
"""
import argparse
import io
import re
import sys
import time

import serial

RESET_HINTS = re.compile(
    r"Guru Meditation|Backtrace:|rst:0x|abort\(\)|assert failed|"
    r"Stack canary|StoreProhibited|LoadProhibited|InstrFetchProhibited|"
    r"task_wdt|Debug exception|CORRUPT HEAP|Heap corrup|E \(\d+\) ")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--com", default="COM4")
    ap.add_argument("--out", default="mon.txt")
    ap.add_argument("--minutes", type=float, default=15.0)
    args = ap.parse_args()

    sp = serial.Serial()
    sp.port = args.com
    sp.baudrate = 115200
    sp.timeout = 0.5
    sp.dtr = False          # ← 触らない(触るとエッジがリセットする)
    sp.rts = False
    sp.open()

    print("監視開始 %s -> %s (%.0f分)" % (args.com, args.out, args.minutes))
    t0 = time.time()
    buf = b""
    hits = 0
    with io.open(args.out, "w", encoding="utf-8", errors="replace") as f:
        while time.time() - t0 < args.minutes * 60:
            b = sp.read(4096)
            if not b:
                continue
            buf += b
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                t = ln.decode("utf-8", "replace").rstrip("\r")
                stamp = time.strftime("%H:%M:%S")
                f.write("%s %s\n" % (stamp, t))
                f.flush()
                if RESET_HINTS.search(t):
                    hits += 1
                    print("!! %s %s" % (stamp, t[:150]))
    sp.close()
    print("監視終了。手掛かり %d 行。%s" % (hits, args.out))


if __name__ == "__main__":
    main()
