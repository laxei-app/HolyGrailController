# -*- coding: utf-8 -*-
# 「ライブビュー測光が実際の静止画露出を反映しない」ことを実証するプローブ。
#   固定シーン・固定ISO/F値でシャッターだけをスイープし、各段で
#     Y_lv    = liveview/flipdetail?kind=info の histogram(Y) 中央値  … アプリが測光に使う値そのもの
#     Y_still = その設定で実撮影→JPEG取得→輝度中央値              … 真の露出(グラウンドトゥルース)
#   を測って並べる。ライブビューが忠実なら両者は一致してシャッター1段ごとに約1段ずつ上がる。
#   Y_lv が途中で頭打ちになり Y_still だけ上がり続ければ「ライブビューは信用できない」の証明。
# アプリ/エッジは一切変更しない。カメラのCCAPIを直接叩くだけ。
import sys, time, json, http.client, io, functools
from PIL import Image
print = functools.partial(print, flush=True)

PORT = 8080
def req(ip, method, path, body=None, timeout=20):
    c = http.client.HTTPConnection(ip, PORT, timeout=timeout)
    try:
        data=None; hd={}
        if body is not None:
            data=json.dumps(body).encode(); hd["Content-Type"]="application/json"
        c.request(method, path, body=data, headers=hd)
        r=c.getresponse(); raw=r.read()
        return r.status, dict(r.getheaders()), raw
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

def get_lv_histY(ip, flip):
    # flipdetail?kind=info: 先頭7B/末尾2Bを外したJSONの liveviewdata.histogram[0] が Yビン
    st,hd,raw = req(ip,"GET",flip+"?kind=info",timeout=8)
    if st!=200 or len(raw)<10 or raw[0]!=0xFF:
        return None, st
    try:
        j=json.loads(raw[7:-2].decode("utf-8","replace"))
        return j["liveviewdata"]["histogram"][0], st
    except Exception:
        return None, st

def put_setting(ip, name, value):
    st,hd,raw = req(ip,"PUT","/ccapi/ver100/shooting/settings/"+name, {"value":value})
    return st, raw

def newest_file(ip):
    # ストレージ(card1/sd)→最後のフォルダ→最後のファイルURL を動的にたどる
    st,hd,raw=req(ip,"GET","/ccapi/ver130/contents")
    stor=json.loads(raw.decode()).get("path",[])
    if not stor: return None
    st,hd,raw=req(ip,"GET",stor[-1])
    folders=json.loads(raw.decode()).get("path",[])
    if not folders: return None
    folder=folders[-1]
    st,hd,raw=req(ip,"GET",folder+"?kind=number")
    n=json.loads(raw.decode()).get("contentsnumber",0)
    if n<=0: return None
    page=(n+99)//100
    st,hd,raw=req(ip,"GET",folder+"?kind=list&page=%d"%page)
    files=json.loads(raw.decode()).get("path",[])
    return files[-1] if files else None

def shoot_and_measure_still(ip, wait_s):
    before=newest_file(ip)
    st,hd,raw = req(ip,"POST","/ccapi/ver100/shooting/control/shutterbutton",{"af":False},timeout=max(25,wait_s+20))
    if st not in (200,201,202,203,204):
        return None, "shutter st=%d %r"%(st,raw[:80])
    # 撮影完了+記録待ち(POSTが露光ぶん待つ場合もあるが余裕を見る)
    time.sleep(2.0)
    url=None
    for _ in range(8):
        cur=newest_file(ip)
        if cur and cur!=before: url=cur; break
        time.sleep(0.8)
    if not url:
        return None, "no new file"
    # 表示用JPEG(RAWでもカメラ生成の表示画像)を取得して輝度中央値
    st3,hd3,img = req(ip,"GET",url+"?kind=display",timeout=15)
    if st3!=200 or len(img)<1000:
        return None, "download st=%d len=%d"%(st3,len(img))
    im=Image.open(io.BytesIO(img)).convert("L")
    px=list(im.getdata()); px.sort()
    med=px[len(px)//2]/255.0
    return med, url.split("/")[-1]

# --- Canon tv 文字列 <-> 秒 ---
def tv_sec(s):
    if s.startswith("1/"): return 1.0/float(s[2:])
    if '"' in s:
        a=s.split('"'); w=a[0]; f=a[1]
        return float(w)+(float(f)/10.0 if f else 0.0)
    return float(s)

def main():
    ip = sys.argv[1]
    tag= sys.argv[2] if len(sys.argv)>2 else ip
    iso= sys.argv[3] if len(sys.argv)>3 else "100"
    av = sys.argv[4] if len(sys.argv)>4 else "f4.0"
    # 1段刻みのシャッター梯子(速→遅)
    ladder = ["1/30","1/15","1/8","1/4","0\"5","1\"","2\"","4\"","8\""]
    print("==== %s (%s)  ISO=%s  AV=%s ====" % (tag, ip, iso, av))
    # liveview 開始
    flip="/ccapi/ver100/shooting/liveview/flipdetail"
    for disp in ("keep","on","off"):
        st,hd,raw=req(ip,"POST","/ccapi/ver100/shooting/liveview",{"liveviewsize":"small","cameradisplay":disp})
        if 200<=st<=204: break
    # 固定設定
    s1,_=put_setting(ip,"av",av); s2,_=put_setting(ip,"iso",iso)
    print("  set av=%s(st%d) iso=%s(st%d)" % (av,s1,iso,s2))
    print("  %-6s %-7s | %-8s | %-8s | diff(stops)" % ("tv","sec","Y_lv","Y_still"))
    base=None
    rows=[]
    for tv in ladder:
        sec=tv_sec(tv)
        st,raw=put_setting(ip,"tv",tv)
        if st not in (200,201,204):
            print("  tv=%s set失敗 st=%d %r"%(tv,st,raw[:60])); continue
        time.sleep(2.2)  # ライブビュー数フレーム落ち着かせる
        ys=[]
        for _ in range(4):
            y,stc=get_lv_histY(ip,flip)
            if y: ys.append(hist_median(y))
            time.sleep(0.4)
        ylv = sorted(ys)[len(ys)//2] if ys else float('nan')
        still, info = shoot_and_measure_still(ip, sec)
        rows.append((tv,sec,ylv,still))
        d = ""
        print("  %-6s %-7.4g | %-8.4f | %-8s | %s" % (tv, sec, ylv, ("%.4f"%still if still is not None else "ERR:"+str(info)), d))
    # まとめ: Y_lv と Y_still が何段でずれているか(明るくなり方の比較)
    print("\n  -- 1/30を基準にした各段の増分(段) --")
    print("  %-6s | dLV(段) | dStill(段)" % "tv")
    ref=rows[0]
    import math
    def st_stops(a,b):
        if a and b and a>0 and b>0: return math.log2(b/a)
        return float('nan')
    for (tv,sec,ylv,still) in rows:
        dlv = st_stops(ref[2],ylv)
        dst = st_stops(ref[3],still) if (ref[3] and still) else float('nan')
        print("  %-6s | %+6.2f | %+6.2f" % (tv, dlv, dst))

if __name__=="__main__":
    main()
