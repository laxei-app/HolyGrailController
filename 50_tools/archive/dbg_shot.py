# シャッター要求の応答と、新ファイルが出来るまでを詳しく見る(R100の撮影失敗調査)
import sys, json, time, http.client

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.4"
PORT = 8080


def req(m, p, b=None, t=90):
    c = http.client.HTTPConnection(IP, PORT, timeout=t)
    try:
        d = None; h = {}
        if b is not None:
            d = json.dumps(b).encode(); h["Content-Type"] = "application/json"
        c.request(m, p, body=d, headers=h)
        r = c.getresponse(); return r.status, r.read()
    finally:
        c.close()


def newest():
    st, raw = req("GET", "/ccapi/ver130/contents")
    s = json.loads(raw.decode()).get("path", [])
    if not s: return None
    st, raw = req("GET", s[-1])
    f = json.loads(raw.decode()).get("path", [])
    if not f: return None
    fo = f[-1]
    st, raw = req("GET", fo + "?kind=number")
    n = json.loads(raw.decode()).get("contentsnumber", 0)
    print("   最終フォルダ %s  枚数=%d" % (fo, n))
    if n <= 0: return None
    st, raw = req("GET", fo + "?kind=list&page=%d" % ((n + 99) // 100))
    fl = json.loads(raw.decode()).get("path", [])
    return fl[-1] if fl else None


print("撮影前:")
b = newest()
print("   最新 =", b)

print("\nshutterbutton POST ...")
t0 = time.time()
st, raw = req("POST", "/ccapi/ver100/shooting/control/shutterbutton", {"af": False})
print("   HTTP %s  %.1f秒  body=%r" % (st, time.time() - t0, raw[:200]))

for k in range(15):
    time.sleep(2)
    c = newest()
    if c != b:
        print("\n新ファイル: %s  (%d秒後)" % (c, (k + 1) * 2))
        break
    print("   待ち %d秒: 変化なし" % ((k + 1) * 2))
else:
    print("\n新ファイルが出来ませんでした")
    st, raw = req("GET", "/ccapi/ver100/shooting/settings/tv")
    print("   現在のTv:", raw[:80])
