# ライブビューは実写と一致するか — 管理された室内シーンでの検証(決定版)。
#
#  1. ISO100/Mに固定し、絞りを自動選択(8秒で白飛びせず、1秒付近が中間になるように)
#  2. 1/30〜8" を振り、各設定で
#       ライブビュー(新鮮なフレームのみ) → 実写1枚 → ライブビュー再測
#     を取る。前後のライブビューがズレた行は「シーンが動いた」として無効にする
#  3. ガンマを外して段数に直し、実写とライブビューの動きを並べる
#
#  判定: 実写と同じだけ動く=ライブビューは正しい / 実写だけ動く区間=そこから先がズレている
import sys, io, json, time, math, http.client
from PIL import Image

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
PORT = 8080
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail"
SWEEP = ["1/30", "1/15", "1/8", "1/4", "1/2", "1\"", "2\"", "4\"", "8\""]
AVS = ["f1.4", "f2.0", "f2.8", "f4.0", "f5.6", "f8.0", "f11", "f16"]


def req(method, path, body=None, timeout=30):
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


def getval(name):
    st, raw = req("GET", "/ccapi/ver100/shooting/settings/" + name)
    return json.loads(raw.decode()) if st == 200 else None


def setval(name, v):
    st, raw = req("PUT", "/ccapi/ver100/shooting/settings/" + name, {"value": v})
    if st in (200, 201, 204):
        return True
    # Tv は機種で表記が違う("1/2" と 0"5 など)。代替表記を試す。
    if name == "tv":
        alt = None
        if v == "1/2":
            alt = "0\"5"
        elif v == "1/3":
            alt = "0\"3"
        elif v == "1/4":
            alt = "0\"4"
        if alt:
            st, raw = req("PUT", "/ccapi/ver100/shooting/settings/tv", {"value": alt})
            return st in (200, 201, 204)
    return False


def median_of(bins):
    tot = sum(bins)
    if tot <= 0:
        return None
    half = tot / 2.0; cum = 0.0; n = len(bins)
    for k in range(n):
        b = bins[k]
        if cum + b >= half:
            frac = (half - cum) / b if b > 0 else 0.0
            return (k + frac) / (n - 1)
        cum += b
    return 1.0


def lv_frame():
    try:
        st, raw = req("GET", FLIP + "?kind=info", timeout=10)
    except Exception:
        return None
    if st != 200 or len(raw) < 10 or raw[0] != 0xFF:
        return None
    try:
        lv = json.loads(raw[7:-2].decode("utf-8", "replace"))["liveviewdata"]
        m = median_of(lv["histogram"][0])
        s = lv.get("systemtime", {})
        ms = int(s.get("sec", 0)) * 1000 + int(s.get("subsec", 0))
        return (ms, m) if m is not None else None
    except Exception:
        return None


