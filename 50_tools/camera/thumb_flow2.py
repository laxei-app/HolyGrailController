# -*- coding: utf-8 -*-
# サムネイル測光の連続テスト(その4・2026-08-12)。待ちの置き場所を変えた版。
#
# 【前回との違い】前回は「露光終了 → 2秒待つ → 通知」だった。通知は1回28msで届くようになった
#  代わりに、記録が終わるまでカメラを2秒放置してから声をかける形だった。
#  今回は逆に **通知は露光終了と同時に引き、待ちはその後ろへ置く**。
#
# 【1コマの流れ】
#   ① シャッター(POSTは露光完了を待たずに戻る)
#   ② 露光時間ぶん待つ                       ← 物理的に必要
#   ③ **即** event/polling でファイル名を受け取る(待たない)
#   ④ 受け取った名前を保存し、**2秒待つ**
#   ⑤ **1つ前**のコマのサムネイルを取得(生成直後のファイルには触らない)
#   ⑥ **即** 露出設定(ISO を 1250/1600 で交互)
#   ⑦ **2秒待って**から ①へ戻る
#
# 【異常時】通知が来なかったら、その場で終了する。busy のカメラにシャッターを投げると
#  503 まで進んで固着が深まる(2026-08-11 実測)。追い打ちを避ける。
#
# 使い方: python thumb_flow2.py --ip 192.168.1.4 --minutes 60
import argparse, os, re, sys, time, statistics as st
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_b = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_b); _b.loader.exec_module(B)
_l = importlib.util.spec_from_file_location('latest', os.path.join(_here, 'thumb_latest.py'))
L = importlib.util.module_from_spec(_l); _l.loader.exec_module(L)

