# -*- coding: utf-8 -*-
# 実写(CR3)の埋め込み全画素JPEGから**リニア輝度**を測り、1コマ1行の TSV にする。
#
# 【なぜ必要か】撮影制御方法(自動露出)が「絵の明るさ」をどれだけ一定に保てているかは、
#  ログの測光値(Y)だけでは分からない。測光はライブビュー/サムネイルの統計量であって
#  出来上がった写真そのものではないため。実写から同じ土俵の数字を採って突き合わせる。
#
# 【測っているもの】
#   Ymean … 各binをリニア化してから加重平均した、画面全体のリニア輝度
#   Ymed  … 256binヒストの中央値(exposureMath の histMedian と同じ手順)をリニア化。
#            アプリの測光と同じ統計量なので、ログのYと直接比べられる
#   sat   … sRGB 250以上の画素の割合(白飛びの目安)
#   Bv    … 露出を外した被写体側の明るさ[段] = log2(Y) + log2(F^2/ss) - log2(ISO/100)
#
# 【重要な注意】これは**センサーRAWではなくカメラ現像後のJPEG**。ピクチャースタイルの
#  トーンカーブが残るので、露出1目盛りに対して画像は 1.3 倍ほど増幅されて見える
#  (R10 実測 1.28〜1.33倍)。段数を語るときはこの倍率で割ること。倍率は
#  an_shot_report.py が「露出1段あたりの画像の応答」として毎回実測して出す。
#  真のセンサーリニアが要るなら rawpy を入れて別途測ること。
#
# 使い方:
#   python an_shot_linear.py "<画像フォルダ>" <出力TSV>
# 例:
#   python an_shot_linear.py "_picture/2026.09.02(朝)/R10 シグモイド" img_0902.tsv
import sys, io, os, math

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cr3_read
from PIL import Image

if len(sys.argv) < 3:
    print(__doc__ or "usage: an_shot_linear.py <画像フォルダ> <出力TSV>")
    raise SystemExit(1)
SRC, OUT = sys.argv[1], sys.argv[2]


def s2l(x):
    """sRGB伝達関数を外す(0..1)。"""
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


LIN = [s2l(k / 255.0) for k in range(256)]   # 256段ぶん先に作っておく


def hist_median(bins):
    """exposureMath.cpp の histMedian と同じ: 累積が半分を超えるbinを線形補間し pos/(n-1)。"""
    total = float(sum(bins))
    if total <= 0:
        return 0.0
    half, cum = total / 2.0, 0.0
    for k in range(256):
        c = float(bins[k])
        if cum + c >= half:
            return (k + ((half - cum) / c if c > 0 else 0.0)) / 255.0
        cum += c
    return 1.0


names = sorted(f for f in os.listdir(SRC) if f.lower().endswith(".cr3"))
if not names:
    print("CR3 が見つかりません:", SRC)
    raise SystemExit(1)

w = io.open(OUT, "w", encoding="utf-8", newline="\n")
w.write("file\ttime\tiso\tss\tfn\tYmean\tYmed\tsat\tBv\n")
for i, nm in enumerate(names):
    try:
        ex, jpg = cr3_read.read(os.path.join(SRC, nm))
        im = Image.open(io.BytesIO(jpg))
        # DCTスケーリングで 1/4 に落として読む。数百コマ回すので全画素は遅すぎる。
        #  中央値・平均とも空間平均なので 1/4 でも値はほぼ変わらない。
        im.draft("L", (im.size[0] // 4, im.size[1] // 4))
        h = im.convert("L").histogram()
        n = float(sum(h))
        ymean = sum(h[k] * LIN[k] for k in range(256)) / n
        ymed = s2l(hist_median(h))
        sat = sum(h[250:]) / n
        iso = float(ex.get(0x8827) or 0)     # ISO
        ss = float(ex.get(0x829A) or 0)      # 露光時間[秒]
        fn = float(ex.get(0x829D) or 0)      # F値
        t = str(ex.get(0x9003) or "")        # 撮影時刻 "YYYY:MM:DD hh:mm:ss"
        bv = ""
        if ymean > 0 and iso > 0 and ss > 0 and fn > 0:
            bv = "%.4f" % (math.log2(ymean) + math.log2(fn * fn / ss) - math.log2(iso / 100.0))
        w.write("%s\t%s\t%g\t%g\t%g\t%.6f\t%.6f\t%.5f\t%s\n"
                % (nm, t, iso, ss, fn, ymean, ymed, sat, bv))
    except Exception as e:
        w.write("%s\tERR\t\t\t\t\t\t\t%s\n" % (nm, e))
    if i % 100 == 0:
        w.flush()
        print(i, nm, flush=True)
w.close()
print("done", len(names), "->", OUT)
