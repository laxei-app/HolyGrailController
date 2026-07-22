import re
recs=[]
for ln in open("hg_2026-06-21.log", encoding="utf-8", errors="replace"):
    f=[x.strip() for x in ln.rstrip("\n").split("|")]
    if len(f)<3 or f[2]!="SHOT": continue
    try: frame=int(f[3]); lum=float(f[7]); detail=f[8] if len(f)>8 else ""
    except: continue
    if detail.split()[0]!="day": continue
    m=re.search(r"Y=([0-9.]+)\s+ev([+-]?[0-9.]+)", detail)
    recs.append((f[0][11:],frame,lum,f[4],f[5],f[6], float(m.group(2)) if m else None))
# 露出が +6 に到達した最初のフレーム
peg=None
for i,r in enumerate(recs):
    if r[2]>=6.0: peg=i; break
print("day 総枚数:", len(recs))
print("-- 露出上限(+6)到達の前後 --")
for r in recs[max(0,peg-4):peg+4]:
    print(f"  {r[0]} f{r[1]} lum{r[2]:+6.2f} 測光ev{r[6]:+.2f}  iso{r[3]} ss{r[4]} f{r[5]}")
print("-- 日中の最初/最後 --")
for r in [recs[0], recs[-1]]:
    print(f"  {r[0]} f{r[1]} lum{r[2]:+6.2f} 測光ev{r[6]:+.2f}  iso{r[3]} ss{r[4]} f{r[5]}")
# 測光evが0付近を保てていた範囲
okrange=[r for r in recs if abs(r[6])<=0.5]
if okrange:
    print(f"-- 測光ev が ±0.5 以内だったのは {okrange[0][0]} 〜 {okrange[-1][0]} --")
