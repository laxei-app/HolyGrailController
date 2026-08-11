# -*- coding: utf-8 -*-
# EOS R10 が突然 CCAPI ごと落ちる件の切り分け(2026-08-11)。
#
# 【なぜ作り直したか】2026-08-11 に6回走らせて3回落ちたが、方式(連番予測/一覧/通知)・待ち時間・
#  カード接触回数・ライブビューの有無、どれとも相関しなかった。同条件で180コマ完走と9コマ停止の
#  両方が起きている。確率的な現象なので、条件を1つずつ変えて**複数回**走らせないと判断できない。
#
# 【この道具の要点】
#  1. --fetch off で「撮るだけ」にできる。ユーザーの実績(取得なしなら何日も安定)を
#     **この実験装置で**再現できるかを先に確かめる。落ちるなら原因は取得ではなく装置側。
#  2. **別接続の見張り**を持つ。撮影に使う接続とは独立に 5秒おきに /ccapi を叩き、
#     「CCAPIサーバーが死んだ」のか「撮影用の接続だけが詰まった」のかを区別する。
#     後者なら張り直すだけで復帰するはずで、対策がまったく変わる。
#  3. 落ちたら**すぐ止める**。以前は気づかず13分叩き続け、状態を悪化させた。
#
# 使い方:
#   python r10_isolate.py --ip 192.168.1.4 --fetch off --minutes 25 --tag A1
#   python r10_isolate.py --ip 192.168.1.4 --fetch on  --minutes 25 --tag B1
import argparse, json, os, re, sys, threading, time
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_b = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_b); _b.loader.exec_module(B)
_l = importlib.util.spec_from_file_location('latest', os.path.join(_here, 'thumb_latest.py'))
L = importlib.util.module_from_spec(_l); _l.loader.exec_module(L)

SET = '/ccapi/ver100/shooting/settings/'
ADDED = re.compile(r'"addedcontents"\s*:\s*\[(.*?)\]', re.S)


class Watch(threading.Thread):
    """撮影用とは別の接続で生存を見張る。落ちた瞬間と、張り直しで戻るかを記録する。"""

    def __init__(self, ip, log):
        super().__init__(daemon=True)
        self.ip = ip; self.log = log
        self.stop = threading.Event()
        self.alive = True
        self.first_dead = None      # 最初に死んだ時刻
        self.fresh_ok = None        # 死んだ後、新しい接続なら通ったか

    def run(self):
        cam = B.Cam(self.ip)
        while not self.stop.wait(5.0):
            s, b, ms = cam.get('/ccapi')
            ok = (s == 200)
            if ok != self.alive:
                self.alive = ok
                self.log('    [見張り] %s %s (http=%d %.0fms)'
                         % (time.strftime('%H:%M:%S'), '復活' if ok else '**応答なし**', s, ms))
                if not ok and self.first_dead is None:
                    self.first_dead = time.strftime('%H:%M:%S')
                    # まっさらな接続なら通るか(接続だけの問題かを見る)
                    fresh = B.Cam(self.ip)
                    s2, _b2, _m2 = fresh.get('/ccapi')
                    self.fresh_ok = (s2 == 200)
                    self.log('    [見張り] 新しい接続での再試行: http=%d → %s'
                             % (s2, '通った(接続だけの問題)' if s2 == 200 else '通らない(サーバーが死んでいる)'))
                    fresh.reset()
            if not ok: cam.reset()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.4')
    ap.add_argument('--fetch', default='on', choices=('on', 'off'))
    ap.add_argument('--liveview', action='store_true')
    ap.add_argument('--minutes', type=float, default=25.0)
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--interval', type=float, default=15.0)
    ap.add_argument('--tag', default='X')
    a = ap.parse_args()

    outdir = os.path.join(_here, 'soak_out'); os.makedirs(outdir, exist_ok=True)
    csvp = os.path.join(outdir, 'iso_%s.csv' % a.tag)
    logp = os.path.join(outdir, 'iso_%s.log' % a.tag)
    lf = open(logp, 'w', encoding='utf-8')

    def log(msg):
        print(msg); lf.write(msg + '\n'); lf.flush()

    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=a.liveview)
    for k, v in (('iso', a.iso), ('av', a.av), ('tv', a.ss)):
        cam.put(SET + k, {'value': v})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    cam.get(p['event/polling'] + '?continue=off')
    log('== %s  取得=%s  LV=%s  露光%s 周期%.0fs  %.0f分 ==' %
        (a.tag, a.fetch, 'あり' if a.liveview else 'なし', a.ss, a.interval, a.minutes))

    w = Watch(a.ip, log); w.start()
    f = open(csvp, 'w', encoding='utf-8', newline='')
    # post_http = シャッターが受理されたか。以前これを記録しておらず、止まったコマで
    # 撮影が続いていたのか拒否されたのかを後から判断できなかった(2026-08-11)。
    f.write('frame,wallclock,cycle_ms,post_http,post_ms,notify_http,notify_ms,polls,'
            'fetch_ms,fetch_http,file,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; stuck = 0
    while time.time() < t_end:
        t0 = time.perf_counter()
        post_http, post_ms = B.shoot(cam)
        rest = expo - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
        te = time.perf_counter()

        path = None; polls = 0; notify_ms = 0.0; fetch_ms = 0.0; sc = 0; note = ''
        # 通知は取得しないときも受ける(撮れているかの確認に要る。カードには触らない)
        notify_http = 0
        while polls < 8:
            polls += 1
            s, b, _ms = cam.get(p['event/polling'] + '?continue=on')
            notify_http = s
            if s != 200: break
            mo = ADDED.search(b.decode('utf-8', 'replace'))
            if mo:
                names = [x.replace('\\/', '/') for x in re.findall(r'"([^"]+)"', mo.group(1))]
                if names: path = names[-1]; break
        notify_ms = (time.perf_counter() - te) * 1000.0
        if not path: note = '通知なし'
        elif a.fetch == 'on':
            t1 = time.perf_counter()
            for _ in range(4):
                sc, body, _ms = cam.get(path + '?kind=thumbnail')
                if sc != 503: break
                time.sleep(0.5)
            fetch_ms = (time.perf_counter() - t1) * 1000.0
            if sc != 200: note = 'fetch(%d)' % sc
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        f.write('%d,%s,%.0f,%d,%.0f,%d,%.0f,%d,%.0f,%d,%s,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, post_http, post_ms, notify_http, notify_ms,
                   polls, fetch_ms, sc, (path or '').split('/')[-1], note))
        f.flush()
        if n % 20 == 0 or note:
            log('  %4d %s cyc=%6.0f post=%d/%.0fms 通知=%d/%.0fms(%d回) 取得=%.0fms %s %s'
                % (n, time.strftime('%H:%M:%S'), cyc, post_http, post_ms, notify_http, notify_ms,
                   polls, fetch_ms, (path or '').split('/')[-1], note))
        stuck = stuck + 1 if note else 0
        if stuck >= 2:                       # 2コマで打ち切る(叩き続けない)
            log('  !! 停止と判断して中断 コマ%d %s' % (n, time.strftime('%H:%M:%S')))
            break
        rest = a.interval - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
    f.close()
    time.sleep(6)                            # 見張りの最後の判定を拾う
    w.stop.set()
    log('== %s 終了: %dコマ  見張り: %s ==' %
        (a.tag, n, ('最後まで生存' if w.first_dead is None else
                    '%s に応答なし / 新接続 %s' % (w.first_dead, 'OK' if w.fresh_ok else 'NG'))))
    lf.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
