# -*- coding: utf-8 -*-
# 17:28問題の観測: 17:05〜18:00 の間、1秒ごとに各機器の応答を記録する。
#  - PR-S300NE(192.168.1.1): ICMP ping
#  - カメラ(自動発見, port8080): TCP接続プローブ + 30秒ごとにHTTP GET /ccapi (keepalive=自動電源OFF抑止)
#  - スマホ(発見できれば): ICMP ping
# 出力: netwatch_YYYYMMDD.csv (time,target,ok,ms)
import socket, subprocess, time, datetime as dt, urllib.request, json, os, concurrent.futures as cf

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "netwatch_%s.csv" % dt.datetime.now().strftime("%Y%m%d"))
START = dt.datetime.now().replace(hour=17, minute=5, second=0, microsecond=0)
END   = dt.datetime.now().replace(hour=18, minute=0, second=0, microsecond=0)

def log(line):
    with open(OUT, "a", encoding="utf-8") as f:
        f.write(line + "\n")

def icmp(ip, timeout_ms=800):
    t0 = time.time()
    r = subprocess.run(["ping", "-n", "1", "-w", str(timeout_ms), ip],
                       capture_output=True, timeout=5)
    ok = (r.returncode == 0) and (b"TTL=" in r.stdout or b"ttl=" in r.stdout)
    return ok, int((time.time() - t0) * 1000)

def tcp(ip, port=8080, timeout=0.8):
    t0 = time.time()
    s = socket.socket()
    s.settimeout(timeout)
    try:
        s.connect((ip, port)); return True, int((time.time() - t0) * 1000)
    except Exception:
        return False, int((time.time() - t0) * 1000)
    finally:
        s.close()

def find_cameras():
    hits = {}
    def chk(i):
        ip = "192.168.1.%d" % i
        ok, _ = tcp(ip, 8080, 0.6)
        if not ok: return
        try:
            with urllib.request.urlopen("http://%s:8080/ccapi/ver100/deviceinformation" % ip, timeout=3) as f:
                d = json.loads(f.read().decode())
                hits[ip] = d.get("productname", "camera")
        except Exception:
            hits[ip] = "port8080"
    with cf.ThreadPoolExecutor(60) as ex:
        list(ex.map(chk, range(2, 255)))
    return hits

log("# start %s" % dt.datetime.now().isoformat())
# 17:05 まで待つ(既に過ぎていれば即開始)。カメラの自動電源OFF抑止は観測窓の間だけ行う。
while dt.datetime.now() < START:
    time.sleep(20)

cams = find_cameras()
log("# cameras: %s" % json.dumps(cams, ensure_ascii=False))
last_keep = 0.0
while dt.datetime.now() < END:
    now = dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
    ok, ms = icmp("192.168.1.1")
    log("%s,HGW_192.168.1.1,%d,%d" % (now, 1 if ok else 0, ms))
    for ip in list(cams):
        ok, ms = tcp(ip)
        log("%s,CAM_%s,%d,%d" % (now, ip, 1 if ok else 0, ms))
    # 30秒ごとにHTTP keepalive(カメラを寝かせない)
    if time.time() - last_keep > 30:
        last_keep = time.time()
        for ip in list(cams):
            try:
                urllib.request.urlopen("http://%s:8080/ccapi" % ip, timeout=3).read(64)
                log("%s,KEEP_%s,1,0" % (now, ip))
            except Exception:
                log("%s,KEEP_%s,0,0" % (now, ip))
        if not cams:      # カメラ未発見なら30秒おきに再探索
            cams = find_cameras()
            if cams: log("# cameras(late): %s" % json.dumps(cams, ensure_ascii=False))
    time.sleep(1.0)
log("# end %s" % dt.datetime.now().isoformat())
