# -*- coding: utf-8 -*-
# 実験4: 実運用の測光ループを完全再現(撮影→event/polling待ち→サムネイル取得)し、
# wait が時間とともに伸びるかを見る。2026-07-29 R10の劣化再現用。
import json, time, urllib.request, sys
B=f"http://{sys.argv[1]}:8080"
MINS=float(sys.argv[2]) if len(sys.argv)>2 else 15
def get(p,t=15): return urllib.request.urlopen(B+p,timeout=t).read()
def post(p,b,t=15):
    r=urllib.request.Request(B+p,method="POST",data=json.dumps(b).encode(),
                             headers={"Content-Type":"application/json"})
    return urllib.request.urlopen(r,timeout=t).read()
cat=json.loads(get("/ccapi"))
def find(s):
    h=[a["path"] for v,l in cat.items() if isinstance(l,list) for a in l if a.get("path","").endswith(s)]
    return h[-1] if h else None
SHUT,POLL=find("control/shutterbutton"),find("/event/polling")
try: urllib.request.urlopen(urllib.request.Request(B+POLL,method="DELETE"),timeout=10).read()
except Exception: pass
print("# n,elapsed_s,wait_ms,fetch_ms,note")
t_all=time.time(); n=0
while time.time()-t_all < MINS*60:
    n+=1; t_sh=time.perf_counter()
    try: post(SHUT,{"af":False})
    except Exception as e:
        print(f"{n},{time.time()-t_all:.0f},,,shutter_err:{type(e).__name__}",flush=True)
        time.sleep(2); continue
    path=None; note="ok"
    while time.perf_counter()-t_sh < 20:
        try:
            b=get(POLL,15)
            if b'"addedcontents"' in b:
                ac=json.loads(b).get("addedcontents") or []
                if ac: path=ac[-1]; break
        except Exception: pass
        time.sleep(0.2)
    wait_ms=(time.perf_counter()-t_sh)*1000
    fetch_ms=0.0
    if path:
        p = path if path.startswith("/") else "/"+path.split("://")[-1].split("/",1)[1]
        t_f=time.perf_counter()
        try: get(p+"?kind=thumbnail",15)
        except Exception as e: note=f"fetch_err:{type(e).__name__}"
        fetch_ms=(time.perf_counter()-t_f)*1000
    else: note="no_event"
    print(f"{n},{time.time()-t_all:.0f},{wait_ms:.0f},{fetch_ms:.0f},{note}",flush=True)
    rest=15.0-(time.perf_counter()-t_sh)
    if rest>0: time.sleep(rest)