SET = '/ccapi/ver100/shooting/settings/'
ADDED = re.compile(r'"addedcontents"\s*:\s*\[(.*?)\]', re.S)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.4')
    ap.add_argument('--minutes', type=float, default=60.0)
    ap.add_argument('--frames', type=int, default=0)   # 0=時間で打ち切り / >0=コマ数で打ち切り
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--isos', default='1250,1600')
    ap.add_argument('--maxpoll', type=int, default=10)
    ap.add_argument('--w1', type=float, default=2.0)   # ④ 通知のあと、取得までの待ち
    ap.add_argument('--w2', type=float, default=2.0)   # ⑦ 露出設定のあと、シャッターまでの待ち
    # ファイル名の求め方。poll=event/polling の通知 / predict=連番予測(通知を一切使わない)。
    # 通知そのものが固着の原因かを切り分けるため(2026-08-12)。他の条件はすべて揃える。
    ap.add_argument('--name', default='poll', choices=('poll', 'predict'))
    # 何コマ前のサムネイルを取るか。取得対象が新しいほど早く固着することが分かったので
    # (直後=19〜49回 / 2.8秒前=145回 / 15秒前=238回 / 数時間前=500回でも無事)、
    # 遅らせて予算が伸びるかを見る(2026-08-12)。露出制御は分単位なので数コマ遅れは無害。
    # JPG+RAW のとき、どちらのサムネイルを取るか。同じ絵でも取得元で挙動が違う疑いが
    # あるため分けられるようにした(2026-08-13)。
    ap.add_argument('--pick', default='jpg', choices=('jpg', 'raw'))
    ap.add_argument('--back', type=int, default=1)
    ap.add_argument('--out', default='soak_flow2.csv')
    a = ap.parse_args()

    isos = [x.strip() for x in a.isos.split(',')]
    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=True)
    cam.put(SET + 'av', {'value': a.av})
    cam.put(SET + 'tv', {'value': a.ss})
    cam.put(SET + 'iso', {'value': isos[0]})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    seq = 0; pre = ''; wid = 4; ext = '.CR3'
    if a.name == 'poll':
        cam.get(p['event/polling'] + '?continue=off')  # たまっている通知を流す
    else:
        # 連番の起点を1度だけ一覧から求める。フォーマット直後で空なら1枚撮って作る。
        base, err = B.latest_path_by_listing(cam)
        if not base:
            B.shoot(cam); time.sleep(expo + 4)
            base, err = B.latest_path_by_listing(cam)
        if not base:
            print('起点が取れない: %s' % err, flush=True); return 1
        mm = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', base)
        pre, wid, ext, seq = mm.group(1), len(mm.group(2)), mm.group(3), int(mm.group(2))
        print('  連番の起点: %s' % base.split('/')[-1], flush=True)
    print('%s 露光%s ISO%s交互  名前=%s / 名前取得後%.1fs で取得 / 露出設定後%.1fs でシャッター  予定%.0f分'
          % (a.ip, a.ss, '/'.join(isos), a.name, a.w1, a.w2, a.minutes), flush=True)
    print('  取得元=%s / %dコマ前' % (a.pick, a.back), flush=True)

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,wallclock,cycle_ms,post_ms,post_http,notify_http,notify_ms,polls,'
            'fetch_ms,fetch_http,iso,iso_ms,file,medY,meanY,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; prev = None; hist = []
    while time.time() < t_end and (not a.frames or n < a.frames):
        t0 = time.perf_counter()
        # ① シャッター(露出は前のコマの末尾で設定済み)
        post_http, post_ms = B.shoot(cam)
        # ② 露光ぶん待つ
        rest = expo - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
        te = time.perf_counter()

        # ③ ファイル名を得る。poll=通知を引く / predict=連番を1つ進めるだけ(通信しない)
        path = None; polls = 0; notify_http = 0
        if a.name == 'predict':
            seq += 1
            path = '%s%0*d%s' % (pre, wid, seq, ext)
        else:
            while polls < a.maxpoll:
                polls += 1
                s, b, _ms = cam.get(p['event/polling'] + '?continue=on')
                notify_http = s
                if s != 200: break
                mo = ADDED.search(b.decode('utf-8', 'replace'))
                if mo:
                    names = [x.replace('\\/', '/') for x in re.findall(r'"([^"]+)"', mo.group(1))]
                    # JPG+RAW だと1コマで2つ返る(CR3とJPG)。サムネイルは JPG から取る。
                    ext = ('.JPG', '.JPEG') if a.pick == 'jpg' else ('.CR3', '.CRAW', '.CR2')
                    pk = [x for x in names if x.upper().endswith(ext)]
                    if pk:    path = pk[-1]; break
                    if names: path = names[-1]; break
        notify_ms = (time.perf_counter() - te) * 1000.0

        sc = 0; body = b''; fetch_ms = 0.0; note = ''; med = mean = -1.0
        iso = ''; iso_ms = 0.0
        if not path:
            note = '通知なし'
        else:
            # ④ 名前を保存して待つ
            hist.append(path)
            time.sleep(a.w1)
            # ⑤ back コマ前のサムネイルを取得
            prev = hist[-1 - a.back] if len(hist) > a.back else None
            if prev:
                t1 = time.perf_counter()
                for _ in range(3):
                    sc, body, _ms = cam.get(prev + '?kind=thumbnail')
                    if sc != 503: break
                    time.sleep(0.3)
                fetch_ms = (time.perf_counter() - t1) * 1000.0
                if sc == 200:
                    try: med, mean, _p90, _sat = L.lum(body)
                    except Exception as e: note = 'decode:%s' % type(e).__name__
                else:
                    note = 'fetch(%d)' % sc
                    if a.name == 'predict':
                        # 名前を外したら一覧で並べ直す(フォルダ繰り上がり・ページ境界など)
                        q, _e = B.latest_path_by_listing(cam)
                        if q:
                            m2 = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', q)
                            pre, wid, ext, seq = m2.group(1), len(m2.group(2)), m2.group(3), int(m2.group(2))
                            note += '/reseed'
            # ⑥ 即 露出設定
            iso = isos[n % len(isos)]
            _s, _b2, iso_ms = cam.put(SET + 'iso', {'value': iso})
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        f.write('%d,%s,%.0f,%.0f,%d,%d,%.0f,%d,%.0f,%d,%s,%.0f,%s,%.5f,%.5f,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, post_ms, post_http, notify_http, notify_ms,
                   polls, fetch_ms, sc, iso, iso_ms, (prev or '').split('/')[-1], med, mean, note))
        f.flush()
        if n % 25 == 0 or note:
            print('  %4d %s cyc=%6.0f post=%d/%.0f 通知=%d/%.0f(%d回) 取得=%.0f ISO%s(%.0f) %s %s'
                  % (n, time.strftime('%H:%M:%S'), cyc, post_http, post_ms, notify_http,
                     notify_ms, polls, fetch_ms, iso, iso_ms, (prev or '').split('/')[-1], note),
                  flush=True)
        if note:
            print('  !! %s のため終了 コマ%d %s（これ以上シャッターを投げない）'
                  % (note, n, time.strftime('%H:%M:%S')), flush=True)
            break
        # ⑦ 2秒待ってから次のシャッター
        time.sleep(a.w2)
    f.close()
    print('\n終了: %dコマ → %s' % (n, a.out), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
