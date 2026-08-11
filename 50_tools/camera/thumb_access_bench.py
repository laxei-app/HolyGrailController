# -*- coding: utf-8 -*-
# EOS R10 で「撮影画像のサムネイルを取りに行く方法」を比べる実験(2026-08-11)。
#
# 【なぜ要るか】
#  夜明けの露出追従が遅れる真因は、ライブビューが長秒露光(8秒)を再現できず、実写より
#  2〜4.6段暗く見えることだった(2026-08-09 実写と突合で確定)。統計量の付け替えでは
#  直らないので、撮影画像そのもの(サムネイル)から測光したい。
#  ところが以前の試作では R10 が27分で固着してネットワークから消えた。原因はサムネイル
#  取得そのものではなく、**取りに行くまでにカードへ3回触ること**だった(一覧→総数→最新名)。
#  記録中のカード接触が R10 の撮影エンジンを固めることは 2026-07-31 にも観測している。
#
# 【何を測るか】
#  busy = 露光終了 → 測光できる体裁で応答が返るまで[ms]。
#         アプリと同じ判定を使う(shooting/liveview/flipdetail?kind=info の先頭が
#         ff 00 01 + 長さ、で本文が足りているか。apiCanonCCAPI.cpp checkLiveViewInfo)。
#  これがアクセス方法ごとにどれだけ伸びるかを比べる。伸びなければ撮影周期を詰められる。
#
# 【使い方】
#   python thumb_access_bench.py --ip 192.168.1.11 --method M0 --frames 20 --interval 8
#   python thumb_access_bench.py --ip 192.168.1.11 --all --frames 15 --interval 8
import argparse, json, os, re, statistics as st, sys, time
import urllib.request, urllib.error

sys.stdout.reconfigure(encoding='utf-8')

# ---------------------------------------------------------------- HTTP
# アプリはセッションを使い回す(1リクエスト1接続にすると R10 が connect 詰まりを起こした
# 経緯がある: 2026-08-03)。ここでも keep-alive を使う。
import http.client


class Cam:
    def __init__(self, ip, port=8080):
        self.ip = ip; self.port = port
        self.conn = None
        self.base = None

    def _c(self):
        if self.conn is None:
            self.conn = http.client.HTTPConnection(self.ip, self.port, timeout=15)
        return self.conn

    def reset(self):
        try:
            if self.conn: self.conn.close()
        except Exception:
            pass
        self.conn = None

    def req(self, method, path, body=None, ctype='application/json'):
        """(status, bytes, elapsed_ms) を返す。失敗は (0, b'', ms)。"""
        t0 = time.perf_counter()
        for attempt in (0, 1):
            try:
                c = self._c()
                hdr = {'Connection': 'keep-alive'}
                if body is not None: hdr['Content-Type'] = ctype
                c.request(method, path, body=body, headers=hdr)
                r = c.getresponse()
                data = r.read()
                return r.status, data, (time.perf_counter() - t0) * 1000.0
            except Exception:
                self.reset()
                if attempt: return 0, b'', (time.perf_counter() - t0) * 1000.0
        return 0, b'', (time.perf_counter() - t0) * 1000.0

    def get(self, path):  return self.req('GET', path)
    def post(self, path, obj): return self.req('POST', path, json.dumps(obj))
    def put(self, path, obj):  return self.req('PUT',  path, json.dumps(obj))


# ---------------------------------------------------------------- 判定
def lv_ready(body: bytes) -> bool:
    """apiCanonCCAPI.cpp checkLiveViewInfo と同じ判定。"""
    if len(body) < 10: return False
    if body[0] != 0xff or body[1] != 0x00 or body[2] != 0x01: return False
    ln = (body[3] << 24) | (body[4] << 16) | (body[5] << 8) | body[6]
    return ln <= len(body) - 9


# ---------------------------------------------------------------- 準備
def setup(cam, verbose=True, liveview=True):
    st_, b, _ = cam.get('/ccapi')
    if st_ != 200: raise SystemExit('CCAPI に接続できません')
    api = json.loads(b.decode())
    paths = {}
    for ver, eps in api.items():
        for e in eps:
            paths.setdefault(e['path'].split('/ccapi/')[1].split('/', 1)[1], e['path'])
    cam.p = paths
    if verbose:
        print('  接続 OK')
    # オートパワーオフ抑止(撮影中は切らせない)
    for key in ('functions/autopoweroff',):
        if key in paths:
            cam.put(paths[key], {'value': 'disable'})
    lv = paths.get('shooting/liveview')
    if liveview:
        # ライブビュー開始(busy 判定に使う)
        for disp in ('keep', 'on', 'off'):
            s, _, _ = cam.post(lv, {'liveviewsize': 'small', 'cameradisplay': disp})
            if s in (200, 201): break
    else:
        # 明示的に止める。サムネイル測光ではライブビューは不要で、アプリも
        # liveViewNeededWhileCapturing が false なら releaseLiveView で離す(captureRunner.cpp:210)。
        # 長秒露光を繰り返す間ライブビューを掴み続けるのが R10 の停止に効いている疑いがあるため、
        # 通常運用と同じ「離した」状態で確かめる(2026-08-11)。
        cam.post(lv, {'liveviewsize': 'off', 'cameradisplay': 'on'})
    return paths


