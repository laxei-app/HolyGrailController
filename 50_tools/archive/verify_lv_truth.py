# R10 のライブビューは本当に間違っているのか — 実写を正解として検証する。
#
# 手順(各シャッター設定ごと):
#   1. Tv を設定
#   2. ライブビューを読む。**systemtime が進んだ新しいフレームだけ**を採り、複数フレームの中央値を取る
#      (前回の「頭打ち」は古いフレームを読んだ疑いがあったので、ここを厳密にする)
#   3. 同じ設定で実写を1枚撮り、カメラ内の表示用JPEGを取得して輝度中央値を出す
#   4. もう一度ライブビューを読む(シーンが動いていないかの検証。薄明中は空が変化するため)
#
# 比較はガンマを外して段数に直して行う。露出を1段増やしたとき
#   実写もライブビューも +1段  → ライブビューは正しい
#   実写は +1段 なのにライブビューは 0段 → **そこから先はライブビューが間違っている**
import sys, io, json, time, http.client
from PIL import Image

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
PORT = 8080
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail"
TVS = ["1/60", "1/15", "1/4", "1\"", "2\"", "4\"", "8\""]


def req(method, path, body=None, timeout=25):
    c = http.client.HTTPConnection(IP, PORT, timeout=timeout)
    try:
        data = None; hd = {}
        if body is not None:
            data = json.dumps(body).encode(); hd["Content-Type"] = "application/json"
        c.request(method, path, body=data, headers=hd)
        r = c.getresponse(); raw = r.read()
        return r.status, raw
    finally:
        c.close()


def hist_median(y):
    total = sum(y)
    if total <= 0:
        return None
    half = total / 2.0; cum = 0.0; n = len(y)
    for k in range(n):
        b = y[k]
        if cum + b >= half:
            frac = (half - cum) / b if b > 0 else 0.0
            return (k + frac) / (n - 1)
        cum += b
    return 1.0


def lv_frame():
    """(systemtime_ms, Y中央値) 取れなければ None"""
    try:
        st, raw = req("GET", FLIP + "?kind=info", timeout=10)
    except Exception:
        return None
    if st != 200 or len(raw) < 10 or raw[0] != 0xFF:
        return None
    try:
        lv = json.loads(raw[7:-2].decode("utf-8", "replace"))["liveviewdata"]
        m = hist_median(lv["histogram"][0])
        s = lv.get("systemtime", {})
        ms = int(s.get("sec", 0)) * 1000 + int(s.get("subsec", 0))
        return (ms, m) if m is not None else None
    except Exception:
        return None


