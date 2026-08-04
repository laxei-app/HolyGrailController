# -*- coding: utf-8 -*-
# CR3(RAW)に埋め込まれたプレビューJPEGから、写真の明るさ(輝度の中央値)を測る。
#
# 【何のためか】ログの測光値 Y は「シャッターの5秒前に、前コマの露出で測った値」なので、
#  実際に撮れた写真の明るさとは一致しない。露出変更分を掛け戻す推定はできるが、
#  ユーザーが見ているのは現像された絵そのもの。推定ではなく実測で突き合わせるために使う。
#
# CR3 は中に複数のJPEGを持つ(小さいサムネ/中間/大きいプレビュー)。
# ここでは2番目(中間サイズ)を使う。小さすぎず、読み込みが速い。
#
# 使い方:
#   python cr3_luma.py <フォルダ> [--from IMG_0571] [--to IMG_0672] [--step 5]
import argparse, glob, os, re, statistics, struct, sys


def embedded_jpegs(path, limit_bytes=3_000_000):
    """CR3 の先頭から JPEG(SOI..EOI) を探して (offset, size) を返す。"""
    with open(path, "rb") as f:
        head = f.read(limit_bytes)
    out = []
    i = 0
    while len(out) < 6:
        i = head.find(b"\xff\xd8\xff", i)
        if i < 0:
            break
        e = head.find(b"\xff\xd9", i)
        if e < 0:
            break
        out.append((i, e + 2 - i))
        i = e + 2
    return out, head


def jpeg_median_luma(data):
    """JPEGを復号して輝度(0..1 sRGB)の中央値を返す。Pillowが無ければ None。"""
    try:
        from PIL import Image
        import io as _io
    except Exception:
        return None
    im = Image.open(_io.BytesIO(data)).convert("L")
    im = im.resize((160, 107))          # 速度のため縮小(中央値は保たれる)
    px = list(im.getdata())
    return statistics.median(px) / 255.0


def srgb_to_linear(x):
    return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--from", dest="a", default="")
    ap.add_argument("--to",   dest="b", default="")
    ap.add_argument("--step", type=int, default=1)
    ap.add_argument("--which", type=int, default=1, help="使うJPEG(0=最小 1=中間 2=最大)")
    g = ap.parse_args()

    files = sorted(glob.glob(os.path.join(g.folder, "IMG_*.CR3")))
    def num(p):
        m = re.search(r"IMG_(\d+)", os.path.basename(p))
        return int(m.group(1)) if m else -1
    lo = num(g.a) if g.a else -1
    hi = num(g.b) if g.b else 10**9
    files = [f for f in files if lo <= num(f) <= hi]
    files = files[::g.step]
    if not files:
        print("該当ファイルなし", file=sys.stderr)
        return 2

    print(f"# {g.folder}  {len(files)}枚")
    print("# ファイル        中央値(sRGB)  リニア    段(0.18基準)")
    for p in files:
        js, head = embedded_jpegs(p)
        if len(js) <= g.which:
            print(f"{os.path.basename(p)}  JPEG見つからず")
            continue
        off, size = js[g.which]
        x = jpeg_median_luma(head[off:off + size])
        if x is None:
            print("Pillow が必要です: py -m pip install pillow", file=sys.stderr)
            return 2
        lin = srgb_to_linear(x)
        import math
        print(f"{os.path.basename(p)}   {x:.4f}      {lin:.4f}   {math.log2(lin/0.18):+6.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
