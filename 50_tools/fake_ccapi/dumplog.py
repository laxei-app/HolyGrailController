#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""エッジの当日ログの末尾をシリアルの 't' コマンドで吸い出す。

RAM/NET の記録は logEvent 経由でログファイルへ行くのでシリアルには流れない。
測定のたびにここから取る。
"""
import argparse
import io
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--com", default="COM4")
    ap.add_argument("--out", default=None)
    ap.add_argument("--wait", type=float, default=25.0)
    args = ap.parse_args()

    sp = serial.Serial(args.com, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
    time.sleep(1.0)
    sp.reset_input_buffer()
    sp.write(b"t")
    buf = b""
    t0 = time.time()
    last = time.time()
    while time.time() - t0 < args.wait:
        b = sp.read(8192)
        if b:
            buf += b
            last = time.time()
        elif time.time() - last > 3.0 and buf:
            break
    sp.close()

    text = buf.decode("utf-8", "replace")
    if args.out:
        io.open(args.out, "w", encoding="utf-8").write(text)
        print("書き出し: %s (%d 文字)" % (args.out, len(text)))
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
