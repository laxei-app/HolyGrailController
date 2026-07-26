# -*- coding: utf-8 -*-
# 測光方式の実験ツール(PCからCCAPI直叩き)。
#  方式A: flipdetail?kind=info のヒストグラム(Y/R/G/B 4ch)からリニア輝度を算出
#  方式B: flipdetail?kind=image の小さいライブビューJPEGを復号してリニア輝度を算出
# それぞれ「できるか」と「何msかかるか」を測る。露出はオプションで固定できる(布かぶせ実験用)。
#
# 使い方:
#   python exp_meter.py <ip> hist  [N]                 # 方式A をN回
#   python exp_meter.py <ip> image [N]                 # 方式B をN回
#   python exp_meter.py <ip> both  [N]                 # 両方交互
#   python exp_meter.py <ip> ... --ss 1/50 --iso 100   # 露出を固定してから測る
#   python exp_meter.py <ip> ... --dump                # 初回の生JSON/生バイトを保存(構造確認)
import sys, json, time, io, urllib.request, urllib.error

def http(url, method="GET", body=None, timeout=8):
    req = urllib.request.Request(url, method=method)
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    t0 = time.perf_counter()
    with urllib.request.urlopen(req, data=data, timeout=timeout) as r:
        raw = r.read()
    return raw, (time.perf_counter() - t0) * 1000.0

def srgb_to_linear(x):
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

def hist_median(bins):
    total = sum(bins)
    if total <= 0: return 0.0
    half, cum = total / 2.0, 0.0
    for k, c in enumerate(bins):
        if cum + c >= half:
            frac = (half - cum) / c if c > 0 else 0.0
            return (k + frac) / (len(bins) - 1)
        cum += c
    return 1.0

class Cam:
    def __init__(self, ip):
        self.base = f"http://{ip}:8080"
        raw, ms = http(self.base + "/ccapi")
        j = json.loads(raw)
        # ファームと同じくサフィックス一致でURLを解決(ver100/ver110差を吸収)
        self.urls = {}
        for ver, apis in j.items():
            if not isinstance(apis, list): continue
            for a in apis:
                p = a.get("path", "")
                for sfx in ("/shooting/liveview", "/shooting/liveview/flipdetail",
                            "/shooting/settings/tv", "/shooting/settings/iso", "/shooting/settings/av"):
                    if p.endswith(sfx):
                        self.urls[sfx] = self.base + p
        print(f"connect ok ({ms:.0f}ms)  liveview={self.urls.get('/shooting/liveview','-')}")

    def start_lv(self):
        for disp in ("keep", "on", "off"):   # ファームと同じフォールバック順
            try:
                raw, ms = http(self.urls["/shooting/liveview"], "POST",
                               {"liveviewsize": "small", "cameradisplay": disp})
                print(f"liveview start ok (display={disp}, {ms:.0f}ms)")
                return
            except urllib.error.HTTPError as e:
                last = e
        raise last

    def set_exposure(self, ss=None, iso=None, fn=None):
        for sfx, val in (("/shooting/settings/tv", ss), ("/shooting/settings/iso", iso),
                         ("/shooting/settings/av", fn)):
            if val is None: continue
            raw, ms = http(self.urls[sfx], "PUT", {"value": val})
            print(f"set {sfx.split('/')[-1]}={val} ok ({ms:.0f}ms)")

    def flipdetail(self, kind):
        return http(self.urls["/shooting/liveview/flipdetail"] + f"?kind={kind}")

def parse_chunks(raw):
    """flipdetailのバイナリ枠(0xff,0x00,type,4Bのbe長,payload)を全チャンク列挙する。"""
    out, pos = [], 0
    while pos + 7 <= len(raw):
        if raw[pos] != 0xFF or raw[pos+1] != 0x00: break
        ctype = raw[pos+2]
        ln = int.from_bytes(raw[pos+3:pos+7], "big")
        out.append((ctype, raw[pos+7:pos+7+ln]))
        pos += 7 + ln
    return out

