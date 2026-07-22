# -*- coding: utf-8 -*-
# 撮影→画像取得のデバッグ。event/polling(long) と contents列挙 の両方を試す。
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
        r=c.getresponse(); raw=r.read()
        return r.status, dict(r.getheaders()), raw
    finally:
        c.close()
ip=sys.argv[1] if len(sys.argv)>1 else "192.168.1.12"

# 撮影前の最新ファイルを列挙(contents)
def list_contents_tail(ip):
    st,hd,raw=req(ip,"GET","/ccapi/ver130/contents/sd?kind=list&page=1")
    print("  contents/sd?kind=list st=%d body=%r" % (st, raw[:200]))
    # ディレクトリ列挙 → 最後のディレクトリ → ファイル列挙
    st,hd,raw=req(ip,"GET","/ccapi/ver130/contents/sd")
    print("  contents/sd st=%d body=%r" % (st, raw[:300]))
    return

print("== settings tv=1/30 ==")
print(req(ip,"PUT","/ccapi/ver100/shooting/settings/tv",{"value":"1/30"})[0])
print("== contents before ==")
list_contents_tail(ip)
print("== drain events (long once) ==")
st,hd,raw=req(ip,"GET","/ccapi/ver100/event/polling?timeout=immediately")
print("  drain st=%d body=%r" % (st, raw[:200]))
print("== SHOOT (shutterbutton af=false) ==")
st,hd,raw=req(ip,"POST","/ccapi/ver100/shooting/control/shutterbutton",{"af":False})
print("  shutter st=%d body=%r" % (st, raw[:200]))
time.sleep(2.0)
print("== event/polling?timeout=long ==")
st,hd,raw=req(ip,"GET","/ccapi/ver100/event/polling?timeout=long",timeout=20)
print("  poll(long) st=%d body=%r" % (st, raw[:400]))
print("== contents after ==")
list_contents_tail(ip)
