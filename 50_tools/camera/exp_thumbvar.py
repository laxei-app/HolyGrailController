# -*- coding: utf-8 -*-
# 実験5: サムネイル取得の「何が」R10を壊すのかを切り分ける。
#   every=N   : Nコマに1回だけ取得(頻度の影響)
#   kind=X    : thumbnail/display(エンドポイントの影響)
#   close=1   : 取得時に Connection: close を付ける(接続蓄積の影響)
import json, time, urllib.request, sys
B=f"http://{sys.argv[1]}:8080"
MINS=float(sys.argv[2]); EVERY=int(sys.argv[3]); KIND=sys.argv[4]; CLOSE=len(sys.argv)>5 and sys.argv[5]=="1"
def get(p,t=15,close=False):
    r=urllib.request.Request(B+p)
    if close: r.add_header("Connection","close")
    return urllib.request.urlopen(r,timeout=t).read()
def post(p,b,t=15):
    r=urllib.request.Request(B+p,method="POST",data=json.dumps(b).encode(),headers={"Content-Type":"application/json"})
    return urllib.request.urlopen(r,timeout=t).read()
cat=json.loads(get("/ccapi"))
def find(s):
    h=[a["path"] for v,l in cat.items() if isinstance(l,list) for a in l if a.get("path","").endswith(s)]
    return h[-1] if h else None
SHUT,POLL=find("control/shutterbutton"),find("/event/polling")
try: urllib.request.urlopen(urllib.request.Request(B+POLL,method="DELETE"),timeout=10).read()
except Exception: pass
print(f"# every={EVERY} kind={KIND} close={CLOSE}")
print("# n,elapsed_s,wait_ms,fetch_ms,note")
t_all=time.time(); n=0
while time.time()-t_all < MINS*60:
    n+=1; t_sh=time.perf_counter()
    try: post(SHUT,{"af":False})
    except Exception as e:
        print(f"{n},{time.time()-t_all:.0f},,,shutter_err:{type(e).__name__}",flush=True); time.sleep(2); continue
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
    if path and (n % EVERY == 0):
        p = path if path.startswith("/") else "/"+path.split("://")[-1].split("/",1)[1]
        t_f=time.perf_counter()
        try: get(p+f"?kind={KIND}",15,CLOSE)
        except Exception as e: note=f"fetch_err:{type(e).__name__}"
        fetch_ms=(time.perf_counter()-t_f)*1000
    elif not path: note="no_event"
    print(f"{n},{time.time()-t_all:.0f},{wait_ms:.0f},{fetch_ms:.0f},{note}",flush=True)
    rest=15.0-(time.perf_counter()-t_sh)
    if rest>0: time.sleep(rest)
