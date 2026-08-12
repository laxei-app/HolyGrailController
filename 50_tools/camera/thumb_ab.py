# -*- coding: utf-8 -*-
# サムネイル取得を止めたら回復するのかを見る A/B 交互テスト(2026-08-12)。
#
# 【なぜこれをやるか】これまでの停止コマ数は 236/237/237/238/239 と、周期・空き・通知の有無・
#  LV待ちの有無を変えてもほぼ一定だった。決めているのは**コマ数**であって、調整してきた
#  待ち時間ではない。ただし「1コマに1回しかないもの」は撮影とサムネイル取得の2つあり、
#  どちらが数えられているのか分かっていない(取得なしの対照実験は100コマ×3回で、
#  1回も237に届いていなかった)。
#
# 【切り分け方】サムネイル取得をする a と、代わりにライブビューを取る b を 110コマずつ
#  交互に3セット回す。撮影回数は増え続けるが、サムネイル取得回数は半分しか増えない。
#   ・**コマ数**が効くなら → 通算238コマ付近 = 2セット目の b の途中で止まる
#   ・**取得回数**が効くなら → 取得238回目 = 3セット目の a の途中(通算458コマ付近)で止まる
#   ・660コマ完走なら → どちらでもない
#
# 【1コマの流れ】(a/b で違うのは④だけ。他はすべて揃える)
#   ① シャッター(POSTは露光完了を待たずに戻る)
#   ② 露光時間ぶん待つ                       ← 物理的に必要
#   ③ event/polling でファイル名を受け取る(待たない)
#   ④ a: **1つ前**のコマのサムネイルを取得 / b: ライブビュー画像を取得
#   ⑤ 露出設定(ISO を 1250/1600 で交互)
#   ⑥ 4秒待ってから ①へ戻る
#
# 【異常時】その場で終了する。busy のカメラにシャッターを投げると固着が深まる(2026-08-11 実測)。
#
# 使い方: python thumb_ab.py --ip 192.168.1.4 --sets 3 --n 110
import argparse, os, re, sys, time
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
    ap.add_argument('--sets', type=int, default=3)     # a+b で1セット
    ap.add_argument('--n', type=int, default=110)      # 各モードのコマ数
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--isos', default='1250,1600')
    ap.add_argument('--maxpoll', type=int, default=10)
    ap.add_argument('--w2', type=float, default=4.0)   # ⑥ 露出設定のあと、シャッターまでの待ち
    # モードを切り替えるたびに HTTP 接続を切って張り直す。取得回数の予算が接続に
    # 紐づいているかを見るため(2026-08-12)。別プロセスでは持ち越された実績がある。
    ap.add_argument('--switch-reset', type=int, default=0)
    ap.add_argument('--out', default='soak_ab.csv')
    a = ap.parse_args()

    isos = [x.strip() for x in a.isos.split(',')]
    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=True)
    cam.put(SET + 'av', {'value': a.av})
    cam.put(SET + 'tv', {'value': a.ss})
    cam.put(SET + 'iso', {'value': isos[0]})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    cam.get(p['event/polling'] + '?continue=off')      # たまっている通知を流す
    lvpath = p['shooting/liveview/flipdetail'] + '?kind=image'

    total = a.sets * 2 * a.n
    est = (expo + 1.2 + a.w2) * total / 60.0
    print('%s 露光%s ISO%s交互 待ち%.1fs  a/b各%dコマ×%dセット=計%dコマ(約%.0f分)'
          % (a.ip, a.ss, '/'.join(isos), a.w2, a.n, a.sets, total, est), flush=True)
    print('  a=1つ前のサムネイル取得 / b=ライブビュー取得（他の条件は同一）', flush=True)

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,set,mode,thumbs,wallclock,cycle_ms,post_ms,post_http,notify_http,notify_ms,'
            'polls,fetch_ms,fetch_http,fetch_bytes,iso,iso_ms,reconn,file,medY,meanY,note\n')
    n = 0; thumbs = 0; prev = None; stop = ''
    for si in range(1, a.sets + 1):
        for mode in ('a', 'b'):
            if a.switch_reset and n:
                cam.reset()                       # TCP を明示的に閉じる
                time.sleep(2.0)
                s0, _b0, ms0 = cam.get('/ccapi/ver100/deviceinformation')
                print('    ** 接続を張り直した http=%d %.0fms (通算再接続%d回)'
                      % (s0, ms0, cam.reconnects), flush=True)
            print('--- セット%d モード%s 開始 %s (通算%dコマ / サムネ取得%d回)'
                  % (si, mode, time.strftime('%H:%M:%S'), n, thumbs), flush=True)
            for _ in range(a.n):
                t0 = time.perf_counter()
                # ① シャッター(露出は前のコマの末尾で設定済み)
                post_http, post_ms = B.shoot(cam)
                # ② 露光ぶん待つ
                rest = expo - (time.perf_counter() - t0)
                if rest > 0: time.sleep(rest)
                te = time.perf_counter()

                # ③ event/polling でファイル名を受け取る
                path = None; polls = 0; notify_http = 0
                while polls < a.maxpoll:
                    polls += 1
                    s, b, _ms = cam.get(p['event/polling'] + '?continue=on')
                    notify_http = s
                    if s != 200: break
                    mo = ADDED.search(b.decode('utf-8', 'replace'))
                    if mo:
                        names = [x.replace('\\/', '/') for x in re.findall(r'"([^"]+)"', mo.group(1))]
                        if names: path = names[-1]; break
                notify_ms = (time.perf_counter() - te) * 1000.0

                sc = 0; body = b''; fetch_ms = 0.0; note = ''; med = mean = -1.0
                iso = ''; iso_ms = 0.0
                if not path:
                    note = '通知なし'
                else:
                    # ④ a=1つ前のサムネイル / b=ライブビュー
                    tgt = prev if mode == 'a' else lvpath
                    if tgt:
                        if mode == 'a': thumbs += 1
                        t1 = time.perf_counter()
                        for _r in range(3):
                            sc, body, _ms = cam.get(tgt + ('?kind=thumbnail' if mode == 'a' else ''))
                            if sc != 503: break
                            time.sleep(0.3)
                        fetch_ms = (time.perf_counter() - t1) * 1000.0
                        if sc == 200:
                            try: med, mean, _p90, _sat = L.lum(body)
                            except Exception as e: note = 'decode:%s' % type(e).__name__
                        else:
                            note = 'fetch%s(%d)' % (mode, sc)
                    prev = path
                    # ⑤ 露出設定
                    iso = isos[n % len(isos)]
                    _s, _b2, iso_ms = cam.put(SET + 'iso', {'value': iso})
                n += 1
                cyc = (time.perf_counter() - t0) * 1000.0
                f.write('%d,%d,%s,%d,%s,%.0f,%.0f,%d,%d,%.0f,%d,%.0f,%d,%d,%s,%.0f,%d,%s,%.5f,%.5f,%s\n'
                        % (n, si, mode, thumbs, time.strftime('%H:%M:%S'), cyc, post_ms, post_http,
                           notify_http, notify_ms, polls, fetch_ms, sc, len(body), iso, iso_ms, cam.reconnects,
                           (prev or '').split('/')[-1], med, mean, note))
                f.flush()
                if n % 25 == 0 or note:
                    print('  %4d[%d%s] %s cyc=%6.0f post=%d/%.0f 通知=%.0f(%d回) 取得=%.0f/%dB '
                          'サムネ%d %s' % (n, si, mode, time.strftime('%H:%M:%S'), cyc, post_http,
                                          post_ms, notify_ms, polls, fetch_ms, len(body), thumbs, note),
                          flush=True)
                if note:
                    stop = '%s コマ%d(セット%d モード%s サムネ取得%d回目) %s' % (
                        note, n, si, mode, thumbs, time.strftime('%H:%M:%S'))
                    print('  !! %s のため終了（これ以上シャッターを投げない）' % stop, flush=True)
                    break
                # ⑥ 4秒待ってから次のシャッター
                time.sleep(a.w2)
            if stop: break
        if stop: break
    f.close()
    print('\n終了: 通算%dコマ / サムネイル取得%d回 → %s' % (n, thumbs, a.out), flush=True)
    if stop: print('停止: %s' % stop, flush=True)
    else:    print('全%dコマ完走（コマ数説・取得回数説とも否定）' % n, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
