# -*- coding: utf-8 -*-
# 2026-07-20夜〜21朝の無人4計画ラン解析。
# 同一日ログに2台(2計画)のSHOTが交錯する。フレーム番号が各計画で1から独立採番される
# ことを使い、時系列で2ストリームへ貪欲分離する。分離の正しさは復元コマ数で検証。
import re, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

def apex_ss(s):
    s=s.strip()
    if '"' in s:  # 8" 形式は使われないが一応
        return float(s.replace('"',''))
    if '/' in s:
        a,b=s.split('/'); return float(a)/float(b)
    return float(s)

def parse(path, win_start, win_end):
    # win_start/end: "HH:MM" 文字列でその範囲のSHOTだけ対象
    shot_re=re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\|INF\|SHOT  \|\s*(\d+)\|\s*(\d+)\|([^|]+)\|([^|]+)\|\s*([-+][\d.]+)\|(\S+)(.*)$')
    evt=[]
    for line in open(path, encoding='utf-8', errors='replace'):
        m=shot_re.match(line.rstrip('\n'))
        if not m: continue
        d,t,frame,iso,ss,fn,ev,ccm,rest=m.groups()
        hh=int(t[:2])
        # 文字列比較でおおまかに窓を絞る(同日内)
        if not (win_start <= t <= win_end): continue
        Y=None; mev=None; late=None; prep=None; setok=True
        my=re.search(r'Y=([\d.]+)',rest)
        if my: Y=float(my.group(1))
        mm=re.search(r'ev(-?[\d.]+)',rest)
        if mm: mev=float(mm.group(1))
        ml=re.search(r'late=(-?\d+)ms',rest)
        if ml: late=int(ml.group(1))
        mp=re.search(r'prep=(\d+)ms',rest)
        if mp: prep=int(mp.group(1))
        if 'set=' in rest and 'NG' in rest.split('set=')[1][:30]:
            setok=False
        evt.append(dict(t=t,frame=int(frame),iso=int(iso),ss=ss.strip(),fn=fn.strip(),
                        ev=float(ev),Y=Y,mev=mev,late=late,prep=prep,ccm=ccm,setok=setok))
    return evt

def secs(t):
    h,m,s=t.split(':'); return int(h)*3600+int(m)*60+int(s)

def split_streams(evt):
    # 貪欲2ストリーム分離。frame==次に期待する番号で振り分け。
    A=[]; B=[]; nextA=1; nextB=1
    for e in evt:
        f=e['frame']
        okA = (f==nextA); okB=(f==nextB)
        if okA and okB:
            # 両方候補: 直前ショットが約15s前のストリームへ
            la = secs(A[-1]['t']) if A else -999
            lb = secs(B[-1]['t']) if B else -999
            ta = secs(e['t'])
            # Aが未開始(空)ならA
            if not A: A.append(e); nextA+=1
            elif not B: B.append(e); nextB+=1
            else:
                if abs(ta-la-15) <= abs(ta-lb-15): A.append(e); nextA+=1
                else: B.append(e); nextB+=1
        elif okA:
            A.append(e); nextA+=1
        elif okB:
            B.append(e); nextB+=1
        else:
            # どちらの期待にも合わない(ドロップ等)。近い方へ寄せる。
            if abs(f-nextA) <= abs(f-nextB):
                A.append(e); nextA=f+1
            else:
                B.append(e); nextB=f+1
    return A,B

def summarize(name, s):
    if not s:
        print(f"[{name}] 空"); return
    n=len(s)
    isos=set(x['iso'] for x in s)
    # ev単調性: 露出は薄明で単調に開く/閉じるはず。ev(制御値=明るさ)の折り返し回数
    evs=[x['ev'] for x in s]
    # 露出設定失敗
    ngset=sum(1 for x in s if not x['setok'])
    # 測光値Yの範囲(測光したコマのみ)
    Ys=[x['Y'] for x in s if x['Y'] is not None]
    print(f"[{name}] コマ={n} frame {s[0]['frame']}..{s[-1]['frame']}  {s[0]['t']}..{s[-1]['t']}")
    print(f"   ISO種類={sorted(isos)}  set失敗={ngset}")
    if Ys:
        print(f"   Y(測光) min={min(Ys):.4f} max={max(Ys):.4f}  測光コマ={len(Ys)}")