def lv_settled(min_frames=5, budget=15.0):
    """設定後の“新しい”フレームだけを集め、その中央値を返す。(値, 採用フレーム数)"""
    f = lv_frame()
    base = f[0] if f else 0
    seen, vals = set(), []
    t0 = time.time()
    while time.time() - t0 < budget and len(vals) < min_frames:
        f = lv_frame()
        if f is None:
            time.sleep(0.3); continue
        ms, m = f
        if ms <= base or ms in seen:
            time.sleep(0.3); continue
        seen.add(ms); vals.append(m)
    if not vals:
        return None, 0
    vals.sort()
    return vals[len(vals) // 2], len(vals)


def newest_file():
    st, raw = req("GET", "/ccapi/ver130/contents")
    stor = json.loads(raw.decode()).get("path", [])
    if not stor:
        return None
    st, raw = req("GET", stor[-1])
    folders = json.loads(raw.decode()).get("path", [])
    if not folders:
        return None
    folder = folders[-1]
    st, raw = req("GET", folder + "?kind=number")
    n = json.loads(raw.decode()).get("contentsnumber", 0)
    if n <= 0:
        return None
    page = (n + 99) // 100
    st, raw = req("GET", folder + "?kind=list&page=%d" % page)
    files = json.loads(raw.decode()).get("path", [])
    return files[-1] if files else None


def shoot_still(wait_s):
    before = newest_file()
    st, raw = req("POST", "/ccapi/ver100/shooting/control/shutterbutton",
                  {"af": False}, timeout=max(30, wait_s + 25))
    if st not in (200, 201, 202, 203, 204):
        return None, "shutter st=%d" % st
    time.sleep(2.0)
    url = None
    for _ in range(10):
        cur = newest_file()
        if cur and cur != before:
            url = cur; break
        time.sleep(0.8)
    if not url:
        return None, "新しいファイルが出来ない"
    st, img = req("GET", url + "?kind=display", timeout=20)
    if st != 200 or len(img) < 1000:
        return None, "download st=%d" % st
    im = Image.open(io.BytesIO(img)).convert("L")
    px = sorted(im.getdata())
    return px[len(px) // 2] / 255.0, url.split("/")[-1]


def srgb_to_linear(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def tv_sec(s):
    s = s.strip()
    if s.startswith("1/"):
        return 1.0 / float(s[2:])
    return float(s.replace('"', "").replace("\\", ""))


import math


def main():
    for disp in ("keep", "on", "off"):
        st, _ = req("POST", "/ccapi/ver100/shooting/liveview",
                    {"liveviewsize": "small", "cameradisplay": disp})
        if st in (200, 201, 204):
            break

    st, raw = req("GET", "/ccapi/ver100/shooting/settings/tv")
    orig = json.loads(raw.decode())["value"]
    st, raw = req("GET", "/ccapi/ver100/shooting/settings/av")
    av = json.loads(raw.decode())["value"]
    st, raw = req("GET", "/ccapi/ver100/shooting/settings/iso")
    iso = json.loads(raw.decode())["value"]
    print("R10  F%s  ISO%s   元のTv=%s" % (av, iso, orig))
    print("実写を正解として、ライブビューがどこまで一致するかを見る\n")

    rows = []
    for tv in TVS:
        st, raw = req("PUT", "/ccapi/ver100/shooting/settings/tv", {"value": tv})
        if st not in (200, 201, 204):
            print("%-6s 設定失敗 st=%d" % (tv, st)); continue
        pre, n1 = lv_settled()
        sec = tv_sec(tv)
        still, info = shoot_still(sec)
        post, n2 = lv_settled()
        drift = abs((post or 0) - (pre or 0))
        rows.append((tv, sec, pre, still, post, drift))
        print("%-6s LV=%.4f(%dframe)  実写=%s  LV再測=%.4f  ぶれ=%.4f  %s"
              % (tv, pre or -1, n1,
                 ("%.4f" % still) if still is not None else "失敗(%s)" % info,
                 post or -1, drift, "" if still is not None else ""))

    req("PUT", "/ccapi/ver100/shooting/settings/tv", {"value": orig})
    print("\n元のTv(%s)へ戻しました。\n" % orig)

    good = [r for r in rows if r[2] is not None and r[3] is not None]
    if len(good) < 2:
        print("比較できるデータが足りません"); return

    print("=== 1段ぶん露出を増やしたとき、実際に何段ぶん明るくなったか ===")
    print("%-14s %10s %10s %10s" % ("区間", "露出変化", "実写", "ライブビュー"))
    print("-" * 50)
    for k in range(1, len(good)):
        ptv, psec, plv, pst, _, _ = good[k - 1]
        ctv, csec, clv, cst, _, _ = good[k]
        d_set = math.log2(csec / psec)
        d_still = math.log2(srgb_to_linear(cst) / srgb_to_linear(pst)) if srgb_to_linear(pst) > 0 else float("nan")
        d_lv = math.log2(srgb_to_linear(clv) / srgb_to_linear(plv)) if srgb_to_linear(plv) > 0 else float("nan")
        mark = ""
        if abs(d_still) > 0.5 and abs(d_lv) < 0.25 * abs(d_still):
            mark = "  ← ライブビューが追従していない"
        print("%-14s %+9.2f段 %+9.2f段 %+9.2f段%s"
              % ("%s→%s" % (ptv, ctv), d_set, d_still, d_lv, mark))

    print("\n※実写とライブビューが同じだけ動いていれば、ライブビューは正しい。")
    print("  実写だけ動いてライブビューが止まる区間があれば、そこから先が信用できない。")
    print("※実写の中央値が 1.0 に近い場合は白飛びで比較不能(その行は無視)。")


if __name__ == "__main__":
    main()