def method_hist(cam, dump=False):
    raw, fetch_ms = cam.flipdetail("info")
    t0 = time.perf_counter()
    chunks = parse_chunks(raw)
    info = next((p for t, p in chunks if t == 0x01), None)
    if info is None:
        return dict(err=f"infoチャンク無し types={[t for t,_ in chunks]} len={len(raw)}", fetch_ms=fetch_ms)
    if dump:
        open("exp_dump_info.json", "wb").write(info)
        print(f"  (生JSONを exp_dump_info.json へ保存 {len(info)}B)")
    j = json.loads(info.rstrip(b"\x00").decode("utf-8", "replace"))
    lvd = j.get("liveviewdata", {})
    hist = lvd.get("histogram")
    if not hist:
        return dict(err=f"histogram無し keys={list(lvd.keys())}", fetch_ms=fetch_ms)
    med = [hist_median(ch) for ch in hist]          # [Y,R,G,B] 各中央値(0..1 sRGB)
    linY = srgb_to_linear(med[0])
    linR, linG, linB = (srgb_to_linear(m) for m in med[1:4])
    lin709 = 0.2126 * linR + 0.7152 * linG + 0.0722 * linB
    lin601 = 0.2990 * linR + 0.5870 * linG + 0.1140 * linB
    calc_ms = (time.perf_counter() - t0) * 1000.0
    return dict(fetch_ms=fetch_ms, calc_ms=calc_ms, bytes=len(raw), nch=len(hist), bins=len(hist[0]),
                medY=med[0], linY=linY, lin709=lin709, lin601=lin601,
                medR=med[1], medG=med[2], medB=med[3])

