# -*- coding: utf-8 -*-
# 撮影中でも 'l' の [LOG]...[LOG] end ブロックだけを確実に拾う(リセット無し)。
import serial, time, sys, io
PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM7'
OUT  = sys.argv[2] if len(sys.argv) > 2 else 'stick01_now.log'
s = serial.Serial(); s.port=PORT; s.baudrate=115200; s.timeout=0.2; s.dtr=False; s.rts=False
s.open()
time.sleep(0.3); s.reset_input_buffer()
buf = b''
started = False
deadline = time.time() + 35.0
last_send = 0.0
while time.time() < deadline:
    # 3秒ごとに 'l' を送り直す(ETP出力に埋もれても再要求)
    if not started and time.time() - last_send > 3.0:
        s.write(b'l'); last_send = time.time()
    n = s.in_waiting
    if n:
        buf += s.read(n)
        if b'[LOG]' in buf and not started:
            started = True
        if started and b'[LOG] end' in buf:
            break
    else:
        time.sleep(0.03)
s.close()
txt = buf.decode('utf-8','replace')
with io.open(OUT, 'w', encoding='utf-8') as f:
    f.write(txt)
# [LOG] ... [LOG] end の中身だけ抜き出して行数報告
i = txt.find('[LOG]')
j = txt.find('[LOG] end')
body = txt[i:j] if (i>=0 and j>=0) else txt
print("saved %d bytes -> %s ; SHOT lines=%d" % (len(txt), OUT, body.count('|SHOT')))
