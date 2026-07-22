# -*- coding: utf-8 -*-
# 通しテスト(7/17 17:00 ~ 7/18 05:30, R10+R100)の解析。
#  ・2台をシャッターのサブ秒位相で分離
#  ・② 露出振動: 自動露出区間(標準/preNight/postNight)で「適用ev(=Sv-Av-Tv)」の増減反転回数
#  ・②bm 夜明け: postNight~標準 の Y と 適用ev の推移(明るすぎないか)
#  ・測光できなかった(Y無し)の内訳: どのccmで起きているか(夜間の固定露出は測光しない=正常)
import re, datetime as dt, statistics as st, collections, math

FILES=["hg_2026-07-17.log","hg_2026-07-18.log"]
RE=re.compile(r'^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2})\|INF\|SHOT  \|'
  r'\s*(\d+)\|\s*(\d+)\|(.{0,11}?)\s*\|(.{0,6}?)\s*\|\s*([-+\d.]+)\|'
  r'(\S+)(?: Y=([\d.]+) ev([-+][\d.]+))?'
  r'.*?sh=(\d{2}):(\d{2}):(\d{2})\.(\d{3})')

rows=[]
for f in FILES:
  for ln in open(f,encoding="utf-8",errors="replace"):
    m=RE.match(ln)
    if not m: continue
    d,t,fr,iso,ss,fn,ev,ccm,Y,evm,H,M,S,MS=m.groups()
    dtm=dt.datetime.strptime(d+" "+t,"%Y-%m-%d %H:%M:%S")
    rows.append(dict(t=dtm,fr=int(fr),iso=iso,ss=ss.strip(),fn=fn.strip(),ev=float(ev),
                     ccm=ccm,Y=float(Y) if Y else None, evm=float(evm) if evm else None,
                     subms=int(MS)))
rows=[r for r in rows if dt.datetime(2026,7,17,17,0)<=r['t']<=dt.datetime(2026,7,18,5,31)]
rows.sort(key=lambda r:r['t'])
print("通し窓のSHOT:",len(rows))

# --- 2台分離: サブ秒位相(mod 15秒周期での位置)。R10とR100でsh末尾が異なる。
# 各コマの sh のサブ秒(0-999ms)でクラスタリング。2峰に分かれるはず。
subs=[r['subms'] for r in rows]
# k=2 の簡易分類: 中央値で割る
med=st.median(subs)
A=[r for r in rows if r['subms']<med]
B=[r for r in rows if r['subms']>=med]
# どっちがどっちか iso初期などで判断せず、frが1から始まる方の初期ssで見る
def label(s):
    s2=sorted(s,key=lambda r:r['t'])[:3]
    return "iso%s ss%s"%(s2[0]['iso'],s2[0]['ss']) if s2 else "?"
print(f"ストリームA n={len(A)} 位相<{med:.0f}ms  初期={label(A)}")
print(f"ストリームB n={len(B)} 位相>={med:.0f}ms 初期={label(B)}")

AUTO={'標準','preNight','postNight'}  # 測光する区間
def analyze(s,nm):
    s=sorted(s,key=lambda r:r['t'])
    print("\n"+"="*66)
    print(f"### {nm}  n={len(s)}  {s[0]['t']} ~ {s[-1]['t']}")
    # 測光できなかった内訳
    noY=collections.Counter(r['ccm'] for r in s if r['Y'] is None)
    withY=collections.Counter(r['ccm'] for r in s if r['Y'] is not None)
    print("測光なし(Y無し)コマ の ccm内訳:", dict(noY))
    print("測光あり(Y有り)コマ の ccm内訳:", dict(withY))
    # ② 振動: 自動露出区間で適用evの1コマ差の符号反転
    for seg in ('標準','preNight','postNight'):
        w=[r for r in s if r['ccm']==seg and r['Y'] is not None]
        if len(w)<20: continue
        evs=[r['ev'] for r in w]
        dd=[b-a for a,b in zip(evs,evs[1:])]
        nz=[x for x in dd if abs(x)>1e-9]
        flips=sum(1 for a,b in zip(nz,nz[1:]) if a*b<0)
        # Y の中庸(0.18)からのブレ
        ys=[r['Y'] for r in w]
        logerr=[abs(math.log2(max(y,1e-6)/0.18)) for y in ys]
        print(f"  [{seg}] n={len(w)} ev幅={max(evs)-min(evs):.2f}段 反転={flips}({100*flips/max(len(nz),1):.0f}%of{len(nz)}) |log2(Y/.18)|平均={st.mean(logerr):.2f}段")

analyze(A,"ストリームA")
analyze(B,"ストリームB")
