# -*- coding: utf-8 -*-
# BATT行(60秒ごと)から放電カーブを取り出し、しきい値決定の材料を出す。
#   使い方: python an_battery.py <ログ...>
# ・chg=1(充電中)は放電カーブから除外する
# ・up が減る/小さくなる箇所は再起動 → 区間を切る(電池切れ以外の再起動を見分ける)
# ・電圧ごとの「残り時間」(その電圧を最後に通過してから区間終端までの分)を出す
import re, sys, os
sys.stdout.reconfigure(encoding='utf-8')

BATT = re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\|INF\|BATT  \|pct=(\d+) volt=(\d+)mV chg=(\d+) up=(\d+)s')

def load(path):
    rows = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = BATT.match(line.rstrip('\n'))
        if not m: continue
        d, t, pct, mv, chg, up = m.groups()
        rows.append(dict(d=d, t=t, pct=int(pct), mv=int(mv), chg=int(chg), up=int(up)))
    return rows

def mins(a, b):
    """HH:MM:SS の差[分]。日跨ぎは扱わない(1回の放電は数時間なので十分)。"""
    def s(x):
        h, m, sec = x.split(':'); return int(h)*3600 + int(m)*60 + int(sec)
    return (s(b) - s(a)) / 60.0

def segments(rows):
    """再起動(upが増えない)と充電状態でセグメントに割る。放電セグメントだけ返す。"""
    segs, cur = [], []
    prev_up = None
    for r in rows:
        newrun = (prev_up is not None and r['up'] <= prev_up)
        if newrun and cur: segs.append(cur); cur = []
        prev_up = r['up']
        if r['chg'] == 1:            # 充電中は放電カーブに含めない
            if cur: segs.append(cur); cur = []
            continue
        cur.append(r)
    if cur: segs.append(cur)
    return [s for s in segs if len(s) >= 5]

def report(path):
    rows = load(path)
    print("="*72)
    print(f"{os.path.basename(path)}  BATT行={len(rows)}")
    if not rows: return
    print("="*72)
    for i, seg in enumerate(segments(rows), 1):
        dur = mins(seg[0]['t'], seg[-1]['t'])
        print(f"\n--- 放電区間{i}: {seg[0]['d']} {seg[0]['t']} 〜 {seg[-1]['t']}  {dur:.0f}分 "
              f"({len(seg)}点)  {seg[0]['mv']}mV/{seg[0]['pct']}% → {seg[-1]['mv']}mV/{seg[-1]['pct']}% ---")
        end_t = seg[-1]['t']
        # 電圧の代表点で「残り時間」を出す(その電圧を最後に下回った時刻から終端まで)
        print("  電圧[mV] : 残り時間[分] (pct)   ※その電圧を最後に通過した時点から区間終端まで")
        for thr in (4000, 3900, 3850, 3800, 3750, 3700, 3650, 3600, 3550, 3500, 3450, 3400):
            last = None
            for r in seg:
                if r['mv'] >= thr: last = r
            if last is None: continue
            rem = mins(last['t'], end_t)
            print(f"    {thr}    : {rem:6.1f}分  (pct={last['pct']:3d}  {last['t']})")
        # 10分ごとの推移(粗い形を見る)
        print("  推移(10分ごと):")
        last_key = None
        for r in seg:
            k = int(mins(seg[0]['t'], r['t']) // 10)
            if k == last_key: continue
            last_key = k
            print(f"    +{k*10:3d}分  {r['mv']}mV  pct={r['pct']:3d}   {r['t']}")
        print(f"    終端    {seg[-1]['mv']}mV  pct={seg[-1]['pct']:3d}   {seg[-1]['t']}")

for p in sys.argv[1:]:
    report(p)
