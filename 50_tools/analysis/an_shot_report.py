# -*- coding: utf-8 -*-
# 実写の測定(an_shot_linear.py)とログ(an_shot_log.py)を突き合わせ、
# **薄明の移行帯(preNight / postNight)で絵の明るさがどれだけ安定しているか**を出す。
#
# 【何を見るための道具か】移行帯の露出は自動制御でユーザーが触れない。平均が狙いに
#  乗っていても、コマ間でノコギリ状に振れるとタイムラプスではちらつきとして出る。
#  平均の追従とコマ間のばらつきを分けて数える。
#
# 出すもの:
#   1) 区間ごとの絵の明るさ(日中を基準にした段数)
#   2) コマ間の明るさ変化 … |Δ|平均・RMS・最大。日中と比べる
#   3) ノコギリの分解 … 「露出が動いたコマ」と「動かないコマ」に分けた変化量、山の長さと振れ
#   4) 露出1段あたりの画像の応答倍率(現像トーンカーブぶん。段数を語るときの割り算に使う)
#   5) 狙い ev0 と測光値の差 … 平均としての追従(ログの ev は18%基準なので別に計算する)
#   6) 被写体(空)の明るさの変化率
#   7) 飽和の割合
#
# 【妥当なコマの条件】`0 < 中央値 < 0.85`。飽和率で切ると、空が明るい日は日中が全滅して
#  基準が取れない(2026-09-02 に遭遇)。中央値そのものが飛んでいなければ中央値ベースの
#  指標は使えるので、この条件で統一する。**比較のため次回以降も変えないこと。**
#
# 使い方:
#   python an_shot_report.py <img.tsv> <log.tsv> <YYYY-MM-DD> [出力joined.tsv] [緯度] [経度]
# 例:
#   python an_shot_report.py img_0902.tsv log_0902.tsv 2026-09-02 joined_0902.tsv
import io, os, sys, math

if len(sys.argv) < 4:
    print("usage: an_shot_report.py <img.tsv> <log.tsv> <YYYY-MM-DD> [joined.tsv] [lat] [lon]")
    raise SystemExit(1)
IMG, LOG, DATE = sys.argv[1], sys.argv[2], sys.argv[3]
JOINED = sys.argv[4] if len(sys.argv) > 4 else ""
LAT = float(sys.argv[5]) if len(sys.argv) > 5 else 36.0514      # 既定=自宅
LON = float(sys.argv[6]) if len(sys.argv) > 6 else 139.4268
TZ = 9.0

Y, M, D = int(DATE[0:4]), int(DATE[5:7]), int(DATE[8:10])
JD0 = (367 * Y - int(7 * (Y + int((M + 9) / 12)) / 4) + int(275 * M / 9) + D + 1721013.5)


def sun_alt(hour_local):
    """太陽高度[°]。NOAA近似(誤差 ~0.1°)。窓の境目が何度で起きたかを見るのに使う。"""
    n = JD0 + (hour_local - TZ) / 24.0 - 2451545.0
    L = (280.460 + 0.9856474 * n) % 360.0
    g = math.radians((357.528 + 0.9856003 * n) % 360.0)
    lam = math.radians(L + 1.915 * math.sin(g) + 0.020 * math.sin(2 * g))
    eps = math.radians(23.439 - 0.0000004 * n)
    dec = math.asin(math.sin(eps) * math.sin(lam))
    ra = math.atan2(math.cos(eps) * math.sin(lam), math.cos(lam))
    gmst = (18.697374558 + 24.06570982441908 * n) % 24.0
    H = math.radians((gmst * 15.0 + LON) % 360.0) - ra
    la = math.radians(LAT)
    return math.degrees(math.asin(math.sin(la) * math.sin(dec) + math.cos(la) * math.cos(dec) * math.cos(H)))


