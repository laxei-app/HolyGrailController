# -*- coding: utf-8 -*-
# サムネイル測光の連続テスト(その3・2026-08-11)。撮影の通知(event/polling)でファイル名を得る版。
#
# 【なぜこれを試すか】これまでは撮影後に一覧(?kind=number + ?kind=list)を引いて最新を求めていたが、
#  カメラ自身が addedcontents で**ファイル名を教えてくれる**。一覧が丸ごと不要になり、
#  カードへ触るのがサムネイル取得の1回だけになる。実測(2026-08-11 R10・露光終了起点):
#    lv(flipdetail?kind=info)  27ms   ← 早いがファイルはまだ取れない。合図として使えない
#    contents が503をやめる    468ms
#    addedcontents が届く      934ms  (continue=on なら3回のポーリングで届く)
#    サムネイルが実際に取れる 1777ms  (一覧経由。名前が分かっていればもっと早いはず)
#
# 【注意】event/polling は以前 contents と併用して R10 が29〜48コマで不応答になり廃止した経緯が
#  ある。今回は一覧を引かないので併用にならないが、長時間まわして再発しないか確かめる必要がある。
#  エンドポイントは ver100。ver110 系の timeout=long/short は 400 Illegal query parameter。
#
# 使い方: python thumb_event.py --ip 192.168.1.11 --minutes 45 --interval 15
import argparse, io, json, os, re, sys, time, statistics as st
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
    ap.add_argument('--ip', default='192.168.1.11')
    ap.add_argument('--minutes', type=float, default=45.0)
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--interval', type=float, default=15.0)
    ap.add_argument('--eventq', default='continue=on')
    ap.add_argument('--maxpoll', type=int, default=8)     # 1コマで通知を待つ最大回数
    # 通知で受け取った名前を何コマ持ち越してから取りに行くか。
    #  0 = 通知されたその場で取る(=生成直後のファイルに触る)
    #  1 = 1コマ持ち越す(=15秒前に書き終わったファイルを取る)
    # 2026-08-11 の実測では、生成直後を取る方式は4回中4回停止し、1コマ前を取る方式は
    # 3回中2回完走した。通知を使うと名前が分かるのが撮影直後のファイルだけなので、
    # 何も考えないと生成直後を掴んでしまう。持ち越して回避する。
    ap.add_argument('--back', type=int, default=1)
    # ライブビューを掴むか。サムネイル測光では不要。既定は掴まない。
    ap.add_argument('--liveview', action='store_true')
    ap.add_argument('--out', default='soak_event.csv')
    a = ap.parse_args()

    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=a.liveview)
    for k, v in (('iso', a.iso), ('av', a.av), ('tv', a.ss)):
        cam.put(SET + k, {'value': v})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    cam.get(p['event/polling'] + '?continue=off')          # たまっている通知を流す
    print('%s  露光%s 周期%.0fs  通知=%s  LV=%s  %dコマ持ち越し  予定%.0f分'
          % (a.ip, a.ss, a.interval, a.eventq, 'あり' if a.liveview else 'なし', a.back, a.minutes))

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,wallclock,cycle_ms,post_ms,notify_ms,polls,fetch_ms,http,bytes,'
            'medY,meanY,p90,sat,file,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; ng = 0; stuck = 0
    pend = []          # 通知で受け取った名前を持ち越す(古い順)
    while time.time() < t_end:
        t0 = time.perf_counter()
        _s, post_ms = B.shoot(cam)
        rest = expo - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)                       # 露光中はカメラに触らない
        t_end_expo = time.perf_counter()

        # --- 撮影の通知でファイル名を受け取る(一覧は引かない) ---
        path = None; polls = 0
        while polls < a.maxpoll:
            polls += 1
            s, b, _ms = cam.get(p['event/polling'] + '?' + a.eventq)
            if s != 200: break
            mo = ADDED.search(b.decode('utf-8', 'replace'))
            if mo:
                names = re.findall(r'"([^"]+)"', mo.group(1))
                names = [x.replace('\\/', '/') for x in names]
                if names: path = names[-1]; break
        notify_ms = (time.perf_counter() - t_end_expo) * 1000.0

        # 受け取った名前は列に積み、a.back コマ前のものを取りに行く。
        if path: pend.append(path)
        target = pend[-(a.back + 1)] if len(pend) > a.back else None
        if len(pend) > a.back + 4: pend.pop(0)

        sc = 0; body = b''; fetch_ms = 0.0; note = ''
        med = mean = p90 = sat = -1.0
        if not path:
            ng += 1; note = '通知なし'
        elif target:
            t1 = time.perf_counter()
            for _ in range(4):
                sc, body, _ms = cam.get(target + '?kind=thumbnail')
                if sc != 503: break
                time.sleep(0.5)
            fetch_ms = (time.perf_counter() - t1) * 1000.0
            if sc == 200:
                try: med, mean, p90, sat = L.lum(body)
                except Exception as e: note = 'decode:%s' % type(e).__name__
            else:
                ng += 1; note = 'fetch(%d)' % sc
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        f.write('%d,%s,%.0f,%.0f,%.0f,%d,%.0f,%d,%d,%.5f,%.5f,%.3f,%.4f,%s,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, post_ms, notify_ms, polls, fetch_ms,
                   sc, len(body), med, mean, p90, sat, (target or '').split('/')[-1], note))
        f.flush()
        if n % 20 == 0 or note:
            print('  %4d %s cyc=%6.0f 通知=%5.0f(%d回) 取得=%5.0f http=%d %s %s'
                  % (n, time.strftime('%H:%M:%S'), cyc, notify_ms, polls, fetch_ms, sc,
                     (target or '').split('/')[-1], note))
        # 生存判定: 通知が来ない/取得できないが3コマ続いたら中断(叩き続けない)
        stuck = stuck + 1 if note else 0
        if stuck >= 3:
            print('  !! 撮影が停止したと判断して中断(コマ%d %s)' % (n, time.strftime('%H:%M:%S')))
            break
        rest = a.interval - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)
    f.close()
    print('\n終了: %dコマ 失敗%d → %s' % (n, ng, a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
