# 測光露出を追い込むのに何秒かかるか実測する。
#   (a) 8" から 1/4" まで1段ずつ下げる場合の合計時間
#   (b) 8" から 1/4" へ一気に飛ばす場合の時間
# 各段で「設定PUT」と「その設定を反映した新しいLVフレームが得られるまで」を分けて計る。
#   反映の判定: systemtime が進んだフレームを取り、値が2回連続で一致したら安定とみなす。
import sys, json, time, http.client

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
PORT = 8080
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail"
CHAIN = ["8\"", "4\"", "2\"", "1\"", "0\"5", "1/4"]


def req(m, p, b=None, timeout=20):
    c = http.client.HTTPConnection(IP, PORT, timeout=timeout)
    try:
        d = None; h = {}
        if b is not None:
            d = json.dumps(b).encode(); h["Content-Type"] = "application/json"
        c.request(m, p, body=d, headers=h)
        r = c.getresponse(); return r.status, r.read()
    finally:
        c.close()


def median_of(bins):
    tot = sum(bins)
    if tot <= 0:
        return None
    half = tot / 2.0; cum = 0.0; n = len(bins)
    for k in range(n):
        b = bins[k]
        if cum + b >= half:
            return (k + ((half - cum) / b if b > 0 else 0.0)) / (n - 1)
        cum += b
    return 1.0


def lv():
    try:
        st, raw = req("GET", FLIP + "?kind=info", timeout=10)
    except Exception:
        return None
    if st != 200 or len(raw) < 10 or raw[0] != 0xFF:
        return None
    try:
        d = json.loads(raw[7:-2].decode("utf-8", "replace"))["liveviewdata"]
        s = d.get("systemtime", {})
        return (int(s.get("sec", 0)) * 1000 + int(s.get("subsec", 0)), median_of(d["histogram"][0]))
    except Exception:
        return None


def set_tv(v):
    t0 = time.time()
    st, _ = req("PUT", "/ccapi/ver100/shooting/settings/tv", {"value": v})
    return (time.time() - t0), st in (200, 201, 204)


def wait_settled(budget=25.0):
    """新しいフレームを採り、値が2回連続一致したら安定とみなす。(所要秒, 値, 読んだ回数)"""
    f = lv()
    base = f[0] if f else 0
    t0 = time.time()
    seen = set(); prev = None; reads = 0
    while time.time() - t0 < budget:
        f = lv()
        if f is None:
            continue
        ms, m = f
        if ms <= base or ms in seen or m is None:
            time.sleep(0.05); continue
        seen.add(ms); reads += 1
        if prev is not None and abs(m - prev) < 0.002:
            return (time.time() - t0), m, reads
        prev = m
    return (time.time() - t0), prev, reads


def main():
    for disp in ("keep", "on", "off"):
        st, _ = req("POST", "/ccapi/ver100/shooting/liveview",
                    {"liveviewsize": "small", "cameradisplay": disp})
        if st in (200, 201, 204):
            break
    dev = json.loads(req("GET", "/ccapi/ver100/deviceinformation")[1].decode())
    print("=== %s (%s) ===\n" % (dev.get("productname"), IP))

    # (a) 1段ずつ
    set_tv(CHAIN[0]); wait_settled()
    print("(a) 8\" から 1段ずつ 1/4\" まで")
    print("%-8s %10s %10s %8s %10s" % ("Tv", "PUT", "反映待ち", "読み回数", "LV値"))
    print("-" * 50)
    total = 0.0
    for v in CHAIN[1:]:
        dput, ok = set_tv(v)
        if not ok:
            print("%-8s 設定失敗" % v); continue
        dw, m, reads = wait_settled()
        total += dput + dw
        print("%-8s %9.2fs %9.2fs %8d %10s"
              % (v, dput, dw, reads, ("%.4f" % m) if m is not None else "--"))
    print("-" * 50)
    print("合計: %.2f 秒\n" % total)

    # (b) 一気に飛ばす
    set_tv(CHAIN[0]); wait_settled()
    print("(b) 8\" から 1/4\" へ一気に")
    dput, ok = set_tv(CHAIN[-1])
    dw, m, reads = wait_settled()
    print("   PUT %.2fs + 反映待ち %.2fs = %.2f 秒 (読み %d回, LV=%.4f)"
          % (dput, dw, dput + dw, reads, m if m is not None else -1))

    # 参考: 8"へ戻す時間
    print("\n(参考) 1/4\" → 8\" へ戻す")
    dput, ok = set_tv("8\"")
    dw, m, reads = wait_settled()
    print("   PUT %.2fs + 反映待ち %.2fs = %.2f 秒" % (dput, dw, dput + dw))


if __name__ == "__main__":
    main()
