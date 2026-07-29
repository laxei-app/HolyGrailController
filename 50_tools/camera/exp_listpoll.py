# -*- coding: utf-8 -*-
# 実験6: event/polling を一切使わず、contents(ファイルリスト)の総数監視で新規画像を検知し、
# サムネイルを取得する。これでも劣化するか=event/pollingが関与しているかの切り分け。
import json, time, urllib.request, sys
B=f"http://{sys.argv[1]}:8080"; MINS=float(sys.argv[2]) if len(sys.argv)>2 else 16
def get(p,t=15): return urllib.request.urlopen(B+p,timeout=t).read()
def post(p,b,t=15):
    r=urllib.request.Request(B+p,method="POST",data=json.dumps(b).encode(),headers={"Content-Type":"application/json"})
    return urllib.request.urlopen(r,timeout=t).read()
cat=json.loads(get("/ccapi"))
def find(s):
    h=[a["path"] for v,l in cat.items() if isinstance(l,list) for a in l if a.get("path","").endswith(s)]
    return h[-1] if h else None
SHUT,CONT,POLL=find("control/shutterbutton"),find("/contents"),find("/event/polling")
# event/polling は開始しない。念のため停止だけしておく
try: urllib.request.urlopen(urllib.request.Request(B+POLL,method="DELETE"),timeout=10).read()
except Exception: pass
card=json.loads(get(CONT))["path"][-1]; DIR=json.loads(get(card))["path"][-1]
def count(): return json.loads(get(DIR+"?type=all&kind=number"))["contentsnumber"]
def latest():
    n=json.loads(get(DIR+"?type=all&kind=number")); pg=n.get("pagenumber",1)
    return json.loads(get(DIR+f"?type=all&kind=list&page={pg}"))["path"][-1]
base=count()
print(f"# 開始時ファイル数={base} (event/polling 不使用)")
print("# n,elapsed_s,wait_ms,fetch_ms,note")
t_all=time.time(); n=0
while time.time()-t_all < MINS*60:
    n+=1; t_sh=time.perf_counter()
    try: post(SHUT,{"af":False})
    except Exception as e:
        print(f"{n},{time.time()-t_all:.0f},,,shutter_err:{type(e).__name__}",flush=True); time.sleep(2); continue
    note="ok"; found=False
    while time.perf_counter()-t_sh < 20:
        try:
            if count() > base: base += 1; found=True; break
        except Exception: pass
        time.sleep(0.3)
    wait_ms=(time.perf_counter()-t_sh)*1000
    fetch_ms=0.0
    if found:
        t_f=time.perf_counter()
        try: get(latest()+"?kind=thumbnail",15)
        except Exception as e: note=f"fetch_err:{type(e).__name__}"
        fetch_ms=(time.perf_counter()-t_f)*1000
    else: note="no_newfile"
    print(f"{n},{time.time()-t_all:.0f},{wait_ms:.0f},{fetch_ms:.0f},{note}",flush=True)
    rest=15.0-(time.perf_counter()-t_sh)
    if rest>0: time.sleep(rest)
