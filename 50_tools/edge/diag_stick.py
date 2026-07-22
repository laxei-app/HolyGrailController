# -*- coding: utf-8 -*-
import serial, time, sys
PORT = sys.argv[1] if len(sys.argv)>1 else 'COM7'
s = serial.Serial(); s.port=PORT; s.baudrate=115200; s.timeout=0.2; s.dtr=False; s.rts=False
s.open()
def drain(sec):
    end=time.time()+sec; buf=b''
    while time.time()<end:
        n=s.in_waiting
        if n: buf+=s.read(n)
        else: time.sleep(0.03)
    return buf.decode('utf-8','replace')
time.sleep(0.3); s.reset_input_buffer()
print("--- send i ---"); s.write(b'i'); print(repr(drain(3.0)))
print("--- send G (log file names) ---"); s.write(b'G'); print(repr(drain(3.0)))
print("--- send l ---"); s.write(b'l'); print(repr(drain(8.0)))
s.close()
