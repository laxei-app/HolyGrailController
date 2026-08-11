# -*- coding: utf-8 -*-
# 「サムネイル取得のあと次のシャッターまでの空き」を 4.0秒から 0.5秒刻みで縮めながら、
# 各1時間ずつ回して**どこまで詰められるか**を測る(2026-08-11 夜・無人運転)。
#
# 【なぜ大きい方から縮めるか】小さい方から伸ばすと、途中で固まったらそこで止まってしまい
#  その先(安全側)を試せない。大きい方=安全側から縮めれば、固まった時点が「そこが限界」に
#  なり、それまでの結果がすべて手に入る。カメラが固まると物理操作でしか復帰できないため。
#
# 【実測の前提】
#  ・撮影直後のファイルに触ると固まる → 通知で受けた名前を**1コマ持ち越して**取る
#  ・空き0秒で42コマ、0.5秒で144コマ、約6秒で180コマ完走。境目を探す
#  ・カメラが固まっても必ず戻るよう、Cam に強制打ち切りの見張りを入れた
#
# 固まったら**その場で掃引を止める**。死んだカメラを叩き続けない。
import os, subprocess, sys, time, csv, datetime
sys.stdout.reconfigure(encoding='utf-8')
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
_b = importlib.util.spec_from_file_location('bench', os.path.join(_here, 'thumb_access_bench.py'))
B = importlib.util.module_from_spec(_b); _b.loader.exec_module(B)

IP = sys.argv[1] if len(sys.argv) > 1 else '192.168.1.4'
MIN = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
GAPS = [4.0, 3.5, 3.0, 2.5, 2.0, 1.5, 1.0, 0.5]
OUT = os.path.join(_here, 'soak_out')
os.makedirs(OUT, exist_ok=True)
SUM = os.path.join(OUT, 'gap_sweep_summary.txt')


def log(m):
    line = '%s %s' % (datetime.datetime.now().strftime('%H:%M:%S'), m)
    print(line, flush=True)
    with open(SUM, 'a', encoding='utf-8') as f: f.write(line + '\n')


def alive():
    c = B.Cam(IP, hard=15)
    s, _b, _m = c.get('/ccapi/ver100/deviceinformation')
    c.reset()
    return s == 200


log('=== 空き掃引 開始 ip=%s 各%.0f分 %s ===' % (IP, MIN, GAPS))
for gap in GAPS:
    if not alive():
        log('!! カメラが応答しない。掃引を中止する(空き%.1f秒の手前まで完了)' % gap)
        break
    tag = 'gap%03d' % int(gap * 10)
    csvp = os.path.join(OUT, 'sweep_%s.csv' % tag)
    log('--- 空き%.1f秒 開始 → %s' % (gap, os.path.basename(csvp)))
    # -u で標準出力のバッファリングを切る(前回ログが空になった)
    r = subprocess.run([sys.executable, '-u', os.path.join(_here, 'thumb_tight.py'),
                        '--ip', IP, '--minutes', str(MIN), '--gap', str(gap), '--out', csvp],
                       capture_output=True, text=True, encoding='utf-8', errors='replace',
                       timeout=MIN * 60 + 900)
    # 結果を読む
    n = 0; ng = 0; last = ''
    try:
        rows = list(csv.DictReader(open(csvp, encoding='utf-8')))
        n = len(rows); last = rows[-1]['wallclock'] if rows else ''
        ng = sum(1 for x in rows if x['note'])
    except Exception:
        pass
    # 1コマ = 露光8.0 + 通知0.9 + 取得0.05 + 空き。空きを入れ忘れると常に「停止」と誤判定する。
    want = int(MIN * 60 / (8.95 + gap))
    ok = (n >= want * 0.9) and (ng <= 2)
    log('    空き%.1f秒: %dコマ(期待約%d) 異常%d 最終%s → %s'
        % (gap, n, want, ng, last, '完走' if ok else '**停止**'))
    if not ok:
        log('!! 空き%.1f秒 で停止した。ここが限界。掃引を終了する。' % gap)
        break
    time.sleep(20)                      # 次の条件へ移る前に少し休ませる
log('=== 掃引 終了 ===')
