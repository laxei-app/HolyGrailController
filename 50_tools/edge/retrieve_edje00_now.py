# -*- coding: utf-8 -*-
# Edje00(CoreS3, COM4) の当日SDログを、撮影を止めずに('l'コマンド, リセット無し)読み出す。
import serial, time, sys, io
PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
OUT  = sys.argv[2] if len(sys.argv) > 2 else 'edje00_now.log'
s = serial.Serial(); s.port=PORT; s.baudrate=115200; s.timeout=0.3; s.dtr=False; s.rts=False
s.open()
def drain(sec):
    end=time.time()+sec; buf=b''
    while time.time()<end:
        n=s.in_waiting
        if n: buf+=s.read(n); end=time.time()+2.0   # データが来る限り延長
        else: time.sleep(0.05)
    return buf
time.sleep(0.3); s.reset_input_buffer()
s.write(b'l')
data = drain(20.0)
s.close()
txt = data.decode('utf-8','replace')
with io.open(OUT, 'w', encoding='utf-8') as f:
    f.write(txt)
lines = txt.splitlines()
print("captured %d lines -> %s" % (len(lines), OUT))