def shoot(cam):
    """シャッターを切り、露光が終わる(=POSTが返る)までを待つ。"""
    p = cam.p['shooting/control/shutterbutton']
    t0 = time.perf_counter()
    s, b, _ = cam.post(p, {'af': False})
    return s, (time.perf_counter() - t0) * 1000.0


def wait_busy(cam, limit_ms=15000, max_tries=20):
    """露光終了→測光できる体裁になるまで[ms]。アプリの busy と同じ意味。

    【連打しないこと】カメラが応答しなくなったとき、以前は 50ms おきに上限まで叩き続け、
    1コマあたり約400回のリクエストを浴びせていた(2026-08-11)。壊れた原因ではないが、
    壊れた後の状態を悪化させ「電源を入れ直さないと戻らない」状況を作った疑いがある。
    回数を絞り、間隔も広げていく。
    """
    p = cam.p['shooting/liveview/flipdetail'] + '?kind=info'
    t0 = time.perf_counter()
    tries = 0
    wait = 0.05
    while tries < max_tries and (time.perf_counter() - t0) * 1000.0 < limit_ms:
        tries += 1
        s, b, _ = cam.get(p)
        if s == 200 and lv_ready(b):
            return (time.perf_counter() - t0) * 1000.0, tries
        time.sleep(wait)
        wait = min(wait * 1.6, 1.0)          # 50ms → 80 → 128 … 最大1秒
    return -1.0, tries


