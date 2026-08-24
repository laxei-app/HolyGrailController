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

    # 【DTR/RTS を触らずに開く】ESP32-S3 の USB-CDC は DTR/RTS の変化でリセットする。
    # 普通に open すると吸い出すたびにエッジが再起動してしまう(2026-08-25 実測)。
    sp = serial.Serial()
    sp.port = args.com
    sp.baudrate = 115200
    sp.timeout = 0.5
    sp.dtr = False
    sp.rts = False
    sp.open()
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
