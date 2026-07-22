# カメラが返すヒストグラムの bin 数を実機に聞く。
#   alzMetering() の binCon = size/256 が 1 より大きいと、bin まとめループの
#   代入(=上書き)バグで binCon-1 個ぶんの画素が捨てられる。まず実数を確認する。
import sys, json, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.4"
BASE = "http://%s:8080/ccapi" % HOST


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.status, r.read()


def post(path, obj, timeout=10):
    req = urllib.request.Request(BASE + path, data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read()


def main():
    try:
        st, b = get("/")
        print("ccapi root: HTTP", st)
    except Exception as e:
        print("到達できません:", e); return

    # ライブビュー開始(アプリと同じ small)。cameradisplay は機種差があるので順に試す。
    for disp in ("keep", "on", "off"):
        try:
            st, b = post("/ver100/shooting/liveview", {"liveviewsize": "small", "cameradisplay": disp})
            print("liveview start (cameradisplay=%s): HTTP %s" % (disp, st)); break
        except Exception as e:
            print("  cameradisplay=%s 不可: %s" % (disp, e))

    try:
        st, b = get("/ver100/shooting/liveview/flipdetail?kind=info", timeout=15)
    except Exception as e:
        print("flipdetail 取得失敗:", e); return

    # 応答は先頭にバイナリヘッダが付く形式(アプリは begin()+7 / end()-2 で切っている)
    s = b.decode("utf-8", "replace")
    i = s.find('{"liveviewdata"')
    if i < 0:
        i = s.find("{")
    j = s.rfind("}")
    try:
        d = json.loads(s[i:j + 1])
    except Exception as e:
        print("JSON parse 失敗:", e)
        print("先頭256B:", repr(s[:256])); return

    lv = d.get("liveviewdata", d)
    h = lv.get("histogram")
    if h is None:
        print("histogram が無い。キー:", list(lv.keys())); return

    print("\n=== histogram ===")
    print("チャンネル数:", len(h))
    for k, ch in enumerate(h):
        print("  [%d] bin数 = %d   合計 = %d" % (k, len(ch), sum(ch)))
    n = len(h[0])
    print("\nbinCon = %d / 256 = %s" % (n, n / 256.0))
    if n == 256:
        print("→ binCon=1。bin まとめループは1回しか回らないので上書きバグは顕在化しない。")
    else:
        print("→ binCon=%d。**上書きバグが顕在化し、画素の %d/%d が捨てられている**。"
              % (n // 256, (n // 256) - 1, n // 256))

    y = h[0]
    tot = sum(y)
    if tot:
        cum = 0
        med = 0
        for idx, c in enumerate(y):
            cum += c
            if cum >= tot / 2.0:
                med = idx; break
        print("\nY中央値 bin = %d (%.4f)" % (med, med / float(n - 1)))
        # 上書きバグを再現: 各グループの最後の sub-bin だけを残した場合の中央値
        bc = n // 256
        if bc > 1:
            dec = [y[b * bc + bc - 1] for b in range(256)]
            t2 = sum(dec); cum = 0; med2 = 0
            for idx, c in enumerate(dec):
                cum += c
                if cum >= t2 / 2.0:
                    med2 = idx; break
            print("バグ再現(最後のsub-binのみ)の中央値 bin = %d (%.4f)" % (med2, med2 / 255.0))


if __name__ == "__main__":
    main()
