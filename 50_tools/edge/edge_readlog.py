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
print("=== [F] backend ==="); s.write(b'F'); drain(1.5)
print("\n=== [l] today's log (SD) ==="); s.write(b'l'); drain(8.0)
s.close(); print("\n=== done ===")
