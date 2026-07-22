# -*- coding: utf-8 -*-
# 両エッジ(COM4=Edje00 / COM7=Stick01)のシリアルを同時に常時監視。
# 接続でリセットしないよう dtr/rts=False で開く。各行に壁時計を付けてログファイルへ。
# リセット/パニックのマーカーを検出したら [!!RESET] 行も併記する。
# 停止: 同ディレクトリに mon_stop.flag を作るか、MAX_SEC 経過。
import serial, time, os, sys

BASE = os.path.dirname(os.path.abspath(__file__))
STOP = os.path.join(BASE, "mon_stop.flag")
MAX_SEC = 21600
PORTS = {'COM4':'Edje00', 'COM7':'Stick01'}

MARKERS = ["ESP-ROM:", "rst:0x", "boot:0x", "load:0x", "Guru Meditation",
           "Backtrace:", "Rebooting", "abort() was called", "assert failed",
           "Panic", "panic", "invalid header", "CORRUPT HEAP", "StoreProhibited",
           "LoadProhibited", "InstrFetchProhibited", "Cache disabled",
           "register dump", "PC      :", "brownout", "Brownout"]

def openp(port):
    s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0
    s.dtr=False; s.rts=False
    s.open()
    return s

ser = {}
logf = {}
for p in PORTS:
    try:
        ser[p] = openp(p)
        logf[p] = open(os.path.join(BASE, f"edge_mon_{p}.log"), "a", encoding="utf-8", errors="replace")
        logf[p].write(f"\n########## monitor start {time.strftime('%Y-%m-%d %H:%M:%S')} port={p} ({PORTS[p]}) ##########\n")
        logf[p].flush()
    except Exception as e:
        print(f"[{p}] open failed: {e}")

buf = {p:b'' for p in ser}
t0 = time.time()
try:
    while True:
        if os.path.exists(STOP): break
        if time.time()-t0 > MAX_SEC: break
        any_data = False
        for p,s in ser.items():
            try:
                n = s.in_waiting
            except Exception as e:
                logf[p].write(f"{time.strftime('%H:%M:%S')} [!!PORT-ERR] {e}\n"); logf[p].flush(); continue
            if n:
                any_data = True
                buf[p] += s.read(n)
                while b'\n' in buf[p]:
                    line, buf[p] = buf[p].split(b'\n', 1)
                    text = line.decode('utf-8','replace').rstrip('\r')
                    ts = time.strftime('%H:%M:%S')
                    logf[p].write(f"{ts} {text}\n")
                    low = text
                    if any(m in low for m in MARKERS):
                        logf[p].write(f"{ts} [!!RESET-MARKER] ^^^\n")
                    logf[p].flush()
        if not any_data:
            time.sleep(0.03)
finally:
    for p,s in ser.items():
        try: s.close()
        except: pass
    for p,f in logf.items():
        f.write(f"########## monitor stop {time.strftime('%Y-%m-%d %H:%M:%S')} ##########\n"); f.close()
print("monitor ended")