def lv_settled(nframes=5, budget=20.0):
    """設定変更後の“新しい”フレームだけを集めて中央値を返す。"""
    f = lv_frame()
    base = f[0] if f else 0
    seen, vals = set(), []
    t0 = time.time()
    while time.time() - t0 < budget and len(vals) < nframes:
        f = lv_frame()
        if f is None:
            time.sleep(0.3); continue
        ms, m = f
        if ms <= base or ms in seen:
            time.sleep(0.25); continue
        seen.add(ms); vals.append(m)
    if not vals:
        return None
    vals.sort()
    return vals[len(vals) // 2]


def newest_file():
    st, raw = req("GET", "/ccapi/ver130/contents")
    stor = json.loads(raw.decode()).get("path", [])
    if not stor:
        return None
    st, raw = req("GET", stor[-1])
    fol = json.loads(raw.decode()).get("path", [])
    if not fol:
        return None
    folder = fol[-1]
    st, raw = req("GET", folder + "?kind=number")
    n = json.loads(raw.decode()).get("contentsnumber", 0)
    if n <= 0:
        return None
    st, raw = req("GET", folder + "?kind=list&page=%d" % ((n + 99) // 100))
    files = json.loads(raw.decode()).get("path", [])
    return files[-1] if files else None


def shoot_still(exp_s):
    before = newest_file()
    st, raw = req("POST", "/ccapi/ver100/shooting/control/shutterbutton",
                  {"af": False}, timeout=max(40, exp_s + 30))
    if st not in (200, 201, 202, 203, 204):
        return None
    # R10 は POST が露光完了まで待つが、R100 は即座に返る。露光時間+書き込み分を待つ。
    time.sleep(min(2.0, exp_s) )
    url = None
    deadline = time.time() + exp_s + 25.0
    while time.time() < deadline:
        cur = newest_file()          # 露光/書込中は None を返すことがあるので無視して待つ
        if cur and cur != before:
            url = cur; break
        time.sleep(1.0)
    if not url:
        return None
    st, img = req("GET", url + "?kind=display", timeout=25)
    if st != 200 or len(img) < 1000:
        return None
    im = Image.open(io.BytesIO(img)).convert("L")
    px = sorted(im.getdata())
    return px[len(px) // 2] / 255.0


def to_lin(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def tv_sec(s):
    s = s.strip()
    return 1.0 / float(s[2:]) if s.startswith("1/") else float(s.replace('"', ""))


def calibrate():
    """8秒で白飛びせず、1秒付近が中間になる絞りを選ぶ。"""
    print("絞りを選定中(ISO100固定)...")
    setval("iso", "100")
    avail = getval("av")
    cand = [a for a in AVS if a in (avail or {}).get("ability", [])] or [(avail or {}).get("value")]
    setval("tv", "1/4")
    # 1/4秒でのライブビュー値から 8秒(=5段上)の明るさを見積もり、中間(linear≈0.17)を狙う
    best = None
    for av in cand:
        if not setval("av", av):
            continue
        m = lv_settled(nframes=3, budget=10)
        if m is None:
            continue
        pred8 = to_lin(m) * 32.0     # 1/4"→8" は 5段
        print("  %-5s : LV(1/4\")=%.4f → 8\"の予測 linear=%.3f" % (av, m, pred8))
        if best is None or abs(math.log2(max(pred8, 1e-9) / 0.17)) < abs(math.log2(max(best[1], 1e-9) / 0.17)):
            best = (av, pred8)
    if best:
        setval("av", best[0])
        print("→ 絞り %s を採用\n" % best[0])
        return best[0]
    return (avail or {}).get("value")


def main():
    for disp in ("keep", "on", "off"):
        st, _ = req("POST", "/ccapi/ver100/shooting/liveview",
                    {"liveviewsize": "small", "cameradisplay": disp})
        if st in (200, 201, 204):
            break

    dev = json.loads(req("GET", "/ccapi/ver100/deviceinformation")[1].decode())
    mode = getval("shootingmodedial")
    print("=== %s (%s) ===" % (dev.get("productname"), IP))
    print("モード: %s" % (mode or {}).get("value"))
    orig_tv = (getval("tv") or {}).get("value")
    orig_av = (getval("av") or {}).get("value")
    orig_iso = (getval("iso") or {}).get("value")
    print("元の設定: %s %s ISO%s\n" % (orig_tv, orig_av, orig_iso))

    # 引数で絞り/ISOを指定されたら自動選定せずそれに固定する(夜間の実設定を再現する用)。
    if len(sys.argv) > 3:
        av, iso = sys.argv[2], sys.argv[3]
        setval("iso", iso); setval("av", av)
        print("固定設定で測定: %s ISO%s (自動選定なし)\n" % (av, iso))
    else:
        av = calibrate()

    print("%-6s %10s %10s %10s %8s" % ("Tv", "LV", "実写", "LV再測", "ぶれ"))
    print("-" * 50)
    rows = []
    for tv in SWEEP:
        if not setval("tv", tv):
            print("%-6s 設定失敗" % tv); continue
        pre = lv_settled()
        still = shoot_still(tv_sec(tv))
        post = lv_settled()
        drift = abs((post or 0) - (pre or 0)) if (pre is not None and post is not None) else 9.9
        ok = (pre is not None and still is not None and drift < 0.02)
        print("%-6s %10s %10s %10s %8.4f %s"
              % (tv,
                 "%.4f" % pre if pre is not None else "--",
                 "%.4f" % still if still is not None else "--",
                 "%.4f" % post if post is not None else "--",
                 drift,
                 "" if ok else "← 無効(シーンが動いた/取得失敗)"))
        if ok:
            rows.append((tv, tv_sec(tv), pre, still))

    setval("tv", orig_tv); setval("av", orig_av); setval("iso", orig_iso)
    print("\n元の設定へ戻しました。")

    if len(rows) < 3:
        print("有効データが足りません"); return

    print("\n=== 露出を増やしたとき、実際に何段ぶん明るくなったか ===")
    print("%-14s %9s %9s %9s" % ("区間", "露出変化", "実写", "ライブビュー"))
    print("-" * 48)
    verdict = []
    for k in range(1, len(rows)):
        ptv, ps, plv, pst = rows[k - 1]
        ctv, cs, clv, cst = rows[k]
        d_set = math.log2(cs / ps)
        lp, lc = to_lin(pst), to_lin(cst)
        vp, vc = to_lin(plv), to_lin(clv)
        d_still = math.log2(lc / lp) if lp > 0 else float("nan")
        d_lv = math.log2(vc / vp) if vp > 0 else float("nan")
        sat = cst > 0.97 or pst > 0.97
        note = ""
        if sat:
            note = "  (白飛びで比較不能)"
        elif abs(d_still) > 0.4 and abs(d_lv) < 0.3 * abs(d_still):
            note = "  ← ライブビューが追従していない"
            verdict.append(ptv)
        print("%-14s %+8.2f段 %+8.2f段 %+8.2f段%s"
              % ("%s→%s" % (ptv, ctv), d_set, d_still, d_lv, note))

    print()
    if verdict:
        print("結論: %s より長いシャッターで、ライブビューが実写に追従していません。" % verdict[0])
    else:
        print("結論: 全区間でライブビューは実写と同じだけ動いています(=ライブビューは正しい)。")


if __name__ == "__main__":
    main()
