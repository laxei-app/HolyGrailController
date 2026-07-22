# 明るさ合わせ用: f1.4/ISO1600/8秒 で1枚撮り、実写の明るさが検証に適した範囲かを判定する。
#  狙い: 実写の中央値 0.35〜0.65(=夜空を夜間設定で撮ったときと同じくらい)。
#  そこに入れば、ライブビューと実写を暗所で正しく比較できる。
import sys, io, json, time, http.client
from PIL import Image

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
PORT = 8080


def req(m, p, b=None, t=60):
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
    if n <= 0: return None
    st, raw = req("GET", fo + "?kind=list&page=%d" % ((n + 99) // 100))
    fl = json.loads(raw.decode()).get("path", [])
    return fl[-1] if fl else None


for k, v in (("iso", "1600"), ("av", "f1.4"), ("tv", "8\"")):
    req("PUT", "/ccapi/ver100/shooting/settings/" + k, {"value": v})

before = newest()
print("f1.4 / ISO1600 / 8秒 で1枚撮影中...")
req("POST", "/ccapi/ver100/shooting/control/shutterbutton", {"af": False}, t=60)
time.sleep(2)
url = None
deadline = time.time() + 40      # R100はPOSTが即返るので露光8秒+書込を待つ
while time.time() < deadline:
    c = newest()                 # 露光/書込中は None が返ることがあるので無視して待つ
    if c and c != before:
        url = c; break
    time.sleep(1.0)
if not url:
    sys.exit("撮影ファイルが見つかりません")
st, img = req("GET", url + "?kind=display", t=30)
im = Image.open(io.BytesIO(img)).convert("L")
px = sorted(im.getdata())
med = px[len(px) // 2] / 255.0
print("実写の明るさ(中央値) = %.4f" % med)

if med < 0.20:
    print("→ **暗すぎます**。もう少し明るくしてください。")
elif med > 0.75:
    print("→ **明るすぎます**。少し暗くしてください。")
else:
    print("→ **ちょうど良い範囲です。** この明るさで検証できます。")
