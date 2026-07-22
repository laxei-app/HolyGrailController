# -*- coding: utf-8 -*-
# サブネットを CCAPI(8080) で総当りしてカメラを特定する。
# SSDPはスマホ直結後に止まることがあるので、HTTPで直接叩くこちらを正とする。
# 以前 PowerShell で 254本を同時に開いて700msだけ待つ実装にしたら .7 のカメラを取りこぼした。
# → ここではスレッドプール+ホスト毎に明示タイムアウトで確実に判定する。
import socket, json, sys
from concurrent.futures import ThreadPoolExecutor
import urllib.request

SUBNET = sys.argv[1] if len(sys.argv) > 1 else "192.168.1"
PORT = 8080

def probe(i):
    ip = f"{SUBNET}.{i}"
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(1.5)
    try:
        if s.connect_ex((ip, PORT)) != 0:
            return None
    finally:
        s.close()
    # 8080が開いていた → CCAPI か確認し、機種/シリアルを取る
    info = {"ip": ip, "ccapi": None, "model": None, "serial": None}
    try:
        with urllib.request.urlopen(f"http://{ip}:{PORT}/ccapi", timeout=4) as r:
            info["ccapi"] = r.status
    except Exception as e:
        info["ccapi"] = f"NG({e})"
        return info
    try:
        with urllib.request.urlopen(f"http://{ip}:{PORT}/ccapi/ver100/deviceinformation", timeout=4) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
            info["model"] = d.get("productname")
            info["serial"] = d.get("serialnumber")
    except Exception as e:
        info["model"] = f"NG({e})"
    return info

with ThreadPoolExecutor(max_workers=64) as ex:
    for res in ex.map(probe, range(1, 255)):
        if res:
            print(f"{res['ip']:<15} ccapi={res['ccapi']} model={res['model']} serial={res['serial']}")
print("done")
