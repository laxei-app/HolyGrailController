import serial, time, sys

s = serial.Serial()
s.port = 'COM4'
s.baudrate = 115200
s.timeout = 0.3
s.dtr = False   # CoreS3 が接続でリセットしないように
s.rts = False
s.open()

def drain(sec):
    end = time.time() + sec
    buf = b''
    while time.time() < end:
        n = s.in_waiting
        if n:
            buf += s.read(n)
        else:
            time.sleep(0.05)
    sys.stdout.write(buf.decode('utf-8', 'replace'))
    sys.stdout.flush()

time.sleep(1.0)
s.reset_input_buffer()

print("\n=== [i] info ===")
s.write(b'i'); drain(1.5)

print("\n=== [F] backend (SD/LittleFS) ===")
s.write(b'F'); drain(1.5)

print("\n=== [D] clear internal LittleFS logs ===")
s.write(b'D'); drain(3.0)

print("\n=== [F] backend again ===")
s.write(b'F'); drain(1.5)

s.close()
print("\n=== done ===")
