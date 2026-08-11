# -*- coding: utf-8 -*-
# サムネイル測光の連続テスト(その2・2026-08-11)。ファイル名を**予測しない**版。
#
# 【なぜ予測をやめたか】連番予測はカード接触が1回で済むが破綻する条件が多い。
#   ・DCF のフォルダ繰り上がり(100CANON → 101CANON)
#   ・ファイル番号の「オートリセット」設定でカード交換時に 0001 へ戻る
#   ・9999 での折り返し
#   ・複数カード/記録先の切り替え
#  一覧を引く手間より、名前を外して 404 を踏むリスクのほうが高い(404 の連打は R10 の
#  撮影エンジンを固めた実績がある: 2026-08-11)。そこで毎コマ一覧から最新を求める。
#
# 【方式】露光終了 + settle 待ってから
#   ① dir?kind=number       … 総数とページ数
#   ② dir?kind=list&page=末尾 … 末尾ページのファイル名
#   ③ 最新の**1つ前**の ?kind=thumbnail   ← 生成中のファイルには触らない
#  ディレクトリの解決(contents → card → dir)はキャッシュし、総数が増えなくなったら
#  フォルダ繰り上がりを疑って解決し直す。
#
# 使い方: python thumb_latest.py --ip 192.168.1.11 --minutes 45 --interval 15 --settle 2.5
import argparse, io, os, re, sys, time, json, statistics as st
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(B)
from PIL import Image

SET = '/ccapi/ver100/shooting/settings/'
IMG_RE = re.compile(r'\.(CR3|CR2|JPG|JPEG|HIF)$', re.I)


def s2l(x): return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def lum(b):
    i = b.find(b'\xff\xd8\xff')
    im = Image.open(io.BytesIO(b[i:] if i >= 0 else b)).convert('L')
    h = im.histogram(); t = float(sum(h))
    c = 0.0; med = 1.0
    for k in range(256):
        if c + h[k] >= t / 2:
            med = (k + ((t / 2 - c) / h[k] if h[k] else 0)) / 255.0; break
        c += h[k]
    cc = 0; p90 = 1.0
    for k in range(256):
        cc += h[k]
        if cc >= t * 0.90: p90 = k / 255.0; break
    return s2l(med), sum(s2l(k / 255.0) * h[k] for k in range(256)) / t, p90, h[255] / t


