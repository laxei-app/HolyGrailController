# -*- coding: utf-8 -*-
# エッジに残った旧形式(テキスト)の撮影レポートを削除する('R' コマンド)。
#
# 撮影レポートは 2026-08-05 に JSON へ移行した。旧 .txt はスマホの回収対象外
# (C_REPORT_* は .json のみ扱う)なのでエッジに残り続ける。その掃除用。
# 接続でリセットしないよう dtr/rts=False。
#
# 使い方: python edge_rmoldreports.py COM4 COM7
import serial, sys, time


def run(port):
    s = serial.Serial()
    s.port = port; s.baudrate = 115200; s.timeout = 0.2
    s.dtr = False; s.rts = False
    try:
        s.open()
    except Exception as e:
        return "[%s] OPEN FAILED: %s" % (port, e)

    def drain(sec):
        end = time.time() + sec
        buf = b""
        while time.time() < end:
            n = s.in_waiting
            if n:
                buf += s.read(n)
                end = time.time() + 1.5   # 出力が続く限り延長
            else:
                time.sleep(0.03)
        return buf.decode("utf-8", "replace")

    time.sleep(0.3); s.reset_input_buffer()
    s.write(b"R")
    out = "===== %s =====\n%s" % (port, drain(10.0))
    s.close()
    return out


for p in (sys.argv[1:] or ["COM4", "COM7"]):
    print(run(p))