def dump_transition(name, s, lo, hi):
    print(f"--- {name} 露出推移 {lo}..{hi} (間引き) ---")
    prev=None
    for x in s:
        if not (lo<=x['t']<=hi): continue
        line=f"  {x['t']} f{x['frame']:>3} ISO{x['iso']:>4} SS{x['ss']:>7} f/{x['fn']} ev{x['ev']:+.2f} ccm={x['ccm']}"
        if x['Y'] is not None: line+=f" Y={x['Y']:.4f}"
        print(line)

logE="Z:\\projects\\cameraControl\\projects\\HolyGrailController\\HolyGrailController\\_retrieved_logs\\hg_2026-07-20.log"
logD="Z:\\projects\\cameraControl\\projects\\HolyGrailController\\HolyGrailController\\_retrieved_logs\\hg_2026-07-21.log"

print("========== 夕方 (18:24-22:00) ==========")
ev=parse(logE,"18:24","22:00")
A,B=split_streams(ev)
# 先に始まった方(A)がEveR10(18:25:17〜)、後がEveR100(18:26:43〜)
summarize("EveR10 ", A)
summarize("EveR100", B)

print("\n========== 明朝 (02:00-05:30) ==========")
dv=parse(logD,"02:00","05:30")
A2,B2=split_streams(dv)
# レポートのコマ数で正しく対応付け: DawnR10=826, DawnR100=840。先起動(A2=840)=DawnR100
if len(A2) > len(B2):
    DawnR100, DawnR10 = A2, B2
else:
    DawnR100, DawnR10 = B2, A2
summarize("DawnR10 ", DawnR10)
summarize("DawnR100", DawnR100)

def osc_metric(name, s):
    # 振動指標: 測光コマ列で「目標との差(mev)」の符号反転回数と、ev制御値の1コマ差の符号反転。
    seq=[x for x in s if x['mev'] is not None]
    if len(seq)<3:
        print(f"   [{name}] 測光コマ<3"); return
    # ev制御値(明るさAPEX)の階差の符号反転回数
    ev=[x['ev'] for x in seq]
    dev=[ev[i+1]-ev[i] for i in range(len(ev)-1)]
    rev=0
    for i in range(len(dev)-1):
        if dev[i]!=0 and dev[i+1]!=0 and (dev[i]>0)!=(dev[i+1]>0): rev+=1
    # |mev| (目標残差)の平均・最大
    am=[abs(x['mev']) for x in seq]
    print(f"   [{name}] 測光={len(seq)} ev階差の符号反転={rev} ({100*rev/max(1,len(dev)):.0f}%)  |目標残差ev| 平均={sum(am)/len(am):.2f} 最大={max(am):.2f}")

print("\n===== 露出制御の滑らかさ(振動) =====")
osc_metric("EveR10 ", A)
osc_metric("EveR100", B)
osc_metric("DawnR10 ", DawnR10)
osc_metric("DawnR100", DawnR100)

print("\n===== 夕方の薄明 EveR10 露出推移(1分間引き) 18:45-19:35 =====")
last=None
for x in A:
    if not ("18:45"<=x['t']<="19:35"): continue
    if last and x['t'][:5]==last: continue
    last=x['t'][:5]
    l=f"  {x['t']} ISO{x['iso']:>4} SS{x['ss']:>7} f/{x['fn']} ev{x['ev']:+.2f} ccm={x['ccm']}"
    if x['Y'] is not None: l+=f" Y={x['Y']:.4f} 残差ev={x['mev']:+.2f}" if x['mev'] is not None else f" Y={x['Y']:.4f}"
    print(l)

print("\n===== 明朝の薄明 DawnR10 露出推移(1分間引き) 04:15-05:05 =====")
last=None
for x in DawnR10:
    if not ("04:15"<=x['t']<="05:05"): continue
    if last and x['t'][:5]==last: continue
    last=x['t'][:5]
    l=f"  {x['t']} ISO{x['iso']:>4} SS{x['ss']:>7} f/{x['fn']} ev{x['ev']:+.2f} ccm={x['ccm']}"
    if x['Y'] is not None: l+=f" Y={x['Y']:.4f} 残差ev={x['mev']:+.2f}" if x['mev'] is not None else f" Y={x['Y']:.4f}"
    print(l)

print("\n===== ccm(制御方法)切替の記録 =====")
def ccm_switches(name, s):
    prev=None; out=[]
    for x in s:
        if x['ccm']!=prev:
            out.append(f"{x['t']}→{x['ccm']}"); prev=x['ccm']
    print(f"  [{name}] "+" ".join(out))
ccm_switches("EveR10 ", A)
ccm_switches("EveR100", B)
ccm_switches("DawnR10 ", DawnR10)
ccm_switches("DawnR100", DawnR100)
