import serial, time, sys, re

s = serial.Serial(); s.port='COM4'; s.baudrate=115200; s.timeout=0.3; s.dtr=False; s.rts=False
s.open()

def drain(sec):
    end=time.time()+sec; buf=b''
    while time.time()<end:
        n=s.in_waiting
        if n: buf+=s.read(n)
        else: time.sleep(0.05)
    return buf

time.sleep(0.3); s.reset_input_buffer()
out=b''
s.write(b'F'); out+=drain(1.5)
s.write(b'i'); out+=drain(1.5)
s.write(b'l'); out+=drain(12.0)
s.close()

txt=out.decode('utf-8','replace')
# header/footer (avoid dumping the whole body to stdout)
for line in txt.splitlines():
    if line.startswith('[FS]') or line.startswith('[INFO]') or line.startswith('[LOG]'):
        print(line[:200])

m=re.search(r'\[LOG\] (\S+) \((\d+) bytes\)\n', txt)
if m:
    path=m.group(1); nbytes=int(m.group(2))
    body_start=m.end()
    body_end=txt.find('[LOG] end', body_start)
    body = txt[body_start:body_end] if body_end>=0 else txt[body_start:]
    date=path.split('/')[-1].replace('.log','')
    fn=f"Z:/projects/cameraControl/projects/HolyGrailController/HolyGrailController/_retrieved_logs/{date}_retrieved.log"
    with open(fn,'w',encoding='utf-8') as f: f.write(body)
    print(f"SAVED {fn}  header_bytes={nbytes} body_chars={len(body)}")
else:
    print("NO [LOG] header found. Raw tail:")
    print(txt[-600:])
