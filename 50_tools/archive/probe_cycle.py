# -*- coding: utf-8 -*-
# 撮影サイクル再現: 実際に8秒露光を繰り返し、その合間に測光(flipdetail?kind=info)して
#   Y と 取得所要時間 を見る。昨夜のように「露光を挟むとR10の測光が黒く固まる」かを検証。
# 各露光の直後から複数回測ってライブビューの回復曲線も見る。
import sys, time, json, http.client, functools
print = functools.partial(print, flush=True)
PORT=8080
def req(ip, method, path, body=None, timeout=25):
    c=http.client.HTTPConnection(ip,PORT,timeout=timeout)
    try:
        data=None; hd={}
        if body is not None:
            data=json.dumps(body).encode(); hd["Content-Type"]="application/json"
        c.request(method,path,body=data,headers=hd)
        r=c.getresponse(); raw=r.read(); return r.status, raw
    finally:
        c.close()
def hist_median(y):
    total=sum(y)
    if total<=0: return 0.0
    half=total/2.0; cum=0.0; n=len(y)
    for k in range(n):
        b=y[k]
        if cum+b>=half:
            return (k+((half-cum)/b if b>0 else 0))/(n-1)
        cum+=b
    return 1.0
def meter(ip,flip):
    t0=time.time()
    st,raw=req(ip,"GET",flip+"?kind=info",timeout=8)
    dt=(time.time()-t0)*1000.0
    if st!=200 or len(raw)<10 or raw[0]!=0xFF: return None,dt
    try:
        j=json.loads(raw[7:-2].decode("utf-8","replace"))
        return hist_median(j["liveviewdata"]["histogram"][0]), dt
    except Exception: return None,dt
def put(ip,name,val): return req(ip,"PUT","/ccapi/ver100/shooting/settings/"+name,{"value":val})[0]

ip=sys.argv[1]; tag=sys.argv[2] if len(sys.argv)>2 else ip
flip="/ccapi/ver100/shooting/liveview/flipdetail"
for disp in ("keep","on","off"):
    st,raw=req(ip,"POST","/ccapi/ver100/shooting/liveview",{"liveviewsize":"small","cameradisplay":disp})
    if 200<=st<=204: break
put(ip,"iso","1600"); put(ip,"av","f1.4"); put(ip,"tv","8\"")
print("==== %s  ISO1600 f1.4 tv=8\" 撮影サイクル再現 ====" % tag)
print("  [定常] 露光前の測光(連続LV):")
for i in range(3):
    y,dt=meter(ip,flip); print("     Y=%.4f (%.0fms)"%(y if y else -1,dt)); time.sleep(0.5)
for cyc in range(4):
    print("  -- 8秒露光 #%d --" % (cyc+1))
    t0=time.time()
    st,raw=req(ip,"POST","/ccapi/ver100/shooting/control/shutterbutton",{"af":False},timeout=30)
    print("     shutter st=%d (%.1fs)"%(st,time.time()-t0))
    # 露光直後から測光を連打して回復を見る
    for k in range(8):
        y,dt=meter(ip,flip)
        print("     +%d: Y=%.4f (%.0fms)"%(k,y if y is not None else -1,dt))
        time.sleep(0.7)
