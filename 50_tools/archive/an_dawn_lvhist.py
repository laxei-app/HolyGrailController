# 夜明けの露出制御 解析: SHOT(露出設定+実測ev) と LVHIST(p99/pMx) を突き合わせる。
#   狙い: 薄明でライブビューが「明るい画素」を見えているか(pMx上昇)を、
#         中央値Yが暗いまま(=測光統計の選び方の問題)かどうかと分離する。
# 使い方: python an_dawn_lvhist.py <log> [開始HH:MM] [終了HH:MM]
import sys, re, io

SHOT = re.compile(
    r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|\w+\|SHOT\s*\|\s*(\d+)\|\s*(\d+)\|([^|]*)\|([^|]*)\|\s*([-+0-9.]+)\|(.*)$')
LVH = re.compile(
    r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|\w+\|LVHIST\s*\|\s*fr=(\d+) Y=([0-9.]+) p99=([0-9.-]+) pMx=([0-9.-]+)')


def load(path):
    for enc in ('utf-8', 'cp932', 'utf-8-sig'):
        try:
            with io.open(path, encoding=enc, errors='strict') as f:
                return f.readlines()
        except UnicodeDecodeError:
            continue
    with io.open(path, encoding='utf-8', errors='replace') as f:
        return f.readlines()


def main():
    path = sys.argv[1]
    t0 = sys.argv[2] if len(sys.argv) > 2 else '00:00'
    t1 = sys.argv[3] if len(sys.argv) > 3 else '23:59'

    shots, lvh = {}, {}
    for ln in load(path):
        ln = ln.rstrip('\n')
        m = SHOT.match(ln)
        if m:
            d, t, fr, iso, tv, av, ev, rest = m.groups()
            if t0 <= t[:5] <= t1:
                shots[int(fr)] = dict(t=t, iso=iso, tv=tv.strip(), av=av.strip(),
                                      ev=float(ev), mode=rest.split()[0] if rest.split() else '')
            continue
        m = LVH.match(ln)
        if m:
            d, t, fr, y, p99, pmx = m.groups()
            lvh[int(fr)] = dict(t=t, y=float(y), p99=float(p99), pmx=float(pmx))

    frames = sorted(set(shots) | set(lvh))
    if not frames:
        print('該当フレーム無し'); return

    print('  時刻    frame   ISO  シャッター   F     ev     Y(中央) p99   pMax  モード')
    print('-' * 88)
    prev = None
    for fr in frames:
        s, l = shots.get(fr), lvh.get(fr)
        t = (s or l)['t']
        iso = s['iso'] if s else '   -'
        tv = s['tv'] if s else '-'
        av = s['av'] if s else '-'
        ev = '%+7.3f' % s['ev'] if s else '      -'
        y = '%.4f' % l['y'] if l else '  -   '
        p99 = '%.3f' % l['p99'] if l else '  -  '
        pmx = '%.3f' % l['pmx'] if l else '  -  '
        mode = s['mode'] if s else ''
        mark = ''
        if s and prev and (s['iso'], s['tv']) != prev:
            mark = '  <<< 露出変更'
        if s:
            prev = (s['iso'], s['tv'])
        print('%s %6d %5s %-11s %-5s %s  %s  %s %s %-8s%s'
              % (t, fr, iso, tv, av, ev, y, p99, pmx, mode, mark))

    # 判定材料のサマリ
    ls = [l for f, l in sorted(lvh.items())]
    if ls:
        n = max(1, len(ls) // 10)
        print('\n--- LVHIST 推移(前半/後半の代表値) ---')
        print('最初%d件 平均: Y=%.4f p99=%.3f pMx=%.3f' %
              (n, sum(x['y'] for x in ls[:n]) / n,
               sum(x['p99'] for x in ls[:n]) / n,
               sum(x['pmx'] for x in ls[:n]) / n))
        print('最後%d件 平均: Y=%.4f p99=%.3f pMx=%.3f' %
              (n, sum(x['y'] for x in ls[-n:]) / n,
               sum(x['p99'] for x in ls[-n:]) / n,
               sum(x['pmx'] for x in ls[-n:]) / n))
        print('\n判定: pMxが薄明で上がらない → カメラのLVが長秒露光を映せていない(測光統計では救えない)')
        print('      pMxは上がるがYが暗いまま → 測光統計(中央値)の選び方の問題=ソフトで直せる')


if __name__ == '__main__':
    main()
