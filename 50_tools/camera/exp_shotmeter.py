# -*- coding: utf-8 -*-
# 「撮影画像フィードバック」実験: 実際にシャッターを切り、撮れた画像から
# リニア輝度を取得できるか+各段階の所要時間を測る(PCからCCAPI直叩き)。
#
# 流れ: 露出設定 → shutterbutton(af無し) → event/polling で新規ファイル検知
#       → contents を kind=thumbnail / display で取得 → 復号 → 中央値/平均 → 逆ガンマ
#
# 使い方:
#   python exp_shotmeter.py <ip> [--ss 8"] [--iso 1600] [--fn 1.4] [--n 3]
import sys, json, time, io, urllib.request, urllib.error

def http(url, method="GET", body=None, timeout=45):
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

def parse_ss(s):
    if "/" in s:
        a, b = s.split("/"); return float(a) / float(b)
    return float(s.replace('"', '.').rstrip('.'))

class Cam:
    def __init__(self, ip):
        self.base = f"http://{ip}:8080"
        raw, ms = http(self.base + "/ccapi")
        j = json.loads(raw)
        self.urls = {}
        for ver, apis in j.items():
            if not isinstance(apis, list): continue
            for a in apis:
                p = a.get("path", "")
                for sfx in ("control/shutterbutton", "/shooting/settings/tv",
                            "/shooting/settings/iso", "/shooting/settings/av",
                            "/event/polling"):
                    if p.endswith(sfx):
                        self.urls[sfx] = self.base + p
        print(f"connect ok ({ms:.0f}ms)  shutter={self.urls.get('control/shutterbutton','-')}"
              f"  polling={self.urls.get('/event/polling','-')}")

    def set_exposure(self, ss=None, iso=None, fn=None):
        for sfx, val in (("/shooting/settings/tv", ss), ("/shooting/settings/iso", iso),
                         ("/shooting/settings/av", fn)):
            if val is None: continue
            raw, ms = http(self.urls[sfx], "PUT", {"value": val})
            print(f"set {sfx.split('/')[-1]}={val} ok ({ms:.0f}ms)")

    def drain_events(self):
        # たまっているイベントを読み捨てる(直前の撮影の addedcontents を拾わないように)。
        try: http(self.urls["/event/polling"], timeout=3)
        except Exception: pass

    def shoot_and_meter(self, ss_sec):
        t_start = time.perf_counter()
        raw, sh_ms = http(self.urls["control/shutterbutton"], "POST", {"af": False})
        # 新規ファイルが registered されるまで event/polling を回す(露光+現像+SD書込を含む)
        added = None
        while added is None:
            if time.perf_counter() - t_start > ss_sec + 40:
                raise TimeoutError("addedcontents が来ない")
            try:
                raw, _ = http(self.urls["/event/polling"], timeout=40)
                ev = json.loads(raw)
                ac = ev.get("addedcontents")
                if ac: added = ac[0]
            except urllib.error.HTTPError:
                time.sleep(0.3)
        t_added = time.perf_counter()
        detect_ms = (t_added - t_start) * 1000.0
        overhead_ms = detect_ms - ss_sec * 1000.0
        print(f"  shutter要求 {sh_ms:.0f}ms / 露光込みファイル登録まで {detect_ms:.0f}ms "
              f"(露光{ss_sec:.1f}s を引いた実質 {overhead_ms:.0f}ms)")
        print(f"  file: {added}")

        out = {}
        for kind in ("thumbnail", "display"):
            try:
                t0 = time.perf_counter()
                raw, fetch_ms = http(self.base + added + f"?kind={kind}", timeout=30)
                from PIL import Image
                im = Image.open(io.BytesIO(raw)).convert("L")
                px = list(im.getdata())
                px.sort()
                med = px[len(px) // 2] / 255.0
                mean = sum(px) / len(px) / 255.0
                calc_ms = (time.perf_counter() - t0) * 1000.0 - fetch_ms
                lin_med, lin_mean = srgb_to_linear(med), srgb_to_linear(mean)
                print(f"  {kind:9}: fetch {fetch_ms:6.1f}ms + 復号/計算 {calc_ms:5.1f}ms "
                      f"{len(raw):7d}B {im.size} | med={med:.4f} lin_med={lin_med:.6f} lin_mean={lin_mean:.6f}")
                out[kind] = dict(fetch_ms=fetch_ms, calc_ms=calc_ms, bytes=len(raw),
                                 size=im.size, lin_med=lin_med, lin_mean=lin_mean)
            except Exception as e:
                print(f"  {kind:9}: ERR {type(e).__name__} {e}")
        return out

if __name__ == "__main__":
    ip = sys.argv[1]
    args = sys.argv[2:]
    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default
    ss = opt("--ss", '8"')
    iso = opt("--iso", "1600")
    fn = opt("--fn")
    n = int(opt("--n", "3"))

    cam = Cam(ip)
    cam.set_exposure(ss=ss, iso=iso, fn=fn)
    time.sleep(0.5)
    for i in range(n):
        print(f"[{i+1}/{n}] ss={ss} iso={iso}")
        cam.drain_events()
        cam.shoot_and_meter(parse_ss(ss))
