# -*- coding: utf-8 -*-
# 両エッジに 'b'(電源情報) を投げて読み値を確認する。接続でリセットしないよう dtr/rts=False。
import serial, time, sys
sys.stdout.reconfigure(encoding='utf-8')

def query(port, label):
    s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.2
    s.dtr=False; s.rts=False
    try:
        s.open()
    except Exception as e:
        print(f"[{port}] OPEN FAILED: {e}"); return
    def drain(sec):
        end=time.time()+sec; buf=b''
        while time.time()<end:
            n=s.in_waiting
            if n:
                buf+=s.read(n); end=time.time()+0.8
            else:
                time.sleep(0.03)
        return buf.decode('utf-8','replace')
    time.sleep(0.4); s.reset_input_buffer()
    s.write(b'b')
    out = drain(6.0)
    s.close()
    print(f"===== {label} ({port}) =====")
    for ln in out.splitlines():
        if 'BATT' in ln or 'PM1' in ln or ln.strip().startswith(('00:','10:','20:','30:')):
            print("  " + ln.strip())
    print()

query('COM4', 'Edje00 / CoreS3')
query('COM7', 'Stick01 / StickS3')
