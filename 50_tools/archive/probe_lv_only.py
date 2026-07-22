# -*- coding: utf-8 -*-
# 測光専用(撮影なし)の高速スイープ。ライブビューYヒスト中央値だけをシャッター別に測る。
# 目的: 同一シーンで低照度(絞り込み)にしたとき、R10とR100の測光値の差が開くかを見る。
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
        if cum+b>=half:
            frac=(half-cum)/b if b>0 else 0.0
            return (k+frac)/(n-1)
        cum+=b
    return 1.0
def lvY(ip,flip):
    st,hd,raw=req(ip,"GET",flip+"?kind=info",timeout=8)
    if st!=200 or len(raw)<10 or raw[0]!=0xFF: return None
    try:
        j=json.loads(raw[7:-2].decode("utf-8","replace"))
        return hist_median(j["liveviewdata"]["histogram"][0])
    except Exception: return None
def put(ip,name,val): return req(ip,"PUT","/ccapi/ver100/shooting/settings/"+name,{"value":val})[0]

ip=sys.argv[1]; tag=sys.argv[2] if len(sys.argv)>2 else ip
iso=sys.argv[3] if len(sys.argv)>3 else "1600"
av =sys.argv[4] if len(sys.argv)>4 else "f8"
ladder=["1/30","1/8","0\"5","1\"","2\"","4\"","8\""]
flip="/ccapi/ver100/shooting/liveview/flipdetail"
for disp in ("keep","on","off"):
    st,hd,raw=req(ip,"POST","/ccapi/ver100/shooting/liveview",{"liveviewsize":"small","cameradisplay":disp})
    if 200<=st<=204: break
put(ip,"av",av); put(ip,"iso",iso)
print("==== %s  ISO=%s AV=%s (測光専用) ====" % (tag,iso,av))
print("  tv     sec    Y_lv")
res={}
for tv in ladder:
    put(ip,"tv",tv); time.sleep(2.2)
    ys=[y for y in (lvY(ip,flip) for _ in range(5)) if y is not None]
    time.sleep(0.1)
    y=sorted(ys)[len(ys)//2] if ys else float('nan')
    res[tv]=y
    sec = (1.0/float(tv[2:]) if tv.startswith("1/") else (float(tv.split('"')[0])+ (float(tv.split('"')[1])/10 if tv.split('"')[1] else 0)))
    print("  %-6s %-6.3g %.4f" % (tv,sec,y))
print("  -> plateau(8\") = %.4f" % res.get("8\"",float('nan')))
