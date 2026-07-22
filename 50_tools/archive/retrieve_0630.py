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
boot = drain(6.0); s.reset_input_buffer()
s.write(b'i'); print("=== [i] ==="); print(drain(1.5).decode('utf-8','replace'))
s.write(b'Rhg_2026-06-30.log\n')
out = drain(20.0); s.close()
txt = out.decode('utf-8','replace')
m = re.search(r'\[LOG\] (\S+) \((\d+) bytes\)\n', txt)
if m:
    body_start=m.end(); body_end=txt.find('[LOG] end', body_start)
    body = txt[body_start:body_end] if body_end>=0 else txt[body_start:]
    fn="Z:/projects/cameraControl/projects/HolyGrailController/HolyGrailController/_retrieved_logs/hg_2026-06-30.log"
    open(fn,'w',encoding='utf-8',newline='').write(body)
    print(f"SAVED lines={body.count(chr(10))}")
    print("=== TAIL (last 40 lines) ===")
    print("\n".join(body.splitlines()[-40:]))
else:
    print("no log header; tail:"); print(txt[-800:])
