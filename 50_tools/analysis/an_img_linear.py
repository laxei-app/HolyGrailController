# -*- coding: utf-8 -*-
# 実写画像(CR3の埋め込み全画素JPEG)から、アプリの測光と「同一手順」でリニア輝度を出す。
#   アプリ: x = histMedian(LVヒスト輝度256bin) ; Y = srgbToLinear(x)
#   ここ  : x = histMedian(実写画像の輝度256binヒスト) ; Y = srgbToLinear(x)
# これでライブビュー由来のYと実写由来のYを同じ土俵で比べる。
# 注意: CR3のRAW(センサー線形)ではなくカメラ現像後JPEGなのでピクチャースタイルのトーンカーブが乗る。
#       LVヒストも同じ現像系なので比較としては整合するが、真のセンサー線形ではない。
import subprocess, sys, os, io, math
from PIL import Image

ET = r"C:\Program Files\exiftool-13.36_64\exiftool.exe"
# 画像はリポジトリ追跡外の _picture に置く運用。スクリプト位置(50_tools/analysis)から相対で求める。
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PDIR = os.path.join(REPO, "_picture")

def srgb_to_linear(x):
    if x <= 0.04045:
        return x / 12.92
    return ((x + 0.055) / 1.055) ** 2.4

def hist_median(bins):
    # exposureMath.cpp histMedian と同じ: 累積が半分を超える bin を線形補間し pos/(n-1)
    n = len(bins)
    total = float(sum(bins))
    if total <= 0:
        return 0.0
    half = total / 2.0
    cum = 0.0
    for k in range(n):
        c = float(bins[k])
        if cum + c >= half:
            frac = (half - cum) / c if c > 0 else 0.0
            return (k + frac) / float(n - 1)
        cum += c
    return 1.0

def analyze(path):
    # 埋め込み全画素JPEGを取り出す
    jpg = subprocess.run([ET, "-b", "-JpgFromRaw", path], capture_output=True).stdout
    if len(jpg) < 1000:
        jpg = subprocess.run([ET, "-b", "-PreviewImage", path], capture_output=True).stdout
    if len(jpg) < 1000:
        return None
    im = Image.open(io.BytesIO(jpg)).convert("L")   # ITU-R 601-2 luma
    h = im.histogram()                               # 256 bins
    x = hist_median(h)
    y = srgb_to_linear(x)
    # 参考: 飽和画素率(255付近)と平均
    total = sum(h)
    clip = sum(h[250:]) / total if total else 0
    mean = sum(i * h[i] for i in range(256)) / total / 255.0 if total else 0
    return dict(px=im.size, med_srgb=x, linear=y, clip=clip, mean_srgb=mean)

if __name__ == "__main__":
    for arg in sys.argv[1:]:
        cam, name = arg.split("/", 1)
        p = os.path.join(PDIR, cam, name)
        r = analyze(p)
        if r is None:
            print(f"{name}: 抽出失敗")
            continue
        print(f"{name}  中央値(sRGB)={r['med_srgb']:.4f}  リニアY={r['linear']:.4f}  "
              f"平均(sRGB)={r['mean_srgb']:.3f}  飽和率={r['clip']*100:.1f}%  {r['px']}")
