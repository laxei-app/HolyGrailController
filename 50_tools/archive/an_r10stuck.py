# -*- coding: utf-8 -*-
# R10(long: ss>=4=8秒系)とR100(short: ss<4)を分けて夜明けの露出/測光を時系列表示。
import re, collections
RE = re.compile(r'^(\d{2}:\d{2}:\d{2})\|INF\|SHOT  \|\s*\d+\|\s*(\d+)\|([^|]+)\|\s*([\d.]+)\s*\|\s*([+\-][\d.]+)\|(\w+) Y=([\d.]+) ev([+\-][\d.]+)')
def rows(fn):
    out=[]
    for ln in open(fn, encoding='utf-8', errors='replace'):
        if '|SHOT' not in ln: continue
        m = RE.match(ln[11:])
        if not m: continue
        t,iso,ss,fn2,G,band,Y,ev = m.groups()
        out.append((t, int(iso), ss.strip(), G, band, float(Y), float(ev)))
    return out
def ssval(s):
    if '/' in s:
        a,b=s.split('/'); return float(a)/float(b)
    try: return float(s)
    except: return -1
allrows=[]
for f in ["hg_2026-07-17.log","hg_2026-07-18.log"]:
    allrows += rows(f)
tbl=collections.OrderedDict()
for (t,iso,ss,G,band,Y,ev) in allrows:
    stream = 'long' if ssval(ss)>=4 else 'short'
    mm=t[:5]
    tbl.setdefault(mm,{'long':None,'short':None})
    tbl[mm][stream]=(iso,ss,G,Y,ev,band)
def inr(mm):
    h,m=mm.split(':'); tot=int(h)*60+int(m)
    return (2*60+30) <= tot <= (4*60+30) and (int(m)%2==0)
def fmt(x):
    if not x: return f"{'-':<5} {'-':<4} {'-':<7} {'-':<6} {'-':<5}"
    iso,ss,G,Y,ev,band=x
    return f"{iso:<5} {ss:<4} G{G:<6} Y{Y:<6.4f} ev{ev:<+5.2f} {band[:5]}"
print("time  |  R10 (ss>=4 / 8秒系)                          |  R100 (ss<4)")
for mm,d in tbl.items():
    if not inr(mm): continue
    print(f"{mm} | {fmt(d['long'])} | {fmt(d['short'])}")