def method_image(cam, dump=False):
    raw, fetch_ms = cam.flipdetail("image")
    t0 = time.perf_counter()
    # チャンク中からJPEG(SOI 0xFFD8)を探す(枠のtypeに依存しない)
    soi = raw.find(b"\xff\xd8")
    if soi < 0:
        return dict(err=f"JPEG無し len={len(raw)}", fetch_ms=fetch_ms)
    eoi = raw.rfind(b"\xff\xd9")
    jpg = raw[soi:eoi + 2] if eoi > soi else raw[soi:]
    if dump:
        open("exp_dump_image.jpg", "wb").write(jpg)
        print(f"  (JPEGを exp_dump_image.jpg へ保存 {len(jpg)}B)")
    from PIL import Image
    im = Image.open(io.BytesIO(jpg)).convert("L")
    px = list(im.getdata())
    decode_ms = (time.perf_counter() - t0) * 1000.0
    t1 = time.perf_counter()
    px.sort()
    med = px[len(px) // 2] / 255.0
    mean = sum(px) / len(px) / 255.0
    lin_med = srgb_to_linear(med)
    lin_mean = srgb_to_linear(mean)
    calc_ms = (time.perf_counter() - t1) * 1000.0
    return dict(fetch_ms=fetch_ms, decode_ms=decode_ms, calc_ms=calc_ms,
                bytes=len(jpg), size=im.size, medL=med, lin_med=lin_med, lin_mean=lin_mean)

def show(tag, r):
    if "err" in r:
        print(f"  {tag}: ERR {r['err']} (fetch {r['fetch_ms']:.0f}ms)")
        return
    if tag == "hist":
        print(f"  hist : fetch {r['fetch_ms']:6.1f}ms calc {r['calc_ms']:5.2f}ms {r['bytes']}B "
              f"{r['nch']}ch x {r['bins']}bin | medY={r['medY']:.4f} linY={r['linY']:.5f} "
              f"lin709={r['lin709']:.5f} lin601={r['lin601']:.5f} (R{r['medR']:.3f} G{r['medG']:.3f} B{r['medB']:.3f})")
    else:
        print(f"  image: fetch {r['fetch_ms']:6.1f}ms decode {r['decode_ms']:5.1f}ms calc {r['calc_ms']:5.1f}ms "
              f"{r['bytes']}B {r['size']} | medL={r['medL']:.4f} lin_med={r['lin_med']:.5f} lin_mean={r['lin_mean']:.5f}")

def sweep(cam, ss_list, settle_s, per_ss):
    """ssラダー実験: 各ssに設定→settle待ち→両方式で測る。忠実なら1段ごとにリニア輝度が2倍で動く。"""
    print(f"{'ss':>7} | {'linY':>9} {'lin709':>9} {'lin601':>9} | {'img_med':>9} {'img_mean':>9} | 応答(段/段, Y基準)")
    prev = None
    import math
    for ss in ss_list:
        cam.set_exposure(ss=ss)
        time.sleep(settle_s)
        hs, ims = [], []
        for _ in range(per_ss):
            try:
                h = method_hist(cam)
                if "err" not in h: hs.append(h)
            except Exception as e:
                print(f"    (hist例外: {type(e).__name__} {e})")
            try:
                m = method_image(cam)
                if "err" not in m: ims.append(m)
            except Exception as e:
                print(f"    (image例外: {type(e).__name__} {e})")
        if not hs:
            print(f"{ss:>7} | 測光失敗(hist 0件)")
            prev = None
            continue
        a = lambda k, rs: sum(r[k] for r in rs) / len(rs)
        linY, l709, l601 = a("linY", hs), a("lin709", hs), a("lin601", hs)
        imed  = a("lin_med", ims) if ims else float("nan")
        imean = a("lin_mean", ims) if ims else float("nan")
        resp = ""
        if prev and prev[1] > 1e-7 and linY > 1e-7:
            dss = math.log2(parse_ss(ss) / parse_ss(prev[0]))     # 指示した変化[段]
            dY  = math.log2(linY / prev[1])                        # 実際に動いた[段]
            if abs(dss) > 1e-9: resp = f"{dY/dss:+.2f}"
        print(f"{ss:>7} | {linY:9.6f} {l709:9.6f} {l601:9.6f} | {imed:9.6f} {imean:9.6f} | {resp}")
        prev = (ss, linY)

def parse_ss(s):
    # CCAPI表記: "1/50" / "0\"5"(=0.5s) / "8\""(=8s) / "1\"3"(=1.3s)
    if "/" in s:
        a, b = s.split("/"); return float(a) / float(b)
    return float(s.replace('"', '.').rstrip('.'))

if __name__ == "__main__":
    ip = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "both"
    n = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 3
    args = sys.argv[3:]
    def opt(name):
        return args[args.index(name) + 1] if name in args else None
    dump = "--dump" in args

    cam = Cam(ip)
    cam.start_lv()
    cam.set_exposure(ss=opt("--ss"), iso=opt("--iso"), fn=opt("--fn"))
    time.sleep(1.0)  # LV開始/露出反映の落ち着き待ち(実験では十分粗くてよい)

    if mode == "sweep":
        # ss ラダーはカメラの広告値(ability)から構築する(表記の機種差を吸収。8"表記等)。
        raw, _ = http(cam.urls["/shooting/settings/tv"])
        ab = json.loads(raw).get("ability", [])
        vals = []
        for v in ab:
            try: vals.append((parse_ss(v), v))
            except Exception: pass          # bulb 等はスキップ
        vals.sort()                          # 速い(小さい秒)→遅い
        step = int(opt("--step") or 3)       # 3=1段刻み(テーブルは1/3段刻み)
        ladder = [v for _, v in vals[::step]]
        if opt("--ladder"): ladder = opt("--ladder").split(",")
        print("ladder:", " ".join(ladder))
        sweep(cam, ladder, float(opt("--settle") or 3.0), int(opt("--per") or 3))
    else:
        for i in range(n):
            print(f"[{i+1}/{n}]")
            if mode in ("hist", "both"):
                show("hist", method_hist(cam, dump and i == 0))
            if mode in ("image", "both"):
                show("image", method_image(cam, dump and i == 0))
