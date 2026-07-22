import re, sys

def parse(path):
    recs=[]
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln=ln.rstrip("\n")
        f=[x.strip() for x in ln.split("|")]
        if len(f)<3: continue
        ev=f[2]
        if ev!="SHOT": 
            recs.append({"t":f[0],"ev":ev,"raw":ln}); continue
        try:
            frame=int(f[3]); iso=f[4]; ss=f[5]; fn=f[6]; lum=float(f[7]); detail=f[8] if len(f)>8 else ""
        except: 
            continue
        m=re.search(r"Y=([0-9.]+)\s+ev([+-]?[0-9.]+)", detail)
        Y=float(m.group(1)) if m else None
        mev=float(m.group(2)) if m else None
        ccm=detail.split()[0] if detail else "?"
        recs.append({"t":f[0],"ev":"SHOT","frame":frame,"iso":iso,"ss":ss,"fn":fn,"lum":lum,"ccm":ccm,"Y":Y,"mev":mev})
    return recs

def runs(recs):
    out=[]
    for r in recs:
        if r["ev"]!="SHOT": continue
        if out and out[-1]["ccm"]==r["ccm"]:
            g=out[-1]; g["n"]+=1; g["t1"]=r["t"]; g["f1"]=r["frame"]; g["lum1"]=r["lum"]
            g["lmin"]=min(g["lmin"],r["lum"]); g["lmax"]=max(g["lmax"],r["lum"])
        else:
            out.append({"ccm":r["ccm"],"t0":r["t"],"t1":r["t"],"f0":r["frame"],"f1":r["frame"],
                        "n":1,"lum0":r["lum"],"lum1":r["lum"],"lmin":r["lum"],"lmax":r["lum"]})
    return out

for path in ["hg_2026-06-21.log","hg_2026-06-22.log"]:
    recs=parse(path)
    rs=runs(recs)
    print("="*78); print(path)
    print(f"{'ccm':9} {'start':19} {'end':8} {'n':>4} {'lum0':>6} {'lum1':>6} {'lmin':>6} {'lmax':>6}")
    for g in rs:
        print(f"{g['ccm']:9} {g['t0']} {g['t1'][11:]} {g['n']:4d} {g['lum0']:6.2f} {g['lum1']:6.2f} {g['lmin']:6.2f} {g['lmax']:6.2f}")
    # フラグメント数(種別ごとの連続区間の数)
    from collections import Counter
    c=Counter(g["ccm"] for g in rs)
    print("区間数(連続ラン):", dict(c), " 総ラン数:", len(rs))
