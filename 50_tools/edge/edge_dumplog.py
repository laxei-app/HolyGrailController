# -*- coding: utf-8 -*-
# エッジの当日ログをシリアル経由で吸い出す。
#  使い方: python edge_dumplog.py COM7 [出力ファイル]
#  ※ mon_edges.py がポートを掴んでいると開けない。先に mon_stop.flag で止めること。
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
out  = sys.argv[2] if len(sys.argv) > 2 else None

s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.3
s.dtr = False; s.rts = False          # 接続でリセットさせない
s.open()

def drain(sec):
    end = time.time() + sec; buf = b''
    while time.time() < end:
        n = s.in_waiting
        if n:
            buf += s.read(n)
            end = time.time() + 2.0   # 受信が続く限り延長
        else:
            time.sleep(0.05)
    return buf.decode('utf-8', 'replace')

time.sleep(0.3); s.reset_input_buffer()
s.write(b'l')                          # 当日ログ出力
text = drain(20.0)
s.close()

if out:
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"wrote {out} ({len(text)} chars)")
else:
    sys.stdout.reconfigure(encoding='utf-8')
    print(text)
