# -*- coding: utf-8 -*-
import sys, json, http.client, functools
print = functools.partial(print, flush=True)
PORT=8080
def req(ip, method, path, body=None, timeout=15):
    c=http.client.HTTPConnection(ip,PORT,timeout=timeout)
    try:
        data=None; hd={}
        if body is not None:
            data=json.dumps(body).encode(); hd["Content-Type"]="application/json"
        c.request(method,path,body=data,headers=hd)
        r=c.getresponse(); raw=r.read(); return r.status, raw
    finally:
        c.close()
ip=sys.argv[1] if len(sys.argv)>1 else "192.168.1.12"
# event/polling の許可パラメータ
st,b=req(ip,"GET","/ccapi")
api=json.loads(b.decode('utf-8','replace'))
for ver in api:
    for e in api[ver]:
        if e.get('path','').endswith('/event/polling'):
            print("event/polling entry:", json.dumps(e, ensure_ascii=False))
# contents ルート
st,b=req(ip,"GET","/ccapi/ver130/contents")
print("contents root st=%d body=%r" % (st, b[:300]))
try:
    j=json.loads(b.decode());
    # 1階層ずつ降りる
    node=j
    path=None
    if isinstance(j,dict):
        # {"path":[...]} または {"url":[...]}
        for k in ('path','url','contentsnumber'):
            if k in j: print("  key",k,"=",j[k][:5] if isinstance(j[k],list) else j[k])
    # ストレージ一覧を試す
    for cand in ['/ccapi/ver130/contents/card1','/ccapi/ver130/contents/card1/100CANON']:
        st2,b2=req(ip,"GET",cand+"?kind=list&page=1")
        print("  %s st=%d body=%r" % (cand, st2, b2[:250]))
except Exception as e:
    print("parse err", e)
# event polling no-query
st,b=req(ip,"GET","/ccapi/ver100/event/polling",timeout=8)
print("event/polling (no query) st=%d body=%r" % (st, b[:250]))
