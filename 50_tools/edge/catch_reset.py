# -*- coding: utf-8 -*-
# リセットの瞬間を捕まえる。ブート要因(rst:0x..)・パニック(Guru/Backtrace)・
# 直前の数行を丸ごと出す。接続でリセットさせないよう dtr/rts=False。
#   使い方: python catch_reset.py COM4 [秒]
import serial, time, sys
sys.stdout.reconfigure(encoding='utf-8')

port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0
s.dtr = False; s.rts = False
s.open()

buf = b''
end = time.time() + dur
print(f"--- {port} を {dur:.0f}秒 監視 ---", flush=True)
while time.time() < end:
    n = s.in_waiting
    if n:
        buf += s.read(n)
    else:
        time.sleep(0.02)
s.close()

text = buf.decode('utf-8', 'replace')
lines = text.splitlines()
print(f"[受信 {len(buf)}バイト / {len(lines)}行]")
MARK = ("ESP-ROM", "rst:0x", "Guru Meditation", "Backtrace", "abort()", "assert",
        "Rebooting", "Panic", "panic", "StoreProhibited", "LoadProhibited",
        "InstrFetchProhibited", "Cache disabled", "CORRUPT HEAP", "Brownout",
        "brownout", "watchdog", "WDT", "boot:", "invalid header")
hits = [i for i, l in enumerate(lines) if any(m in l for m in MARK)]
if hits:
    print(f"★リセット/異常のマーカー {len(hits)}件")
    shown = set()
    for i in hits:
        for j in range(max(0, i-6), min(len(lines), i+10)):
            if j in shown: continue
            shown.add(j)
            print(("  >> " if j == i else "     ") + lines[j])
        print("     " + "-"*50)
else:
    print("マーカーなし。末尾30行:")
    for l in lines[-30:]:
        print("     " + l)
