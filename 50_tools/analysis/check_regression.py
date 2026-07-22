# -*- coding: utf-8 -*-
"""撮影ログの健全性チェック(長時間撮影の前に必ず通すゲート)。

使い方:
    python check_regression.py hg_2026-07-17.log [...]

判定する指標(1つでも赤なら長時間撮影に進まない):
  - 露出設定NG率      : PUTを送ったコマのうち失敗した割合。旧正常ログ=0%。
                        ここが高い=カメラに露出が届いておらず、アプリの露出モデルと実機がズレる。
  - 測光NG率          : 測光できなかったコマの割合。
  - Y飽和(>=1.0)      : 測光が振り切れたコマ。露出制御が効いていない兆候。
  - ERR件数           : ログ中のエラー行。
  - 周期              : シャッター間隔の分布(sh= から算出)。
  - late              : 新ログのみ。周期からの遅れ[ms]。0=ぴったり。

終了コード: 0=OK / 1=赤あり
"""
import re, sys, datetime as dt, statistics as st, collections

SHOT = re.compile(
    r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|INF\|SHOT  \|'
    r'\s*(\d+)\|\s*(\d+)\|(.{0,11}?)\s*\|(.{0,6}?)\s*\|\s*([-+\d.]+)\|'
    r'(\S+)(?: Y=([\d.]+) ev[-+][\d.]+)?')
RDY  = re.compile(r'rdy=(\d+)ms\((OK|NG)(?:,try(\d+))?\)')
SET  = re.compile(r'set=(\d+)ms\((OK|NG)(?:,try(\d+))?\)')
PREP = re.compile(r'prep=(\d+)ms')
LATE = re.compile(r'late=(\d+)ms')
SH   = re.compile(r'sh=(\d{2}:\d{2}:\d{2}\.\d{3})')

def load(path):
    rows, errs = [], 0
    for line in open(path, encoding="utf-8", errors="replace"):
        if "|ERR|" in line:
            errs += 1
            continue
        m = SHOT.match(line)
        if not m:
            continue
        r = dict(Y=float(m.group(9)) if m.group(9) else None, ccm=m.group(8))
        mm = RDY.search(line);  r["rdy"], r["rdyok"], r["rdytry"] = (int(mm.group(1)), mm.group(2), int(mm.group(3) or 0)) if mm else (None, None, 0)
        mm = SET.search(line);  r["set"], r["setok"], r["settry"] = (int(mm.group(1)), mm.group(2), int(mm.group(3) or 0)) if mm else (None, None, 0)
        mm = PREP.search(line); r["prep"] = int(mm.group(1)) if mm else None
        mm = LATE.search(line); r["late"] = int(mm.group(1)) if mm else None
        mm = SH.search(line)
        r["sh"] = dt.datetime.strptime(m.group(1) + " " + mm.group(1), "%Y-%m-%d %H:%M:%S.%f") if mm else None
        rows.append(r)
    return rows, errs

def report(path):
    rows, errs = load(path)
    print("=" * 70)
    print(f"{path}   SHOT={len(rows)}  ERR={errs}")
    if not rows:
        print("  SHOT行なし"); return True
    bad = False

    put = [r for r in rows if r["set"] is not None and r["set"] > 0]
    ng  = [r for r in put if r["setok"] == "NG"]
    rate = 100 * len(ng) / len(put) if put else 0.0
    flag = "  <<< 赤" if rate > 1.0 else ""
    print(f"  露出設定: PUT送信={len(put):5d}  NG={len(ng):5d}  NG率={rate:5.1f}%{flag}")
    if rate > 1.0: bad = True

    mng = [r for r in rows if r["rdyok"] == "NG"]
    mrate = 100 * len(mng) / len(rows)
    flag = "  <<< 赤" if mrate > 1.0 else ""
    print(f"  測光    : NG={len(mng):5d}  NG率={mrate:5.1f}%{flag}")
    if mrate > 1.0: bad = True

    ys = [r for r in rows if r["Y"] is not None]
    sat = [r for r in ys if r["Y"] >= 1.0]
    srate = 100 * len(sat) / len(ys) if ys else 0.0
    flag = "  <<< 赤" if srate > 5.0 else ""
    print(f"  Y飽和   : {len(sat):5d} / {len(ys):5d}  {srate:5.1f}%{flag}")
    if srate > 5.0: bad = True

    if errs > 0:
        print(f"  ERR行   : {errs}  <<< 要確認")

    # 試行回数(新ログのみ)
    tries = [r["settry"] for r in put if r["settry"]]
    if tries:
        c = collections.Counter(tries)
        print(f"  設定試行: " + " ".join(f"try{k}={v}" for k, v in sorted(c.items())))
    mtries = [r["rdytry"] for r in rows if r["rdytry"]]
    if mtries:
        c = collections.Counter(mtries)
        print(f"  測光試行: " + " ".join(f"try{k}={v}" for k, v in sorted(c.items())))

    preps = [r["prep"] for r in rows if r["prep"] is not None]
    if preps:
        over = len([p for p in preps if p > 2000])
        print(f"  prep    : median={st.median(preps):6.0f}ms max={max(preps):6.0f}ms  2秒超={over}/{len(preps)}")

    lates = [r["late"] for r in rows if r["late"] is not None]
    if lates:
        ontime = len([l for l in lates if l <= 100])
        print(f"  late    : median={st.median(lates):5.0f}ms max={max(lates):6.0f}ms  "
              f"周期どおり(<=100ms)={ontime}/{len(lates)} ({100*ontime/len(lates):.1f}%)")

    shs = [r["sh"] for r in rows if r["sh"]]
    if len(shs) > 2:
        shs.sort()
        iv = [(b - a).total_seconds() for a, b in zip(shs, shs[1:])]
        iv = [d for d in iv if 0 < d < 600]
        if iv:
            print(f"  周期(全) : median={st.median(iv):6.3f}s min={min(iv):6.3f} max={max(iv):6.3f}  "
                  f"(2台混在時は台数分の間隔になる)")
    print("  => " + ("赤あり: 長時間撮影に進まないこと" if bad else "OK"))
    return bad

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(2)
    anybad = False
    for p in sys.argv[1:]:
        anybad |= report(p)
    sys.exit(1 if anybad else 0)
