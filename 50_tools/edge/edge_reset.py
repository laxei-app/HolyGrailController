import serial, time
s = serial.Serial(); s.port='COM4'; s.baudrate=115200; s.timeout=0.3; s.dtr=False; s.rts=False
s.open()
# EN(RTS)パルスでハードリセット=電源入れ直し相当
s.setDTR(False); s.setRTS(True); time.sleep(0.25); s.setRTS(False)
s.close()
print("edge reset pulse sent")