# ev0 シグモイド(exposureMath.h と同じ)。薄明ほど暗い絵を狙う。
HI, LO = 0.18, 0.06
def ev0_of(bv, alt):
    bm = max(-2.0, min(3.0, -0.5 * alt))
    return LO + (HI - LO) / (1.0 + math.exp(-(bv - bm)))


def load(p):
    rows = []
    with io.open(p, encoding="utf-8") as f:
        h = f.readline().rstrip("\n").split("\t")
        for l in f:
            rows.append(dict(zip(h, l.rstrip("\n").split("\t"))))
    return rows


def sec(t):
    t = t.strip()
    if len(t) >= 19 and ":" in t:
        t = t[11:19]            # "YYYY:MM:DD hh:mm:ss" の時刻部
    return int(t[0:2]) * 3600 + int(t[3:5]) * 60 + int(t[6:8])


def hhmm(v):
    return "%02d:%02d:%02d" % (v // 3600, (v % 3600) // 60, v % 60)


# ---------------------------------------------------------------- 突き合わせ
img, log = load(IMG), load(LOG)
byt = {}
for r in log:
    byt[sec(r["shtime"])] = r
rows = []
for r in img:
    if r.get("time", "") in ("", "ERR"):
        continue
    t = sec(r["time"])
    lr = None
    for d in (0, 1, -1, 2, -2, 3, -3):      # EXIFの秒とログのシャッター時刻は数秒ずれる
        if t + d in byt:
            lr = byt[t + d]
            break
    if lr is None:
        continue
    rows.append(dict(t=t, time=hhmm(t), ccm=lr["ccm"], iso=float(r["iso"]), ss=float(r["ss"]),
                     fn=float(r["fn"]), mean=float(r["Ymean"]), med=float(r["Ymed"]),
                     sat=float(r["sat"]), Ymeter=lr["Ymeter"], everr=lr["evadj"]))
rows.sort(key=lambda x: x["t"])
if not rows:
    print("突き合わせできませんでした。日付と朝夕の選択を確かめてください。")
    raise SystemExit(1)
for r in rows:
    # 露出の強さ[段]。大きいほど「暗く撮る」設定
    r["E"] = math.log2(r["fn"] ** 2 / r["ss"]) - math.log2(r["iso"] / 100.0)
print("突き合わせ %d コマ (画像 %d / ログ %d)" % (len(rows), len(img), len(log)))

if JOINED:
    out = io.open(JOINED, "w", encoding="utf-8", newline="\n")
    out.write("time\tccm\tiso\tss\tfn\tYmean\tYmed\tsat\tYmeter\tevlog\n")
    for r in rows:
        out.write("%s\t%s\t%g\t%g\t%g\t%.5f\t%.5f\t%.4f\t%s\t%s\n" % (
            r["time"], r["ccm"], r["iso"], r["ss"], r["fn"], r["mean"], r["med"],
            r["sat"], r["Ymeter"] or "-", r["everr"] or "-"))
    out.close()
    print("  ->", JOINED)

# ---------------------------------------------------------------- 窓の把握
order, seen = [], set()
for r in rows:
    if r["ccm"] not in seen:
        seen.add(r["ccm"]); order.append(r["ccm"])
TRANS = next((c for c in order if c in ("preNight", "postNight")), None)
DAY = next((c for c in order if c == "Daylight"), None)
print("窓:", " -> ".join("%s(%d)" % (c, sum(1 for r in rows if r["ccm"] == c)) for c in order))
for c in order:
    g = [r for r in rows if r["ccm"] == c]
    print("   %-11s %s 〜 %s  太陽高度 %+.1f° 〜 %+.1f°"
          % (c, g[0]["time"], g[-1]["time"], sun_alt(g[0]["t"] / 3600.0), sun_alt(g[-1]["t"] / 3600.0)))
if TRANS is None:
    print("移行帯(preNight/postNight)がありません。ここまで。")
    raise SystemExit(0)

def ok(r):
    return 0 < r["med"] < 0.85          # 中央値が飛んでいないコマだけ使う
def st(a, b):
    return math.log2(a / b) if a > 0 and b > 0 else float("nan")

tr = [r for r in rows if r["ccm"] == TRANS]
day = [r for r in rows if r["ccm"] == DAY] if DAY else []
if day:
    dmean = sum(r["mean"] for r in day) / len(day)
    dmed = sum(r["med"] for r in day) / len(day)
    print("日中の基準: 平均 %.4f / 中央値 %.4f" % (dmean, dmed))
else:
    dmean = dmed = float("nan")

# ---------------------------------------------------------------- 1) 区間ごとの明るさ
print("\n== %s の明るさ(区間は移行帯を4等分) ==" % TRANS)
t0, t1 = tr[0]["t"], tr[-1]["t"]
for k in range(4):
    a = t0 + (t1 - t0) * k // 4
    b = t0 + (t1 - t0) * (k + 1) // 4
    g = [r for r in tr if a <= r["t"] < b] or [r for r in tr if a <= r["t"] <= b]
    if not g:
        continue
    m = sum(r["mean"] for r in g) / len(g)
    md = sum(r["med"] for r in g) / len(g)
    print("  %s〜%s n=%3d 平均 %.4f(%+.2f段) 中央値 %.4f(%+.2f段)"
          % (hhmm(a), hhmm(b), len(g), m, st(m, dmean), md, st(md, dmed)))
w = min((r for r in tr if ok(r)), key=lambda r: r["med"], default=None)
if w:
    print("  いちばん暗いコマ %s 中央値 %.5f (日中比 %+.2f段)" % (w["time"], w["med"], st(w["med"], dmed)))

# ---------------------------------------------------------------- 2) コマ間の変化
print("\n== コマ間の明るさ変化 ==")
def flick(g, key):
    d = []
    for i in range(1, len(g)):
        a, b = g[i - 1][key], g[i][key]
        if a > 0 and b > 0 and g[i]["t"] - g[i - 1]["t"] <= 20:
            d.append(math.log2(b / a))
    if not d:
        return None
    n = len(d)
    return (n, sum(abs(x) for x in d) / n, math.sqrt(sum(x * x for x in d) / n), max(d), min(d))

def seg(a, b):
    return [r for r in rows if a <= r["t"] < b and ok(r)]

parts = [(TRANS, t0, t1 + 1),
         (TRANS + " 前半", t0, (t0 + t1) // 2),
         (TRANS + " 後半", (t0 + t1) // 2, t1 + 1)]
if day:
    parts.append(("日中", day[0]["t"], day[-1]["t"] + 1))
for nm, a, b in parts:
    g = seg(a, b)
    for key, lab in (("med", "中央値"), ("mean", "平均  ")):
        f = flick(g, key)
        if f:
            print("  %-16s %s n=%3d |Δ|平均 %.3f段 RMS %.3f 最大 %+.2f/%+.2f段"
                  % (nm, lab, f[0], f[1], f[2], f[3], f[4]))

# ---------------------------------------------------------------- 3) ノコギリの分解
g = seg(t0, t1 + 1)
step, hold = [], []
for i in range(1, len(g)):
    if g[i]["t"] - g[i - 1]["t"] > 20:
        continue
    d = math.log2(g[i]["med"] / g[i - 1]["med"])
    de = g[i - 1]["E"] - g[i]["E"]          # 露出を明るくした量[段]
    (step if abs(de) > 0.01 else hold).append((g[i]["time"], d, de))
print("\n== ノコギリの分解 ==")
for nm, arr in (("露出が動いたコマ", step), ("露出そのままのコマ", hold)):
    if not arr:
        continue
    v = [x[1] for x in arr]
    print("  %-18s n=%3d Δ平均 %+.3f段 |Δ|平均 %.3f段 最大 %+.2f/%+.2f段"
          % (nm, len(arr), sum(v) / len(v), sum(abs(x) for x in v) / len(v), max(v), min(v)))
rat = [x[1] / x[2] for x in step if abs(x[2]) > 0.05]
if rat:
    print("  露出1段あたりの画像の応答 %.2f 倍 (n=%d, %.2f〜%.2f) ← 段数はこれで割る"
          % (sum(rat) / len(rat), len(rat), min(rat), max(rat)))
runs, cur = [], [g[0]]
for i in range(1, len(g)):
    if abs(g[i]["E"] - g[i - 1]["E"]) > 0.01:
        runs.append(cur); cur = [g[i]]
    else:
        cur.append(g[i])
runs.append(cur)
rr = [r for r in runs if len(r) >= 2]
if rr:
    L = [len(r) for r in rr]
    Dv = [math.log2(r[-1]["med"] / r[0]["med"]) for r in rr]
    print("  山 %d本 1本 %.1fコマ(%.0f秒) 振れ 平均%.2f段 最大%.2f段"
          % (len(rr), sum(L) / len(L), 15.0 * sum(L) / len(L),
             sum(abs(x) for x in Dv) / len(Dv), max(abs(x) for x in Dv)))
    for x in sorted(rr, key=lambda r: -abs(math.log2(r[-1]["med"] / r[0]["med"])))[:5]:
        print("    %s %2dコマ %+.2f段" % (x[0]["time"], len(x), math.log2(x[-1]["med"] / x[0]["med"])))

# ---------------------------------------------------------------- 5) 狙い ev0 と測光値
print("\n== 狙い ev0 と測光値(移行帯を5分ごと) ==")
print("  時刻     高度   ISO  ss       測光Y   狙いev0  差[段]  画像中央値")
last = -999
for r in tr:
    if r["t"] - last < 300:
        continue
    last = r["t"]
    alt = sun_alt(r["t"] / 3600.0)
    y = r["Ymeter"]
    if y in ("-", ""):
        print("  %s %6.2f %5.0f %-8g %-7s %-8s %-6s %9.5f"
              % (r["time"], alt, r["iso"], r["ss"], "-", "-", "-", r["med"]))
        continue
    y = float(y)
    bv = 2 * math.log2(r["fn"]) + math.log2(1.0 / r["ss"]) - math.log2(r["iso"] / 100.0) + math.log2(y / 0.18)
    e0 = ev0_of(bv, alt)
    print("  %s %6.2f %5.0f %-8g %-7.4f %-8.4f %+6.2f %9.5f"
          % (r["time"], alt, r["iso"], r["ss"], y, e0, math.log2(y / e0), r["med"]))

# ---------------------------------------------------------------- 6) 被写体(空)の明るさ
print("\n== 被写体(空)の明るさ ==")
sky = [(r["t"], r["time"], r["ccm"], math.log2(r["med"]) + r["E"]) for r in rows if ok(r)]
z = sky[0][3]
for nm, a, b in [(TRANS, t0, t1)] + ([("日中", day[0]["t"], day[-1]["t"])] if day else []):
    A = min(sky, key=lambda x: abs(x[0] - a))
    B = min(sky, key=lambda x: abs(x[0] - b))
    mins = (B[0] - A[0]) / 60.0
    if mins > 0:
        print("  %-11s %s→%s %+.2f段 / %.0f分 = %+.3f段/分"
              % (nm, A[1], B[1], B[3] - A[3], mins, (B[3] - A[3]) / mins))
last = -999
for x in sky:
    if x[0] - last < 900:
        continue
    last = x[0]
    print("    %s %-11s %+.2f段" % (x[1], x[2], x[3] - z))

# ---------------------------------------------------------------- 7) 飽和
print("\n== 飽和(sRGB 250以上の画素の割合) ==")
for c in order:
    gg = [r for r in rows if r["ccm"] == c]
    print("  %-11s 平均 %.1f%% 最大 %.1f%% 1%%超 %d/%d"
          % (c, 100 * sum(r["sat"] for r in gg) / len(gg), 100 * max(r["sat"] for r in gg),
             sum(1 for r in gg if r["sat"] >= 0.01), len(gg)))