class Finder:
    """撮影ディレクトリをキャッシュしつつ、毎コマ一覧から最新2件を求める。"""

    def __init__(self, cam):
        self.cam = cam
        self.dirs = []          # 撮影ディレクトリ(古い順)
        self.cur = None         # いま使っているディレクトリ
        self.reqs = 0           # このコマで撮影ディレクトリへ投げた回数

    def _get(self, url):
        """503(記録中)だけ短く待ち直す。404 は待っても出てこないので即返す。"""
        for _ in range(4):
            s, b, _ms = self.cam.get(url)
            self.reqs += 1
            if s != 503: return s, b
            time.sleep(0.5)
        return 503, b''

    def resolve(self):
        """contents → カード → 撮影ディレクトリ を引き直す(初回とフォルダ繰り上がり時のみ)。"""
        s, b = self._get(self.cam.p['contents'])
        if s != 200: return False, 'contents(%d)' % s
        cards = json.loads(b.decode())['path']
        if not cards: return False, 'nocard'
        s, b = self._get(cards[-1])          # 記録先が複数なら最後のカード
        if s != 200: return False, 'card(%d)' % s
        self.dirs = json.loads(b.decode())['path']
        if not self.dirs: return False, 'nodir'
        self.cur = self.dirs[-1]             # 名前順で最後 = 最新のフォルダ
        return True, ''

    def latest_two(self):
        """(最新, 1つ前) を返す。フォルダ繰り上がり直後は前のフォルダへ遡る。"""
        if self.cur is None:
            ok, err = self.resolve()
            if not ok: return None, None, err
        for attempt in (0, 1):
            s, b = self._get(self.cur + '?kind=number')
            if s != 200:
                if attempt: return None, None, 'number(%d)' % s
                ok, err = self.resolve()
                if not ok: return None, None, err
                continue
            j = json.loads(b.decode())
            n = j.get('contentsnumber', 0); pages = max(1, j.get('pagenumber', 1))
            if n <= 0:
                # 空のフォルダ = 繰り上がった直後。引き直して1つ前のフォルダを見る。
                if attempt: return None, None, 'empty'
                ok, err = self.resolve()
                if not ok: return None, None, err
                continue
            s, b = self._get('%s?kind=list&page=%d' % (self.cur, pages))
            if s != 200: return None, None, 'list(%d)' % s
            lst = [x for x in json.loads(b.decode())['path'] if IMG_RE.search(x)]
            if len(lst) >= 2: return lst[-1], lst[-2], ''
            if len(lst) == 1:
                # 末尾ページに1件しかない。原因は2つあり、どちらも実際に起きる。
                #  ・総数が100の倍数+1 になった直後(ページ境界)  → 1つ前は**前のページ**の末尾
                #  ・フォルダ繰り上がり直後(100CANON→101CANON) → 1つ前は**前のフォルダ**の末尾
                # 2026-08-11 の R100 実測では前者(総数301→page4に1件)で、フォルダは1つのままだった。
                if pages > 1:
                    s2, b2 = self._get('%s?kind=list&page=%d' % (self.cur, pages - 1))
                    if s2 == 200:
                        prev_page = [x for x in json.loads(b2.decode())['path'] if IMG_RE.search(x)]
                        if prev_page: return lst[-1], prev_page[-1], ''
                prev = self.prev_dir_last()
                return lst[-1], prev, ('' if prev else 'prevdir')
            if attempt: return None, None, 'nofile'
            ok, err = self.resolve()
            if not ok: return None, None, err
        return None, None, 'giveup'

    def prev_dir_last(self):
        if len(self.dirs) < 2: return None
        d = self.dirs[-2]
        s, b = self._get(d + '?kind=number')
        if s != 200: return None
        pages = max(1, json.loads(b.decode()).get('pagenumber', 1))
        s, b = self._get('%s?kind=list&page=%d' % (d, pages))
        if s != 200: return None
        lst = [x for x in json.loads(b.decode())['path'] if IMG_RE.search(x)]
        return lst[-1] if lst else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.11')
    ap.add_argument('--minutes', type=float, default=45.0)
    ap.add_argument('--ss', default='8"')
    ap.add_argument('--iso', default='1600')
    ap.add_argument('--av', default='f1.4')
    ap.add_argument('--interval', type=float, default=15.0)
    ap.add_argument('--settle', type=float, default=2.5)
    ap.add_argument('--out', default='soak_latest.csv')
    a = ap.parse_args()

    cam = B.Cam(a.ip); B.setup(cam, verbose=False)
    for k, v in (('iso', a.iso), ('av', a.av), ('tv', a.ss)):
        cam.put(SET + k, {'value': v})
    time.sleep(0.5)
    expo_s = float(a.ss.rstrip('"')) if a.ss.rstrip('"').replace('.', '').isdigit() else 0.0
    fd = Finder(cam)
    ok, err = fd.resolve()
    print('ディレクトリ: %s (%s)  露光%s 周期%.0fs 待ち%.1fs 予定%.0f分'
          % (fd.cur, err or 'ok', a.ss, a.interval, a.settle, a.minutes))

    f = open(a.out, 'w', encoding='utf-8', newline='')
    f.write('frame,wallclock,cycle_ms,post_ms,lv_ms,find_ms,dirreq,fetch_ms,http,bytes,'
            'medY,meanY,p90,sat,file,note\n')
    t_end = time.time() + a.minutes * 60
    n = 0; ng = 0
    # 撮影が止まったのに叩き続けない。ファイルが増えない/LVが応答しないが続いたら中断する。
    stuck = 0; last_file = None
    while time.time() < t_end:
        t0 = time.perf_counter()
        _s, post_ms = B.shoot(cam)
        lv_ms, _ = B.wait_busy(cam, limit_ms=20000)
        rest = expo_s + a.settle - (time.perf_counter() - t0)
        if rest > 0: time.sleep(rest)

        fd.reqs = 0
        t1 = time.perf_counter()
        newest, prev, err = fd.latest_two()
        find_ms = (time.perf_counter() - t1) * 1000.0
        sc = 0; body = b''; fetch_ms = 0.0; note = err
        med = mean = p90 = sat = -1.0
        name = ''
        if prev:
            name = prev.split('/')[-1]
            t2 = time.perf_counter()
            for _ in range(4):
                sc, body, _ms = cam.get(prev + '?kind=thumbnail')
                if sc != 503: break
                time.sleep(0.5)
            fetch_ms = (time.perf_counter() - t2) * 1000.0
            if sc == 200:
                try: med, mean, p90, sat = lum(body)
                except Exception as e: note = 'decode:%s' % type(e).__name__
            else:
                ng += 1; note = 'fetch(%d)' % sc
        else:
            ng += 1; note = note or 'noprev'
        n += 1
        cyc = (time.perf_counter() - t0) * 1000.0
        f.write('%d,%s,%.0f,%.0f,%.0f,%.0f,%d,%.0f,%d,%d,%.5f,%.5f,%.3f,%.4f,%s,%s\n'
                % (n, time.strftime('%H:%M:%S'), cyc, post_ms, lv_ms, find_ms, fd.reqs,
                   fetch_ms, sc, len(body), med, mean, p90, sat, name, note))
        f.flush()
        if n % 20 == 0 or note:
            print('  %4d %s cyc=%6.0f 探索=%5.0f(%d回) 取得=%5.0f http=%d %s %s'
                  % (n, time.strftime('%H:%M:%S'), cyc, find_ms, fd.reqs, fetch_ms, sc, name, note))
        # 生存判定: 最新ファイルが進まない or LV が応答しない が3コマ続いたら中断。
        if newest == last_file or lv_ms < 0: stuck += 1
        else: stuck = 0
        last_file = newest
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
