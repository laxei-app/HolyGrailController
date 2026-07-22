# -*- coding: utf-8 -*-
# 2つの通しで「両カメラ同時のネットワーク詰まり」が起きたかを比較する。
#  異常の定義: シャッター間隔が15±1秒を外れる / rdyが3秒超 / ERR行。
#  相関の定義: 両ストリームの異常が±60秒以内に併発。
import re, datetime as dt, collections, sys

RE = re.compile(
    r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|INF\|SHOT  \|'
    r'\s*(\d+)\|')
RDY = re.compile(r'rdy=(\d+)ms\((OK|NG)')
SH  = re.compile(r'sh=(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?')

def load(files, t0, t1):
    rows = []
    errs = []
    for f in files:
        for ln in open(f, encoding="utf-8", errors="replace"):
            if "|ERR|" in ln:
                try:
                    et = dt.datetime.strptime(ln[:19], "%Y-%m-%d %H:%M:%S")
                    if t0 <= et <= t1: errs.append((et, ln.split("|ERR   |")[-1].strip()[:40]))
                except: pass
                continue
            m = RE.match(ln)
            if not m: continue
            logt = dt.datetime.strptime(m.group(1) + " " + m.group(2), "%Y-%m-%d %H:%M:%S")
            if not (t0 <= logt <= t1): continue
            sh = SH.search(ln)
            if not sh: continue
            g = sh.groups()
            ms = (g[3] or "0").ljust(3, "0")
            sht = dt.datetime.strptime(m.group(1) + " %s:%s:%s.%s" % (g[0], g[1], g[2], ms), "%Y-%m-%d %H:%M:%S.%f")
            if (logt - sht).total_seconds() < -3600: sht -= dt.timedelta(days=1)
            r = dict(t=sht, fr=int(m.group(3)))
            mm = RDY.search(ln)
            if mm: r['rdy'] = int(mm.group(1))
            rows.append(r)
    rows.sort(key=lambda r: r['t'])
    return rows, errs

def split(rows):
    streams = []
    for r in rows:
        best, bestd = None, None
        for s in streams:
            dfr = r['fr'] - s['fr']
            if dfr < 1 or dfr > 20: continue
            exp = s['t'] + dt.timedelta(seconds=15.0 * dfr)
            d = abs((r['t'] - exp).total_seconds())
            if d < 8 and (bestd is None or d < bestd): best, bestd = s, d
        if best is None:
            streams.append(dict(t=r['t'], fr=r['fr'], rows=[r]))
        else:
            best['t'] = r['t']; best['fr'] = r['fr']; best['rows'].append(r)
    return sorted([s['rows'] for s in streams if len(s['rows']) > 100], key=lambda x: x[0]['t'])

def anomalies(rs):
    out = []
    for a, b in zip(rs, rs[1:]):
        iv = (b['t'] - a['t']).total_seconds()
        if abs(iv - 15.0) > 1.0 and iv < 300:
            out.append((b['t'], f"間隔{iv:.1f}s"))
    for r in rs:
        if r.get('rdy', 0) > 3000:
            out.append((r['t'], f"rdy={r['rdy']}ms"))
    return sorted(out)

def analyze(name, files, t0, t1):
    rows, errs = load(files, t0, t1)
    sts = split(rows)
    print("=" * 72)
    print(f"### {name}  SHOT={len(rows)} ストリーム={[len(s) for s in sts]} ERR={len(errs)}")
    if len(sts) < 2:
        print("  2ストリームに分離できず"); return
    an = [anomalies(s) for s in sts[:2]]
    print(f"  異常(間隔±1s超 or rdy>3s): A={len(an[0])}件  B={len(an[1])}件")
    # 相関: Aの異常とBの異常が±60秒以内
    used = set()
    pairs = []
    for ta, wa in an[0]:
        for j, (tb, wb) in enumerate(an[1]):
            if j in used: continue
            if abs((ta - tb).total_seconds()) <= 60:
                pairs.append((min(ta, tb), wa, wb)); used.add(j); break
    print(f"  ★両カメラ同時(±60s)の異常: {len(pairs)}件")
    for t, wa, wb in pairs:
        # 近傍のERRも
        near = [e for e in errs if abs((e[0]-t).total_seconds()) <= 90]
        es = (" / ERR: " + "; ".join(e[1] for e in near[:3])) if near else ""
        print(f"    {t.strftime('%m-%d %H:%M:%S')}  A:{wa}  B:{wb}{es}")
    # 単独の異常も時刻だけ
    solo_a = [x for x in an[0] if not any(abs((x[0]-p[0]).total_seconds())<=60 for p in pairs)]
    solo_b = [x for x in an[1] if not any(abs((x[0]-p[0]).total_seconds())<=60 for p in pairs)]
    if solo_a or solo_b:
        print(f"  単独の異常: A={len(solo_a)}件 B={len(solo_b)}件")
        for t,w in (solo_a+solo_b):
            print(f"    {t.strftime('%m-%d %H:%M:%S')} {w}")
    print(f"  ERR一覧:")
    for et, w in errs: print(f"    {et.strftime('%m-%d %H:%M:%S')} {w}")

analyze("おととい 7/16 15:00 ~ 7/17 05:29 (1回目の通し)",
        ["hg_2026-07-16.log", "hg_2026-07-17.log"],
        dt.datetime(2026,7,16,15,0), dt.datetime(2026,7,17,5,29))
print()
analyze("昨夜 7/17 17:00 ~ 7/18 05:30 (2回目の通し)",
        ["hg_2026-07-17.log", "hg_2026-07-18.log"],
        dt.datetime(2026,7,17,17,0), dt.datetime(2026,7,18,5,31))
