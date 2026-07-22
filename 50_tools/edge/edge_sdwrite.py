import serial, time, sys
s = serial.Serial(); s.port='COM4'; s.baudrate=115200; s.timeout=0.3; s.dtr=False; s.rts=False; s.open()
def drain(sec):
    end=time.time()+sec; buf=b''
    while time.time()<end:
        n=s.in_waiting
        if n: buf+=s.read(n)
        else: time.sleep(0.05)
    sys.stdout.write(buf.decode('utf-8','replace')); sys.stdout.flush()
time.sleep(0.3); s.reset_input_buffer()
print("=== [s] start capture ==="); s.write(b's'); drain(30.0)
print("\n=== [x] stop ==="); s.write(b'x'); drain(4.0)
print("\n=== [l] dump today's log (from SD) ==="); s.write(b'l'); drain(6.0)
s.close(); print("\n=== done ===")
