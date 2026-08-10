# -*- coding: utf-8 -*-
# サムネイル測光の連続テスト(2026-08-11)。EOS R10 を長秒露光で回し続けて固着しないか見る。
#
# 【設計の根拠(実測)】ISO1600/F1.4/8秒 で
#   ・シャッターPOSTは 66〜168ms で戻る(露光完了を待たない)
#   ・露光終了の 約1.0秒後 に新ファイルのサムネイルが取得可能になる(現像+書き込み)
#   ・その後の取得は 44〜51ms
#   ・15秒周期なら 取得可(9秒) から 次のシャッター(15秒) まで 6秒空く
#  前回 R10 が27分で固着したのは、現像中(=503)のカードを高頻度で叩き続けたためと考えられる。
#  そこで「露光終了+1秒 待ってから、名前を予測して1回だけ取りに行く」方式を試す。
#
# 使い方: python thumb_soak.py --ip 192.168.1.11 --minutes 60 --ss 8 --interval 15
import argparse, io, json, math, os, re, sys, time
sys.stdout.reconfigure(encoding='utf-8')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
_spec = importlib.util.spec_from_file_location(
    'bench', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(B)
from PIL import Image

SET = '/ccapi/ver100/shooting/settings/'


def s2l(x): return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def jpeg(b):
    i = b.find(b'\xff\xd8\xff')          # CCAPI の枠(ff 00 01 + 長さ)を剥がす
    return b[i:] if i >= 0 else b


def lum(b):
    """サムネイル(またはLV画像)から 中央値/平均/p90/飽和 をリニアで返す。"""
    im = Image.open(io.BytesIO(jpeg(b))).convert('L')
    h = im.histogram(); t = float(sum(h))
    c = 0.0; med = 1.0
    for k in range(256):
        if c + h[k] >= t / 2:
            med = (k + ((t / 2 - c) / h[k] if h[k] else 0)) / 255.0; break
        c += h[k]
    cc = 0; p90 = 1.0
    for i in range(256):
        cc += h[i]
        if cc >= t * 0.90: p90 = i / 255.0; break
    return (s2l(med), sum(s2l(i / 255.0) * h[i] for i in range(256)) / t, p90, h[255] / t)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.11')
    ap.add_argument('--minutes', type=float, default=60.0)
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--interval', type=float, default=15.0)
    ap.add_argument('--settle', type=float, default=1.0)   # 露光終了後の待ち[秒]
    ap.add_argument('--out', default='soak_r10_thumb.csv')
    a = ap.parse_args()

    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False)
    for k, v in (('iso', a.iso), ('av', a.av), ('tv', a.ss)):
        cam.put(SET + k, {'value': v})
    time.sleep(0.5)
    expo_s = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0

    base, err = B.latest_path_by_listing(cam)
    if not base: print('起点の取得に失敗: %s' % err); return 1
    mm = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', base); seq = int(mm.group(2))
    print('起点 %s  露光%s  周期%.0fs  待ち%.1fs  予定%.0f分'
          % (base.split('/')[-1], a.ss, a.interval, a.settle, a.minutes))

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,wallclock,cycle_ms,post_ms,lv_ms,fetch_ms,retry,http,bytes,medY,meanY,p90,sat,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; ng = 0; worst_cycle = 0.0; prev = None
    while time.time() < t_end:
        t0 = time.perf_counter()
        st, post_ms = B.shoot(cam)
        lv_ms, _ = B.wait_busy(cam, limit_ms=20000)
        # 露光終了 + settle まで待つ(ここが要。現像中のカードを叩かない)
        wait_to = expo_s + a.settle
        rest = wait_to - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
        seq += 1
        path = '%s%0*d%s' % (mm.group(1), len(mm.group(2)), seq, mm.group(3))
        retry = 0; sc = 0; body = b''
        t1 = time.perf_counter()
        for _ in range(4):                       # 503 は 0.5秒おきに最大3回だけ
            sc, body, _ = cam.get(path + '?kind=thumbnail')
            if sc == 200: break
            retry += 1; time.sleep(0.5)
        fetch_ms = (time.perf_counter() - t1) * 1000.0
        note = ''
        med = mean = p90 = sat = -1.0
        if sc == 200:
            try: med, mean, p90, sat = lum(body)
            except Exception as e: note = 'decode:%s' % type(e).__name__
        else:
            ng += 1; note = 'fetchNG'
            q, e2 = B.latest_path_by_listing(cam)   # 外れたら並べ直す
            if q:
                mm = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', q); seq = int(mm.group(2))
                note += '/reseed'
            else:
                note += '/reseedNG:%s' % e2
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        worst_cycle = max(worst_cycle, cyc)
        f.write('%d,%s,%.0f,%.0f,%.0f,%.0f,%d,%d,%d,%.5f,%.5f,%.3f,%.4f,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, post_ms, lv_ms, fetch_ms,
                   retry, sc, len(body), med, mean, p90, sat, note))
        f.flush()
        if n % 20 == 0 or note:
            print('  %4d %s cyc=%6.0f post=%4.0f lv=%4.0f fetch=%5.0f r%d http=%d Y=%.5f %s'
                  % (n, time.strftime('%H:%M:%S'), cyc, post_ms, lv_ms, fetch_ms, retry, sc, med, note))
        rest = a.interval - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
    f.close()
    print('\n終了: %dコマ 取得失敗%d 最大周期%.0fms  → %s' % (n, ng, worst_cycle, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
