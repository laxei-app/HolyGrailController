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
# USB-Serial-JTAG リセット(EN=RTS)で再起動 → ensureInit を再判定させる
print("=== reset & boot ===")
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)
drain(6.0)
print("\n=== [F] backend ==="); s.write(b'F'); drain(2.0)
print("\n=== [i] info ==="); s.write(b'i'); drain(1.5)
s.close(); print("\n=== done ===")
