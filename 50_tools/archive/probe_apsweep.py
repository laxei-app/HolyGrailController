# -*- coding: utf-8 -*-
# 絞りスイープで擬似的に極低照度を作り、ライブビュー測光がどこで破綻(暗転)するかを見る。
# 低照度で自動ゲインが打ち消すが限界がある。絞り込むほど暗くなり、いずれ真っ暗に。
# R10がR100より早く(明るい絞りで)暗転すれば、昨夜の3段差(極低照度でR10だけ暗い)を再現。
import sys, time, json, http.client, functools
print = functools.partial(print, flush=True)
PORT=8080
def req(ip, method, path, body=None, timeout=12):
    c=http.client.HTTPConnection(ip,PORT,timeout=timeout)
    try:
        data=None; hd={}
        if body is not None:
            data=json.dumps(body).encode(); hd["Content-Type"]="application/json"
        c.request(method,path,body=data,headers=hd)
        r=c.getresponse(); raw=r.read(); return r.status, dict(r.getheaders()), raw
    finally:
        c.close()
def hist_median(y):
    total=sum(y)
    if total<=0: return 0.0
    half=total/2.0; cum=0.0; n=len(y)
    for k in range(n):
        b=y[k]
        if cum+b>=half: return (k+((half-cum)/b if b>0 else 0))/(n-1)
        cum+=b
    return 1.0
FLIP="/ccapi/ver100/shooting/liveview/flipdetail"
def lvY(ip):
    st,hd,raw=req(ip,"GET",FLIP+"?kind=info",timeout=8)
    if st!=200 or len(raw)<10 or raw[0]!=0xFF: return None
    try:
        j=json.loads(raw[7:-2].decode("utf-8","replace"))
        return hist_median(j["liveviewdata"]["histogram"][0])
    except Exception: return None
def put(ip,name,val): return req(ip,"PUT","/ccapi/ver100/shooting/settings/"+name,{"value":val})[0]
ip=sys.argv[1]; tag=sys.argv[2] if len(sys.argv)>2 else ip
iso=sys.argv[3] if len(sys.argv)>3 else "1600"
tv =sys.argv[4] if len(sys.argv)>4 else "1\""
aps=["f1.4","f2.0","f2.8","f4.0","f5.6","f8","f11","f16","f22"]
for disp in ("keep","on","off"):
    st,hd,raw=req(ip,"POST","/ccapi/ver100/shooting/liveview",{"liveviewsize":"small","cameradisplay":disp})
    if 200<=st<=204: break
put(ip,"iso",iso); put(ip,"tv",tv)
print("==== %s  ISO=%s tv=%s  絞りスイープ(疑似減光) ====" % (tag,iso,tv))
print("  av     Y_lv")
for av in aps:
    if put(ip,"av",av) not in (200,201,204):
        print("  %-6s (設定不可)"%av); continue
    time.sleep(2.0)
    ys=[y for y in (lvY(ip) for _ in range(4)) if y is not None]
    y = sorted(ys)[len(ys)//2] if ys else None
    print("  %-6s %s" % (av, ("%.4f"%y if y is not None else "無効")))
