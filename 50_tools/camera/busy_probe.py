# -*- coding: utf-8 -*-
# 「シャッターを切ってからサムネイルを取りに行けるまで」をどこまで縮められるかの計測(2026-08-11)。
#
# 【なぜ作り直したか】これまでの thumb_soak / thumb_latest は busy を
#  **シャッターPOST直後**に測っていた。POST は露光完了を待たずに戻るので、実際には
#  露光の真っ最中に測っており「露光終了→測光可」を測れていなかった。
#  ここでは露光が終わってから測り始める。
#
# 【測る合図(--signal で1つ選ぶ)】いずれも「露光終了」を起点とした経過[ms]。
#   lv     : shooting/liveview/flipdetail?kind=info が正しい体裁で返るまで
#            (アプリが今使っている合図。checkLiveViewInfo と同じ判定)
#   card   : contents が 503 をやめて 200 を返すまで(カードが空くまで)
#   file   : **新しいファイルのサムネイルが実際に取れるまで**(これが本命の下限)
#   event  : event/polling が撮影の通知(addedcontents 等)を返すまで
#            ※ 以前 contents 併用で R10 が不応答になり廃止した経緯があるので単独で試す
#
# 合図どうしが干渉しないよう、1回の走行では1つだけ測る。
#
# 使い方: python busy_probe.py --ip 192.168.1.11 --signal file --frames 12
import argparse, io, json, os, re, sys, time, statistics as st
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_b = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_b); _b.loader.exec_module(B)
_l = importlib.util.spec_from_file_location('latest', os.path.join(_here, 'thumb_latest.py'))
L = importlib.util.module_from_spec(_l); _l.loader.exec_module(L)

SET = '/ccapi/ver100/shooting/settings/'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.11')
    ap.add_argument('--signal', default='file', choices=('lv', 'card', 'file', 'event'))
    ap.add_argument('--frames', type=int, default=12)
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--interval', type=float, default=20.0)
    ap.add_argument('--poll', type=float, default=0.1)      # 合図を見に行く間隔[秒]
    # event/polling のクエリ。R10/R100 は ver100 なので continue=on/off を使う
    #  (ver110 系の timeout=long/short は 400 Illegal query parameter になる)。
    ap.add_argument('--eventq', default='continue=off')
    ap.add_argument('--out', default='')
    a = ap.parse_args()

    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False)
    for k, v in (('iso', a.iso), ('av', a.av), ('tv', a.ss)):
        cam.put(SET + k, {'value': v})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    fd = L.Finder(cam)
    ok, err = fd.resolve()
    if not ok: print('ディレクトリ解決に失敗: %s' % err); return 1
    # event/polling は溜まっている通知を先に流しておく(初回が即返るのを避ける)
    if a.signal == 'event':
        cam.get(p['event/polling'] + '?continue=off')   # たまっている通知を先に流す

    print('%s  合図=%s  露光%s  周期%.0fs  %dコマ' % (a.ip, a.signal, a.ss, a.interval, a.frames))
    print(' #   露光終了からの経過[ms]  試行  補足')
    rows = []
    for i in range(a.frames):
        # 撮る前の最新を控える。これより新しい名前が出るまでを「まだ」と判定する
        # (控えないと、まだ載っていないときに前コマのファイルを取って即成功してしまう)。
        base_newest = None
        if a.signal == 'file':
            base_newest, _p, _e = fd.latest_two()
        t0 = time.perf_counter()
        _s, post_ms = B.shoot(cam)
        # 露光が終わるまで待つ(ここでは一切カメラに触らない)
        rest = expo - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
        t_expo_end = time.perf_counter()

        tries = 0; note = ''; elapsed = -1.0
        if a.signal == 'event':
            # continue=off は「今たまっている通知」を返してすぐ切れる。撮影の通知
            # (addedcontents)が載るまで繰り返し引く。R10/R100 の polling は ver100 なので
            # ver110 系の timeout=long/short は 400 Illegal query parameter になる。
            while (time.perf_counter() - t_expo_end) < 25.0:
                tries += 1
                s, b, _ms = cam.get(p['event/polling'] + '?' + a.eventq)
                txt = b.decode('utf-8', 'replace')
                if s == 200 and 'addedcontents' in txt:
                    elapsed = (time.perf_counter() - t_expo_end) * 1000.0
                    mo = re.search(r'IMG_\d+\.[A-Za-z0-9]+', txt)
                    note = mo.group(0) if mo else txt[:80].replace('\n', ' ')
                    break
                time.sleep(a.poll)
            if elapsed < 0: note = 'addedcontents が来ない (http=%d)' % s
        else:
            while (time.perf_counter() - t_expo_end) < 25.0:
                tries += 1
                if a.signal == 'lv':
                    s, b, _ms = cam.get(p['shooting/liveview/flipdetail'] + '?kind=info')
                    done = (s == 200 and B.lv_ready(b))
                elif a.signal == 'card':
                    s, b, _ms = cam.get(p['contents'])
                    done = (s == 200)
                else:                       # file
                    newest, prev, e = fd.latest_two()
                    done = False
                    if newest and newest != base_newest:      # **新しい**ファイルが載ったか
                        s, b, _ms = cam.get(newest + '?kind=thumbnail')
                        done = (s == 200)
                        if done: note = newest.split('/')[-1]
                    elif not newest:
                        note = e
                if done:
                    elapsed = (time.perf_counter() - t_expo_end) * 1000.0
                    break
                time.sleep(a.poll)
        rows.append((elapsed, tries))
        print(' %2d  %10.0f            %4d  %s' % (i + 1, elapsed, tries, note))
        rest = a.interval - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)

    good = [r[0] for r in rows if r[0] >= 0]
    if good:
        print('\n露光終了からの経過: 中央%.0f 平均%.0f 最小%.0f 最大%.0f ms  (%d/%d)'
              % (st.median(good), sum(good) / len(good), min(good), max(good), len(good), len(rows)))
    else:
        print('\n合図が返らなかった')
    if a.out:
        with open(a.out, 'w', encoding='utf-8', newline='') as f:
            f.write('frame,elapsed_ms,tries\n')
            for i, (e, t) in enumerate(rows): f.write('%d,%.0f,%d\n' % (i + 1, e, t))
    return 0


if __name__ == '__main__':
    sys.exit(main())
