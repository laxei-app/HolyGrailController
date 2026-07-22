# 測光シャッターへ切り替える「本当の追加コスト」を測る。
#
#  実装で実際に必要なのは:
#     PUT(測光用Tv) → それが反映されたLVコマを1枚読む → PUT(撮影用Tv) → シャッター
#  つまり増えるのは「PUT 2回」と「Tv変更が反映されるまでの待ち」だけ。
#  戻り側(撮影用Tvに戻す)はLVを見ないので待ち不要。
#
#  ここでは PUT 直後から新しいコマを連続で拾い、
#     ・1枚目が来るまで何秒か
#     ・何枚目で値が落ち着くか(1枚で足りるのか)
#  を出す。
import sys, json, time, http.client

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
SHOOT_TV = sys.argv[2] if len(sys.argv) > 2 else "8\""
METER_TV = sys.argv[3] if len(sys.argv) > 3 else "1/4"
PORT = 8080
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail"


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
    t0 = time.time()
    try:
        st, raw = req("GET", FLIP + "?kind=info", timeout=12)
    except Exception:
        return None
    if st != 200 or len(raw) < 10 or raw[0] != 0xFF:
        return None
    try:
        d = json.loads(raw[7:-2].decode("utf-8", "replace"))["liveviewdata"]
        s = d.get("systemtime", {})
        ms = int(s.get("sec", 0)) * 1000 + int(s.get("subsec", 0))
        return ms, median_of(d["histogram"][0]), (time.time() - t0)
    except Exception:
        return None


def put_tv(v):
    t0 = time.time()
    st, _ = req("PUT", "/ccapi/ver100/shooting/settings/tv", {"value": v})
    return time.time() - t0, st in (200, 201, 204)


def main():
    for disp in ("keep", "on", "off"):
        st, _ = req("POST", "/ccapi/ver100/shooting/liveview",
                    {"liveviewsize": "small", "cameradisplay": disp})
        if st in (200, 201, 204):
            break
    dev = json.loads(req("GET", "/ccapi/ver100/deviceinformation")[1].decode())
    print("=== %s  撮影Tv=%s → 測光Tv=%s ===\n" % (dev.get("productname"), SHOOT_TV, METER_TV))

    for trial in range(3):
        # 撮影用に戻して落ち着かせる
        put_tv(SHOOT_TV)
        time.sleep(3.0)
        f = lv()
        base_ms = f[0] if f else 0
        base_val = f[1] if f else None

        print("--- 試行%d (切替前 LV=%.4f) ---" % (trial + 1, base_val if base_val else -1))
        dput, ok = put_tv(METER_TV)
        t0 = time.time()
        seen = set(); got = 0
        while got < 5 and time.time() - t0 < 12:
            f = lv()
            if f is None:
                continue
            ms, m, dget = f
            if ms <= base_ms or ms in seen:
                continue
            seen.add(ms); got += 1
            print("   %d枚目: PUTから %.2fs  LV=%.4f  (GET自体 %.2fs)"
                  % (got, time.time() - t0, m, dget))
        dback, _ = put_tv(SHOOT_TV)
        print("   PUT(測光用) %.3fs / PUT(撮影用に戻す) %.3fs  ※戻りは待ち不要\n" % (dput, dback))


if __name__ == "__main__":
    main()
