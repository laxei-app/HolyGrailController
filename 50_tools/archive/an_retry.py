# -*- coding: utf-8 -*-
# 通し(7/17 17:00~7/18 05:30)の リトライ/周期乱れ 集計。
# 2台分離: サブ秒位相は一晩でドリフトする(prep超過のたび相対アンカーがずれる)ため、
# フレーム連番+期待時刻(前コマ+15秒×コマ差)で追跡する。
import re, datetime as dt, statistics as st, collections

FILES = ["hg_2026-07-17.log", "hg_2026-07-18.log"]
RE = re.compile(
    r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|INF\|SHOT  \|'
    r'\s*(\d+)\|\s*(\d+)\|(.{0,11}?)\s*\|(.{0,6}?)\s*\|\s*([-+\d.]+)\|'
    r'(\S+)(?: Y=([\d.]+) ev([-+][\d.]+))?')
RDY  = re.compile(r'rdy=(\d+)ms\((OK|NG),try(\d+)\)')
SET  = re.compile(r'set=(\d+)ms\((OK|NG),try(\d+)\)')
PREP = re.compile(r'prep=(\d+)ms')
LATE = re.compile(r'late=(\d+)ms')
STALE= re.compile(r'stale=(\d+)')
# 行末切り詰め(detail[128]溢れ)でミリ秒が欠けることがある → 1〜3桁を許容し0埋め
SH   = re.compile(r'sh=(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,3}))?')

t0 = dt.datetime(2026, 7, 17, 17, 0)
t1 = dt.datetime(2026, 7, 18, 5, 31)
rows = []
for f in FILES:
    day = f[3:13]
    for ln in open(f, encoding="utf-8", errors="replace"):
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
        r = dict(t=sht, fr=int(m.group(3)), ccm=m.group(8), Y=m.group(9) is not None)
        for pat, keys in ((RDY, ('rdyMs','rdyOk','rdyTry')), (SET, ('setMs','setOk','setTry'))):
            mm = pat.search(ln)
            if mm:
                r[keys[0]] = int(mm.group(1)); r[keys[1]] = (mm.group(2) == 'OK'); r[keys[2]] = int(mm.group(3))
        for pat, key in ((PREP,'prep'), (LATE,'late'), (STALE,'stale')):
            mm = pat.search(ln)
            if mm: r[key] = int(mm.group(1))
        rows.append(r)
rows.sort(key=lambda r: r['t'])
print("対象SHOT:", len(rows))

# --- フレーム連番+期待時刻で2ストリームへ割当 ---
streams = []
for r in rows:
    best, bestd = None, None
    for s in streams:
        dfr = r['fr'] - s['fr']
        if dfr < 1 or dfr > 20: continue            # 連番(多少の抜けは許す)
        exp = s['t'] + dt.timedelta(seconds=15.0 * dfr)
        d = abs((r['t'] - exp).total_seconds())
        if d < 8 and (bestd is None or d < bestd): best, bestd = s, d
    if best is None:
        streams.append(dict(t=r['t'], fr=r['fr'], rows=[r]))
    else:
        best['t'] = r['t']; best['fr'] = r['fr']; best['rows'].append(r)
streams = [s for s in streams if len(s['rows']) > 100]
print("ストリーム数:", len(streams), [len(s['rows']) for s in streams])

def fmt_ms(v): return f"{v}ms"

