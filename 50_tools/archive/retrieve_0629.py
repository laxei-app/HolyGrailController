import serial, time, sys, re

s = serial.Serial(); s.port='COM4'; s.baudrate=115200; s.timeout=0.5; s.dtr=False; s.rts=False
s.open()

def drain(sec):
    end=time.time()+sec; buf=b''
    while time.time()<end:
        n=s.in_waiting
        if n: buf+=s.read(n)
        else: time.sleep(0.05)
    return buf

# allow boot (open resets CoreS3)
boot = drain(6.0)
s.reset_input_buffer()

# list logs
s.write(b'G'); glist = drain(3.0)
print("=== [G] log list ===")
print(glist.decode('utf-8','replace'))

# read the 2026-06-29 capture log
s.write(b'Rhg_2026-06-29.log\n')
out = drain(25.0)
s.close()

txt = out.decode('utf-8','replace')
m = re.search(r'\[LOG\] (\S+) \((\d+) bytes\)\n', txt)
if m:
    path=m.group(1); nbytes=int(m.group(2))
    body_start=m.end()
    body_end=txt.find('[LOG] end', body_start)
    body = txt[body_start:body_end] if body_end>=0 else txt[body_start:]
    fn="Z:/projects/cameraControl/projects/HolyGrailController/HolyGrailController/_retrieved_logs/hg_2026-06-29.log"
    with open(fn,'w',encoding='utf-8',newline='') as f: f.write(body)
    print(f"SAVED {fn}  header_bytes={nbytes}  body_chars={len(body)}  lines={body.count(chr(10))}")
else:
    print("NO [LOG] header. tail:")
    print(txt[-800:])
