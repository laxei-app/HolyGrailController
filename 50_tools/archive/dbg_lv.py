# lv() が何を返しているか素で見る
import sys, json, time, http.client

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
PORT = 8080
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail"


def req(m, p, b=None, t=20):
    c = http.client.HTTPConnection(IP, PORT, timeout=t)
    try:
        d = None; h = {}
        if b is not None:
            d = json.dumps(b).encode(); h["Content-Type"] = "application/json"
        c.request(m, p, body=d, headers=h)
        r = c.getresponse(); return r.status, r.read()
    finally:
        c.close()


for i in range(8):
    t0 = time.time()
    try:
        st, raw = req("GET", FLIP + "?kind=info", timeout=12)
    except Exception as e:
        print("%d: 例外 %s" % (i, e)); continue
    dt = time.time() - t0
    head = raw[:8]
    ok0 = (len(raw) >= 10 and raw[0] == 0xFF)
    info = ""
    if ok0:
        try:
            d = json.loads(raw[7:-2].decode("utf-8", "replace"))["liveviewdata"]
            s = d.get("systemtime", {})
            ms = int(s.get("sec", 0)) * 1000 + int(s.get("subsec", 0))
            hs = d.get("histogram")
            info = "systemtime=%d  hist=%s" % (ms, "有(%dch)" % len(hs) if hs else "無")
        except Exception as e:
            info = "parse失敗 %s" % e
    print("%d: HTTP%s %.2fs len=%d head=%r 0xFF先頭=%s  %s"
          % (i, st, dt, len(raw), head, ok0, info))
    time.sleep(0.4)
