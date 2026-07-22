# -*- coding: utf-8 -*-
# 両エッジに 'i'(info) を1回だけ投げて状態を見る。接続でリセットしないよう dtr/rts=False。
import serial, time, sys

def query(port):
    s = serial.Serial(); s.port=port; s.baudrate=115200; s.timeout=0.2
    s.dtr=False; s.rts=False
    try:
        s.open()
    except Exception as e:
        return f"[{port}] OPEN FAILED: {e}"
    def drain(sec):
        end=time.time()+sec; buf=b''
        while time.time()<end:
            n=s.in_waiting
            if n: buf+=s.read(n)
            else: time.sleep(0.03)
        return buf.decode('utf-8','replace')
    time.sleep(0.3); s.reset_input_buffer()
    out = f"===== {port} =====\n"
    s.write(b'i'); out += "[i]\n" + drain(2.5) + "\n"
    s.close()
    return out

for p in ['COM4','COM7']:
    print(query(p))