# ---------------------------------------------------------------- 取得方法
def latest_path_by_listing(cam):
    """現行実装と同じ手順: 一覧 → 総数 → 最新名。カードに3回触る。"""
    root = cam.p['contents']
    s, b, _ = getRetry(cam, root)
    if s != 200: return None, 'contents(%d)' % s
    card = json.loads(b.decode())['path'][0]
    s, b, _ = getRetry(cam, card)
    if s != 200: return None, 'card(%d)' % s
    dirs = json.loads(b.decode())['path']
    if not dirs: return None, 'nodir'
    d = dirs[-1]
    s, b, _ = getRetry(cam, d + '?kind=number')
    if s != 200: return None, 'number(%d)' % s
    n = json.loads(b.decode()).get('contentsnumber', 0)
    if n <= 0: return None, 'empty'
    page = max(1, (n + 99) // 100)
    s, b, _ = getRetry(cam, '%s?kind=list&page=%d' % (d, page))
    if s != 200: return None, 'list(%d)' % s
    lst = json.loads(b.decode())['path']
    return (lst[-1] if lst else None), None


def getRetry(cam, url, tries=6, wait=0.05):
    """503(記録中)は待って取り直す。撮影直後のカードは約130ms 塞がる(2026-08-11 実測)。"""
    n503 = 0
    for _ in range(tries):
        s, b, ms = cam.get(url)
        if s != 503: return s, b, n503
        n503 += 1
        time.sleep(wait)
    return 503, b'', n503


def fetch(cam, path, kind):
    t0 = time.perf_counter()
    s, b, n = getRetry(cam, '%s?kind=%s' % (path, kind))
    return s, len(b), (time.perf_counter() - t0) * 1000.0


# ---------------------------------------------------------------- 各方式
def run(cam, method, frames, interval, warm_path=None):
    rows = []
    known = warm_path
    seq = None
    if known:
        m = re.search(r'(\d+)(\.[A-Za-z0-9]+)$', known)
        if m: seq = int(m.group(1))
    for i in range(frames):
        cyc0 = time.perf_counter()
        s, shot_ms = shoot(cam)
        busy_ms, busy_try = wait_busy(cam)
        acc_ms = 0.0; acc_note = ''; size = 0
        t0 = time.perf_counter()
        if method == 'M0':
            acc_note = '取得なし'
        elif method == 'M1':                      # 現行: 一覧→総数→最新名→サムネ
            p, err = latest_path_by_listing(cam)
            if p is None: acc_note = 'NG:' + str(err)
            else:
                sc, size, _ = fetch(cam, p, 'thumbnail')
                acc_note = 'ok' if sc == 200 else 'NG:%d' % sc
                known = p
        elif method == 'M2':                      # 連番予測 → サムネ直接(カード接触1回)
            if known is None:
                known, err = latest_path_by_listing(cam)
                acc_note = 'seed'
            else:
                m = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', known)
                nxt = '%s%0*d%s' % (m.group(1), len(m.group(2)), int(m.group(2)) + 1, m.group(3))
                sc, size, _ = fetch(cam, nxt, 'thumbnail')
                if sc == 200: known = nxt; acc_note = 'ok'
                else:
                    acc_note = 'miss:%d' % sc
                    known, _ = latest_path_by_listing(cam)   # 外したら並べ直す
        elif method == 'M3':                      # kind=info だけ(画像を取らない)
            if known is None:
                known, _ = latest_path_by_listing(cam)
            m = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', known)
            nxt = '%s%0*d%s' % (m.group(1), len(m.group(2)), int(m.group(2)) + 1, m.group(3))
            sc, size, _ = fetch(cam, nxt, 'info')
            if sc == 200: known = nxt; acc_note = 'ok'
            else: acc_note = 'miss:%d' % sc; known, _ = latest_path_by_listing(cam)
        elif method == 'M5':                      # カードが再開するまでの待ち時間を測る
            root = cam.p['contents']
            t1 = time.perf_counter(); n503 = 0; sc = 0
            while (time.perf_counter() - t1) < 25.0:
                sc, bb, _ = cam.get(root)
                if sc == 200: break
                n503 += 1
                time.sleep(0.1)
            acc_note = ('card再開 %.0fms (503x%d)' % ((time.perf_counter() - t1) * 1000.0, n503)) if sc == 200 else 'card再開せず'
        elif method in ('M6', 'M7'):
            # N コマ前のサムネを取る。8秒露光では書き込みに8.3秒かかり次コマに間に合わないので、
            # 「今書いている最新」ではなく確実に書き終わっている過去のコマを狙う(M6=1つ前 / M7=2つ前)。
            # 露出制御は15秒周期なので1〜2コマ遅れは実害にならない。
            back = 1 if method == 'M6' else 2
            if known is None:
                known, err = latest_path_by_listing(cam)
                acc_note = 'seed:%s' % (err or 'ok')
            else:
                mm = re.match(r'(.*?)(\d+)(\.[A-Za-z0-9]+)$', known)
                seqn = int(mm.group(2))
                tgt = '%s%0*d%s' % (mm.group(1), len(mm.group(2)), seqn - back + 1, mm.group(3))
                sc, size, _ = fetch(cam, tgt, 'thumbnail')
                acc_note = ('ok(%dコマ前)' % back) if sc == 200 else 'NG:%d' % sc
                known = '%s%0*d%s' % (mm.group(1), len(mm.group(2)), seqn + 1, mm.group(3))
        acc_ms = (time.perf_counter() - t0) * 1000.0
        rows.append(dict(i=i + 1, shot=shot_ms, busy=busy_ms, btry=busy_try,
                         acc=acc_ms, size=size, note=acc_note))
        print('  %3d  shot=%6.0f  busy=%6.0f(try%2d)  取得=%6.0f  %7dB  %s'
              % (i + 1, shot_ms, busy_ms, busy_try, acc_ms, size, acc_note))
        rest = interval - (time.perf_counter() - cyc0)
        if rest > 0: time.sleep(rest)
    return rows, known


def summarize(name, rows):
    ok = [r for r in rows if r['busy'] >= 0]
    if not ok:
        print('  %-4s : 全滅' % name); return
    b = [r['busy'] for r in ok]; a = [r['acc'] for r in rows]
    ng = sum(1 for r in rows if r['note'].startswith('NG') or r['note'].startswith('miss'))
    print('  %-4s  busy 中央%6.0f 平均%6.0f 最大%6.0f  |  取得 中央%6.0f 最大%6.0f  |  失敗%d/%d'
          % (name, st.median(b), sum(b) / len(b), max(b), st.median(a), max(a), ng, len(rows)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ip', default='192.168.1.11')
    ap.add_argument('--method', default='M0')
    ap.add_argument('--frames', type=int, default=20)
    ap.add_argument('--interval', type=float, default=8.0)
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--out', default='')
    a = ap.parse_args()

    cam = Cam(a.ip)
    setup(cam)
    NAMES = {'M0': '取得なし(基準)', 'M1': '現行(一覧→総数→最新名→サムネ)',
             'M2': '連番予測→サムネ', 'M3': '連番予測→info のみ', 'M4': '連番予測→display',
             'M5': 'カード再開待ちの計測', 'M6': '1コマ前のサムネ', 'M7': '2コマ前のサムネ'}
    methods = ['M0', 'M1', 'M2', 'M3', 'M4'] if a.all else a.method.split(',')
    allrows = {}
    known = None
    for m in methods:
        print('\n===== %s : %s =====' % (m, NAMES[m]))
        rows, known = run(cam, m, a.frames, a.interval, known)
        allrows[m] = rows
    print('\n===== まとめ (すべて ms) =====')
    for m in methods: summarize(m, allrows[m])
    if a.out:
        json.dump(allrows, open(a.out, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        print('\n保存: %s' % a.out)


if __name__ == '__main__':
    main()
