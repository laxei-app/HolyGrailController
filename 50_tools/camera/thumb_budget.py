# -*- coding: utf-8 -*-
# 「サムネイル取得の回数だけ」で撮影が死ぬのかを、撮影せずに最速で確かめる(2026-08-12)。
#
# 【背景】a/b交互テストで、EOS R10 は**サムネイル取得238回目**で撮影が死ぬと確定した。
#  撮影回数(439回)・ライブビュー取得(400回)・カード枚数・周期・待ちはすべて無関係で、
#  HTTP を切って張り直しても予算は戻らない(電源投入からの通算)。
#  ただしこれまでの走行はすべて「8秒露光の撮影とセット」で、撮影抜きでも減るのかは未確認。
#
# 【やること】撮影せずにカード上の既存ファイルのサムネイルを取り続け、
#  一定回数ごとにシャッターを1回だけ試して**まだ撮れるか**を確かめる。
#  死んだら、そのときの取得回数を報告する。
#
#   ・死ぬ  → 取得回数だけで決まる。以後の検証が1時間→1分になる
#   ・死なない → 撮影とセットでないと減らない。それ自体が手がかり
#
# 【生死の判定】情報系(deviceinformation/temperature/battery)は死んでも 200 を返すので
#  使ってはいけない。シャッターを切って**ファイルが実際に増えたか**で判定する(2026-08-12)。
#
# 使い方: python thumb_budget.py --ip 192.168.1.4
import argparse, json, os, re, sys, time
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_b = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_b); _b.loader.exec_module(B)

SET = '/ccapi/ver100/shooting/settings/'
ADDED = re.compile(r'"addedcontents"\s*:\s*\[(.*?)\]', re.S)


def probe(cam, p, expo, maxpoll=6):
    """シャッターを1回切り、ファイルが増えたかで生死を見る。(生きてる?, 説明) を返す。"""
    s, ms = B.shoot(cam)
    if s not in (200, 201):
        return False, 'シャッター %d (%.0fms)' % (s, ms)
    time.sleep(expo + 0.3)
    for _ in range(maxpoll):
        st, body, _ = cam.get(p['event/polling'] + '?continue=on')
        if st != 200:
            return False, '通知 %d' % st
        mo = ADDED.search(body.decode('utf-8', 'replace'))
        if mo and re.findall(r'"([^"]+)"', mo.group(1)):
            return True, ''
    return False, '通知なし(記録されなかった)'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.4')
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--every', type=int, default=40)   # 何回取得ごとに生死を見るか
    ap.add_argument('--limit', type=int, default=500)  # 取得回数の上限(ここまで死ななければ完走)
    ap.add_argument('--out', default='budget.csv')
    a = ap.parse_args()

    cam = B.Cam(a.ip); p = B.setup(cam, verbose=False, liveview=True)
    cam.put(SET + 'av', {'value': a.av})
    cam.put(SET + 'tv', {'value': a.ss})
    cam.put(SET + 'iso', {'value': a.iso})
    time.sleep(0.5)
    expo = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    cam.get(p['event/polling'] + '?continue=off')

    # 取得対象の一覧を1回だけ引く。一覧も /contents なので、余計に触らないよう最小限にする。
    root = cam.p['contents']
    s, b, _ = B.getRetry(cam, root);  card = json.loads(b.decode())['path'][0]
    s, b, _ = B.getRetry(cam, card);  d = json.loads(b.decode())['path'][-1]
    s, b, _ = B.getRetry(cam, d + '?kind=number')
    total = json.loads(b.decode()).get('contentsnumber', 0)
    s, b, _ = B.getRetry(cam, '%s?kind=list&page=1' % d)
    files = json.loads(b.decode())['path']
    print('%s  カード %d枚 / 一覧1ページ目 %d件 を順に取得する（一覧アクセスは4回だけ）'
          % (a.ip, total, len(files)), flush=True)
    if not files:
        print('取得できるファイルが無い。1枚撮ってから実行してください。', flush=True); return 1

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('fetches,wallclock,fetch_ms,fetch_http,bytes,probe,probe_ms,note\n')

    ok, why = probe(cam, p, expo)
    print('  起点の生死確認: %s %s' % ('生きている' if ok else '**死んでいる**', why), flush=True)
    if not ok:
        print('  最初から撮れない。電源を入れ直してください。', flush=True); return 1

    k = 0; dead_at = -1; why = ''
    t0 = time.perf_counter()
    while k < a.limit:
        path = files[k % len(files)]
        t1 = time.perf_counter()
        s, body, _ = B.getRetry(cam, path + '?kind=thumbnail')
        ms = (time.perf_counter() - t1) * 1000.0
        k += 1
        note = '' if s == 200 else 'fetch(%d)' % s
        pr = ''; pms = 0.0
        if k % a.every == 0 or note:
            t2 = time.perf_counter()
            alive, why2 = probe(cam, p, expo)
            pms = (time.perf_counter() - t2) * 1000.0
            pr = 'OK' if alive else 'NG'
            print('  取得%3d回  %s  取得=%.0fms/%dB  撮影=%s %s'
                  % (k, time.strftime('%H:%M:%S'), ms, len(body), pr, why2), flush=True)
            if not alive:
                dead_at = k; why = why2
        f.write('%d,%s,%.0f,%d,%d,%s,%.0f,%s\n'
                % (k, time.strftime('%H:%M:%S'), ms, s, len(body), pr, pms, note))
        f.flush()
        if dead_at > 0 or note: break
    f.close()
    el = time.perf_counter() - t0
    if dead_at > 0:
        print('\n**撮影が死んだ: サムネイル取得 %d回目（%.0f秒経過・撮影は生死確認の %d回だけ）**'
              % (dead_at, el, dead_at // a.every + 1), flush=True)
        print('  理由: %s' % why, flush=True)
    else:
        print('\n取得 %d回まで撮影は無事（%.0f秒）。取得回数だけでは死なない。' % (k, el), flush=True)
    print('→ %s' % a.out, flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
