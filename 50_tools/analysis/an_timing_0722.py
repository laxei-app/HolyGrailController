# -*- coding: utf-8 -*-
# R10 だけ撮影周期が崩れる原因の切り分け。準備(prep)の内訳 rdy(測光)/set(露出設定) と
# LVHIST の stl(Tv反映待ち)/mss(測光ss) をカメラ別に比べる。
import re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))	# 同じフォルダの an_run_0722 を使う
# an_run_0722 は import 時に sys.stdout を UTF-8 でラップする。ここで二重にラップすると
# 内側のラッパが GC されたときに buffer が閉じられて I/O エラーになるので、こちらでは触らない。
from an_run_0722 import parse, split2, secs, LOGDIR

SHOT = re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\|INF\|SHOT  \|\s*(\d+)\|')

def rich(path, lo, hi):
    """SHOT行から rdy/set/prep/late と ss を取る(ストリーム分離用に frame も)。"""
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SHOT.match(line.rstrip('\n'))
        if not m: continue
        t = m.group(2)
        if not (lo <= t <= hi): continue
        f = int(m.group(3))
        g = lambda p: (re.search(p, line).group(1) if re.search(p, line) else None)
        parts = line.split('|')
        out.append(dict(t=t, frame=f, ss=parts[5].strip() if len(parts) > 5 else '',
                        rdy=int(g(r'rdy=(\d+)ms') or -1),
                        setms=int(g(r'set=(\d+)ms') or -1),
                        prep=int(g(r'prep=(\d+)ms') or -1),
                        late=int(g(r'late=(-?\d+)ms') or 0),
                        tries=int(g(r'try(\d+)\)') or 1),
                        stale=int(g(r'stale=(\d+)') or 0)))
    return out

def pct(vals, p):
    if not vals: return 0
    v = sorted(vals); i = min(len(v)-1, int(len(v)*p/100))
    return v[i]

def report(name, s):
    if not s: print(f"[{name}] 空"); return
    rdy = [x['rdy'] for x in s if x['rdy'] >= 0]
    st  = [x['setms'] for x in s if x['setms'] >= 0]
    pr  = [x['prep'] for x in s if x['prep'] >= 0]
    la  = [x['late'] for x in s]
    print(f"[{name}] コマ={len(s)}")
    for lbl, v in (("rdy(測光)", rdy), ("set(露出設定)", st), ("prep(準備計)", pr)):
        if v: print(f"    {lbl:<14} 平均={sum(v)/len(v):7.0f}ms  中央={pct(v,50):6}ms  p90={pct(v,90):6}ms  最大={max(v):6}ms")
    over = sum(1 for v in pr if v > 5000)   # kPrepLeadMs=5000 を超えると必ず遅れる
    print(f"    prep>5000ms(=リード超過): {over}コマ ({100*over/max(1,len(pr)):.1f}%)")
    print(f"    遅れ: 平均={sum(la)/len(la):.0f}ms 最大={max(la)}ms  1秒超={sum(1 for v in la if v>1000)}コマ")
    print(f"    stale(古いLV破棄)={sum(x['stale'] for x in s)}回")

base = LOGDIR
for day, lo, hi, label in (("hg_2026-07-22.log", "15:00", "21:00", "7/22 夕方"),
                           ("hg_2026-07-23.log", "02:00", "06:00", "7/23 明朝")):
    print("="*66); print(label); print("="*66)
    ev = parse(base + "\\" + day, lo, hi)
    A, B = split2(ev)
    # フレーム番号列で分離した結果を rich 側にも同じ順序で適用する
    r = rich(base + "\\" + day, lo, hi)
    RA, RB, na, nb = [], [], 1, 1
    for e in r:
        f = e['frame']
        if f == na and f == nb:
            if not RA: RA.append(e); na += 1
            elif not RB: RB.append(e); nb += 1
            else:
                ta = secs(e['t'])
                if abs(ta - secs(RA[-1]['t']) - 15) <= abs(ta - secs(RB[-1]['t']) - 15): RA.append(e); na += 1
                else: RB.append(e); nb += 1
        elif f == na: RA.append(e); na += 1
        elif f == nb: RB.append(e); nb += 1
        else:
            if abs(f-na) <= abs(f-nb): RA.append(e); na = f+1
            else: RB.append(e); nb = f+1
    big, small = (RA, RB) if len(RA) > len(RB) else (RB, RA)
    if day.endswith("22.log"): report("R10 ", big); report("R100", small)
    else:                      report("R10 ", small); report("R100", big)
    print()
