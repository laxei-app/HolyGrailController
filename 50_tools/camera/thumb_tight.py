# -*- coding: utf-8 -*-
# サムネイル測光を「待ちなし」で最速に回す連続テスト(2026-08-11)。
#
# 【何を確かめるか】2026-08-11 の実測で、撮影直後のファイルを取りに行くと R10 は4回中4回停止し、
#  1コマ前を取れば止まりにくいと分かった(通知の名前を1コマ持ち越す方式で 180コマ完走)。
#  そこで余計な待ちをすべて外し、実運用より厳しい条件で成立するかを見る。
#
# 【1コマの流れ】固定の周期待ちを入れない。露光が終わったら即座に次へ進む。
#   ① ISO を 1250/1600 で交互に設定(露出は問わない。毎コマ露出を触る実運用に寄せる)
#   ② シャッター(POSTは露光完了を待たずに戻る)
#   ③ 露光時間ぶんだけ待つ ← これは物理的に必要
#   ④ event/polling で「いま追加されたファイル名」を受け取る
#   ⑤ **1つ前**に受け取った名前のサムネイルを取得(生成直後のファイルには触らない)
#   ⑥ すぐ①へ戻る
#
# 使い方: python thumb_tight.py --ip 192.168.1.4 --minutes 60
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
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--isos', default='1250,1600')   # 毎コマ交互に設定する
    ap.add_argument('--maxpoll', type=int, default=10)
    # サムネイル取得のあと、次のシャッターまで空ける時間[秒]。
    # 15秒周期のときはここに約6秒の空白があり180コマ完走したが、0秒にしたら42コマで
    # シャッターPOSTが返らなくなった(カメラは生きたまま。2026-08-11)。境目を探る。
    ap.add_argument('--gap', type=float, default=0.0)
    ap.add_argument('--out', default='soak_tight.csv')
    a = ap.parse_args()

    isos = [x.strip() for x in a.isos.split(',')]
    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=True)
    cam.put(SET + 'av', {'value': a.av})
    cam.put(SET + 'tv', {'value': a.ss})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    cam.get(p['event/polling'] + '?continue=off')       # たまっている通知を流す
    print('%s 露光%s ISO%s交互 シャッター前の空き%.1fs 予定%.0f分'
          % (a.ip, a.ss, '/'.join(isos), a.gap, a.minutes))

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,wallclock,cycle_ms,iso,iso_ms,post_ms,post_http,notify_ms,polls,'
            'fetch_ms,fetch_http,file,medY,meanY,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; ng = 0; stuck = 0; prev = None
    while time.time() < t_end:
        t0 = time.perf_counter()
        iso = isos[n % len(isos)]
        s_iso, _b, iso_ms = cam.put(SET + 'iso', {'value': iso})
        post_http, post_ms = B.shoot(cam)
        rest = expo - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)                    # 露光ぶんだけ。これ以外の待ちは入れない
        te = time.perf_counter()

        path = None; polls = 0
        while polls < a.maxpoll:
            polls += 1
            s, b, _ms = cam.get(p['event/polling'] + '?continue=on')
            if s != 200: break
            mo = ADDED.search(b.decode('utf-8', 'replace'))
            if mo:
                names = [x.replace('\\/', '/') for x in re.findall(r'"([^"]+)"', mo.group(1))]
                if names: path = names[-1]; break
        notify_ms = (time.perf_counter() - te) * 1000.0

        sc = 0; body = b''; fetch_ms = 0.0; note = ''
        med = mean = -1.0
        if not path:
            ng += 1; note = '通知なし'
        elif prev:                                        # **1つ前**を取りに行く
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
                ng += 1; note = 'fetch(%d)' % sc
        if path: prev = path
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        f.write('%d,%s,%.0f,%s,%.0f,%.0f,%d,%.0f,%d,%.0f,%d,%s,%.5f,%.5f,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, iso, iso_ms, post_ms, post_http,
                   notify_ms, polls, fetch_ms, sc, (prev or '').split('/')[-1], med, mean, note))
        f.flush()
        if n % 25 == 0 or note:
            print('  %4d %s cyc=%6.0f ISO%s(%.0fms) post=%.0f 通知=%.0f(%d) 取得=%.0f %s %s'
                  % (n, time.strftime('%H:%M:%S'), cyc, iso, iso_ms, post_ms, notify_ms,
                     polls, fetch_ms, (prev or '').split('/')[-1], note))
        stuck = stuck + 1 if note else 0
        if stuck >= 3:
            print('  !! 停止と判断して中断 コマ%d %s' % (n, time.strftime('%H:%M:%S')))
            break
        if a.gap > 0: time.sleep(a.gap)      # 次のシャッターまでの空き
    f.close()
    print('\n終了: %dコマ 失敗%d → %s' % (n, ng, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
