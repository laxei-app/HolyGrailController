import re
def parse(path):
    recs=[]
    for ln in open(path, encoding="utf-8", errors="replace"):
        f=[x.strip() for x in ln.rstrip("\n").split("|")]
        if len(f)<3 or f[2]!="SHOT": continue
        try:
            frame=int(f[3]); lum=float(f[7]); detail=f[8] if len(f)>8 else ""
        except: continue
        m=re.search(r"Y=([0-9.]+)\s+ev([+-]?[0-9.]+)", detail)
        recs.append({"t":f[0][11:],"frame":frame,"iso":f[4],"ss":f[5],"fn":f[6],
                     "lum":lum,"ccm":detail.split()[0] if detail else "?",
                     "Y":float(m.group(1)) if m else None,"mev":float(m.group(2)) if m else None})
    return recs
def show(recs, idx, before=5, after=12):
    a=max(0,idx-before); b=min(len(recs),idx+after)
    for i in range(a,b):
        r=recs[i]; mark=" <<<境目" if i==idx else ""
        Y = f"Y={r['Y']:.4f}" if r['Y'] is not None else "Y=  -   "
        mev=f"ev{r['mev']:+.2f}" if r['mev'] is not None else "ev  -  "
        print(f"  {r['t']} f{r['frame']:4d} {r['ccm']:9} lum{r['lum']:+6.2f}  {Y} {mev}  iso{r['iso']:>5} ss{r['ss']:<10} f{r['fn']}{mark}")
def boundaries(recs):
    bs=[]
    for i in range(1,len(recs)):
        if recs[i]["ccm"]!=recs[i-1]["ccm"]: bs.append(i)
    return bs
for path in ["hg_2026-06-21.log","hg_2026-06-22.log"]:
    recs=parse(path)
    print("="*86); print(path)
    for idx in boundaries(recs):
        print(f"-- 境目: {recs[idx-1]['ccm']} -> {recs[idx]['ccm']}  ({recs[idx]['t']}) --")
        show(recs, idx)
