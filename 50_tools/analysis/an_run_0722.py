# -*- coding: utf-8 -*-
# 2026-07-22夜〜23朝の通し解析。露出修正(commit 497beb2)後の初の通し。
# 同一ログに2台(2計画)のSHOTが交錯するので、フレーム番号が計画ごとに1から独立採番される
# ことを使い時系列で2ストリームへ貪欲分離する。正しさはレポートのコマ数で検証する。
import re, sys, io, os
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

# ログ実体の置き場。スクリプトはリポジトリ内(50_tools/analysis)、ログ/画像は追跡外の
# _retrieved_logs / _picture に置く運用なので、スクリプト位置から相対で求める。
REPO   = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOGDIR = os.path.join(REPO, "_retrieved_logs")

SHOT = re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\|INF\|SHOT  \|\s*(\d+)\|\s*(\d+)\|([^|]+)\|([^|]+)\|\s*([-+][\d.]+)\|(\S+)(.*)$')

def parse(path, lo, hi):
    out = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = SHOT.match(line.rstrip('\n'))
        if not m: continue
        d, t, fr, iso, ss, fn, ev, ccm, rest = m.groups()
        if not (lo <= t <= hi): continue
        g = lambda pat: (re.search(pat, rest).group(1) if re.search(pat, rest) else None)
        Y    = g(r'Y=([\d.]+)')
        late = g(r'late=(-?\d+)ms')
        prep = g(r'prep=(\d+)ms')
        sh   = g(r'sh=(\d\d:\d\d:\d\d\.\d+)')
        setng = ('set=' in rest) and ('NG' in rest.split('set=')[1][:24])
        out.append(dict(t=t, frame=int(fr), iso=int(iso), ss=ss.strip(), fn=fn.strip(),
                        ev=float(ev), Y=(float(Y) if Y else None),
                        late=(int(late) if late else None),
                        prep=(int(prep) if prep else None), sh=sh, ccm=ccm, setng=setng))
    return out

def secs(t):
    h, m, s = t.split(':'); return int(h)*3600 + int(m)*60 + int(s)

def split2(evt):
    A, B, na, nb = [], [], 1, 1
    for e in evt:
        f = e['frame']; oa, ob = (f == na), (f == nb)
        if oa and ob:
            if not A: A.append(e); na += 1
            elif not B: B.append(e); nb += 1
            else:
                ta = secs(e['t'])
                da = abs(ta - secs(A[-1]['t']) - 15); db = abs(ta - secs(B[-1]['t']) - 15)
                if da <= db: A.append(e); na += 1
                else: B.append(e); nb += 1
        elif oa: A.append(e); na += 1
        elif ob: B.append(e); nb += 1
        else:
            if abs(f - na) <= abs(f - nb): A.append(e); na = f + 1
            else: B.append(e); nb = f + 1
    return A, B

def expo_str(e):
    return f"ISO{e['iso']:<4} {e['ss']:>7} f/{e['fn']:<4}"

def summarize(name, s):
    if not s: print(f"[{name}] 空"); return
    lates = [x['late'] for x in s if x['late'] is not None]
    preps = [x['prep'] for x in s if x['prep'] is not None]
    ontime = sum(1 for v in lates if v <= 100)
    chg = sum(1 for i in range(1, len(s)) if (s[i]['iso'], s[i]['ss'], s[i]['fn']) != (s[i-1]['iso'], s[i-1]['ss'], s[i-1]['fn']))
    print(f"[{name}] コマ={len(s)}  {s[0]['t']}..{s[-1]['t']}  露出変更={chg}回")
    if lates:
        print(f"    遅れ: 100ms以内={ontime}/{len(lates)} ({100*ontime/len(lates):.1f}%) 平均={sum(lates)/len(lates):.0f}ms 最大={max(lates)}ms")
    if preps:
        print(f"    準備: 平均={sum(preps)/len(preps):.0f}ms 最大={max(preps)}ms")
    print(f"    set失敗={sum(1 for x in s if x['setng'])}コマ")

def trajectory(name, s, step_min=15):
    print(f"--- {name} 露出推移({step_min}分ごと) ---")
    last = None
    for x in s:
        key = (secs(x['t']) // (step_min*60))
        if key == last: continue
        last = key
        y = f" Y={x['Y']:.4f}" if x['Y'] is not None else ""
        print(f"  {x['t']}  fr{x['frame']:<5} {expo_str(x)} ev{x['ev']:+7.3f} {x['ccm']:<9}{y}")

if __name__ == "__main__":
    base = LOGDIR

    print("="*70); print("7/22  EveR10コピー(R10 15:00-21:00) と 新規撮影計画(R100 18:17-)"); print("="*70)
    ev = parse(base + r"\hg_2026-07-22.log", "15:00", "21:00")
    A, B = split2(ev)
    R10, R100 = (A, B) if len(A) > len(B) else (B, A)   # レポート: R10=1348, R100=580
    summarize("R10 ", R10); summarize("R100", R100)
    print(); trajectory("R10 (7/22)", R10, 15)
    print(); trajectory("R100 (7/22)", R100, 15)

    print(); print("="*70); print("7/23  DawnR10(02:00-05:30) と DawnR100(02:00-05:46)"); print("="*70)
    dv = parse(base + r"\hg_2026-07-23.log", "02:00", "06:00")
    A2, B2 = split2(dv)
    D10, D100 = (A2, B2) if len(A2) < len(B2) else (B2, A2)  # レポート: R10=820, R100=905
    summarize("R10 ", D10); summarize("R100", D100)
    print(); trajectory("R10 (7/23)", D10, 15)
    print(); trajectory("R100 (7/23)", D100, 15)
