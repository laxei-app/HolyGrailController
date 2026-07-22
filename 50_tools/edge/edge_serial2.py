import serial, time, sys

s = serial.Serial()
s.port = 'COM4'
s.baudrate = 115200
s.timeout = 0.3
s.dtr = False
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

# USB-Serial-JTAG リセット(EN=RTS)で再起動し、起動時の [osfile] 診断を捕まえる。
time.sleep(0.5)
s.reset_input_buffer()
print("=== reset & boot diagnostics ===")
s.setDTR(False)   # IO0 = high (通常起動)
s.setRTS(True)    # EN  = low  (リセット)
time.sleep(0.15)
s.setRTS(False)   # EN  = high (起動)
drain(6.0)        # 起動ログ([osfile] using SD / failed ...)

print("\n=== [F] backend ===")
s.write(b'F'); drain(1.5)
print("\n=== [i] info ===")
s.write(b'i'); drain(1.5)

s.close()
print("\n=== done ===")