def report(rs, nm):
    print("\n" + "=" * 70)
    print(f"### {nm}  n={len(rs)}  {rs[0]['t'].strftime('%H:%M:%S')} ~ {rs[-1]['t'].strftime('%H:%M:%S')}")

    # --- 測光(ライブビュー取得)リトライ ---
    met = [r for r in rs if 'rdyTry' in r]
    tries = collections.Counter(r['rdyTry'] for r in met)
    extra = sum((r['rdyTry'] - 1) for r in met)
    print(f"\n[測光(ライブビュー)] 測光したコマ={len(met)} / 測光しない(夜間等)={len(rs)-len(met)}")
    print(f"  試行回数の分布: " + "  ".join(f"try{k}={v}" for k, v in sorted(tries.items())))
    print(f"  リトライ発動コマ(try>=2): {sum(v for k,v in tries.items() if k>=2)} "
          f"({100*sum(v for k,v in tries.items() if k>=2)/max(len(met),1):.1f}%)  延べ再試行={extra}回")
    rd = [r['rdyMs'] for r in met]
    print(f"  取得時間: median={st.median(rd):.0f}ms p90={sorted(rd)[int(len(rd)*.9)]}ms max={max(rd)}ms")
    # try>=2 がどのccm/時間帯か
    cc = collections.Counter(r['ccm'] for r in met if r['rdyTry'] >= 2)
    print(f"  リトライが出たccm: {dict(cc)}")
    worst = sorted(met, key=lambda r: -r['rdyTry'])[:5]
    for r in worst:
        if r['rdyTry'] < 2: break
        print(f"    {r['t'].strftime('%H:%M:%S')} {r['ccm']} try{r['rdyTry']} rdy={r['rdyMs']}ms")

    # --- 露出設定リトライ ---
    sets = [r for r in rs if 'setTry' in r]
    sent = [r for r in sets if r['setMs'] > 0]      # 実際にPUTを送ったコマ
    stries = collections.Counter(r['setTry'] for r in sent)
    print(f"\n[露出設定] 送信したコマ={len(sent)} / 変更なし(送信0)={len(sets)-len(sent)}")
    print(f"  試行回数の分布(送信したコマ): " + "  ".join(f"try{k}={v}" for k, v in sorted(stries.items())))
    fails = [r for r in sets if not r['setOk']]
    print(f"  最終失敗(NG): {len(fails)}")
    sm = [r['setMs'] for r in sent]
    if sm: print(f"  設定時間: median={st.median(sm):.0f}ms p90={sorted(sm)[int(len(sm)*.9)]}ms max={max(sm)}ms")

    # --- 撮影周期の乱れ(late) ---
    lat = [r for r in rs if 'late' in r]
    lv = [r['late'] for r in lat]
    buck = collections.Counter()
    for v in lv:
        if v <= 100: buck['0-100ms (周期どおり)'] += 1
        elif v <= 500: buck['100-500ms'] += 1
        elif v <= 1000: buck['0.5-1秒'] += 1
        elif v <= 2000: buck['1-2秒'] += 1
        else: buck['2秒超'] += 1
    print(f"\n[撮影周期の乱れ(late=前コマ+15秒からの遅れ)] 計測={len(lv)}コマ")
    for k in ['0-100ms (周期どおり)','100-500ms','0.5-1秒','1-2秒','2秒超']:
        if buck[k]: print(f"  {k:20s}: {buck[k]:5d} ({100*buck[k]/len(lv):5.1f}%)")
    print(f"  合計遅れ={sum(lv)/1000:.1f}秒 / max={max(lv)}ms")
    worst = sorted(lat, key=lambda r: -r['late'])[:6]
    print("  遅れが大きかったコマ:")
    for r in worst:
        print(f"    {r['t'].strftime('%H:%M:%S')} {r['ccm']:9s} late={r['late']}ms prep={r.get('prep','-')}ms rdy={r.get('rdyMs','-')}ms try{r.get('rdyTry','-')}")

    # --- prep がリード(2秒)に収まらなかったコマ ---
    pr = [r for r in rs if 'prep' in r]
    over = [r for r in pr if r['prep'] > 2000]
    print(f"\n[準備(測光→計算→設定)] 2秒超={len(over)}/{len(pr)} ({100*len(over)/max(len(pr),1):.1f}%)  "
          f"prep median={st.median([r['prep'] for r in pr]):.0f}ms max={max(r['prep'] for r in pr)}ms")
    cc = collections.Counter(r['ccm'] for r in over)
    print(f"  2秒超が出たccm: {dict(cc)}")

    # --- stale ---
    sl = [r for r in rs if r.get('stale', 0) > 0]
    print(f"\n[古いライブビュー破棄] {len(sl)}コマ (延べ{sum(r['stale'] for r in sl)}回)")

    # カメラ識別の手掛かり: 最初のコマのlvオフセットは出せないのでコマ数で
    return len(rs)

for s in streams:
    rs = s['rows']
    report(rs, f"ストリーム(開始 {rs[0]['t'].strftime('%H:%M:%S.%f')[:-3]} / 総{len(rs)}コマ)")
