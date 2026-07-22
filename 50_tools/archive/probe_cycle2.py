# -*- coding: utf-8 -*-
# アプリ忠実再現: 8秒露光 → 「シャッター切り終わり(=ライブビュー再開)」から4秒後に1回だけ測光。
# これを数サイクル繰り返し、R10の測光値が昨夜のように ~0.004 に貼り付くかを見る。
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
        if cum+b>=half: return (k+((half-cum)/b if b>0 else 0))/(n-1)
        cum+=b
    return 1.0
FLIP="/ccapi/ver100/shooting/liveview/flipdetail"
def meter(ip):
    t0=time.time()
    st,raw=req(ip,"GET",FLIP+"?kind=info",timeout=8)
    dt=(time.time()-t0)*1000.0
    if st!=200 or len(raw)<10 or raw[0]!=0xFF: return None,dt  # 無効(露光中/未回復)
    try:
        j=json.loads(raw[7:-2].decode("utf-8","replace"))
        return hist_median(j["liveviewdata"]["histogram"][0]), dt
    except Exception: return None,dt
def valid(ip):
    y,_=meter(ip); return y is not None
def put(ip,name,val): return req(ip,"PUT","/ccapi/ver100/shooting/settings/"+name,{"value":val})[0]

ip=sys.argv[1]; tag=sys.argv[2] if len(sys.argv)>2 else ip
for disp in ("keep","on","off"):
    st,raw=req(ip,"POST","/ccapi/ver100/shooting/liveview",{"liveviewsize":"small","cameradisplay":disp})
    if 200<=st<=204: break
put(ip,"iso","1600"); put(ip,"av","f1.4"); put(ip,"tv","8\"")
print("==== %s  ISO1600 f1.4 tv=8\"  アプリ忠実(切り終わり+5秒で1回測光, その2秒後に次コマ) ====" % tag)
# 定常(露光なし)の基準
by,bdt=meter(ip); print("  [基準] 露光前 Y=%s (%.0fms)" % (("%.4f"%by if by is not None else "無効"), bdt))
for cyc in range(6):
    st,raw=req(ip,"POST","/ccapi/ver100/shooting/control/shutterbutton",{"af":False},timeout=30)
    if st==503:
        print("  #%d shutter=503(busy) 3秒待機"%(cyc+1)); time.sleep(3.0); continue
    t_post=time.time()
    # 8秒露光の終了を待つ(露光開始でLVが無効化→再有効化を検出。取れなければ固定待ち)
    while time.time()-t_post<12 and valid(ip): time.sleep(0.15)   # 露光開始(無効化)待ち
    t_susp=time.time()
    while time.time()-t_susp<15 and not valid(ip): time.sleep(0.15) # 露光終了(再有効化)待ち
    t_end=time.time()
    expo=t_end-t_post
    if expo<3.0:  # 検出失敗時は露光8秒とみなし固定
        time.sleep(max(0.0, 8.0-(time.time()-t_post)))
        t_end=t_post+8.0
    # 切り終わりから5秒後に1回だけ測光(アプリ同一)
    time.sleep(5.0 - max(0.0, time.time()-t_end))
    y,dt=meter(ip)
    print("  #%d  露光終了+5s: Y=%s (%.0fms)  [露光検出 %.1fs]" %
          (cyc+1, ("%.4f"%y if y is not None else "無効"), dt, expo))
    time.sleep(2.0)  # その2秒後に次コマ(=次ループのPOST)
