# flipdetail をリトライ付きで取得し、ヒストグラムの bin 数と中央値を出す。
import sys, json, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
BASE = "http://%s:8080/ccapi" % HOST


def get(p, t=15):
    with urllib.request.urlopen(BASE + p, timeout=t) as r:
        return r.status, r.read()


b = None
for attempt in range(8):
    try:
        st, b = get("/ver100/shooting/liveview/flipdetail?kind=info")
        print("attempt %d: HTTP %s  len=%d" % (attempt, st, len(b)))
        break
    except Exception as e:
        print("attempt %d: %s" % (attempt, e))
        time.sleep(2)
if b is None:
    sys.exit("flipdetail を取得できませんでした")

s = b.decode("utf-8", "replace")
i = s.find('{"liveviewdata"')
if i < 0:
    i = s.find("{")
j = s.rfind("}")
d = json.loads(s[i:j + 1])
lv = d.get("liveviewdata", d)
print("liveviewdata keys:", list(lv.keys()))

h = lv.get("histogram")
if h is None:
    sys.exit("histogram が応答に含まれません")

print("チャンネル数:", len(h))
for k, ch in enumerate(h):
    print("  [%d] bins=%d  sum=%d" % (k, len(ch), sum(ch)))

n = len(h[0])
bc = n // 256
print("\nbin数=%d  →  binCon = %d" % (n, bc))
if bc <= 1:
    print("→ binCon=1。bin まとめループは1回で終わるので、上書きバグは顕在化しない。")
else:
    print("→ binCon=%d。**上書きバグが顕在化。各グループの最後の sub-bin しか残らず、"
          "画素の %d/%d が捨てられている**。" % (bc, bc - 1, bc))

y = h[0]
tot = float(sum(y))


def median(arr):
    t = float(sum(arr))
    if t <= 0:
        return -1
    c = 0.0
    for idx, v in enumerate(arr):
        c += v
        if c >= t / 2.0:
            return idx
    return len(arr) - 1


m = median(y)
print("\n正しい Y中央値: bin %d / %d = %.4f" % (m, n - 1, m / float(n - 1)))
if bc > 1:
    dec = [y[q * bc + bc - 1] for q in range(256)]
    m2 = median(dec)
    print("バグ再現   Y中央値: bin %d / 255 = %.4f  (捨てた分を除いた分布)" % (m2, m2 / 255.0))
    # 正しくまとめた場合(合算)
    summed = [sum(y[q * bc:(q + 1) * bc]) for q in range(256)]
    m3 = median(summed)
    print("正しくまとめた場合: bin %d / 255 = %.4f" % (m3, m3 / 255.0))
