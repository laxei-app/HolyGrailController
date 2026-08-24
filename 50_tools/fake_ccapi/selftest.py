#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""偽 CCAPI カメラの自己テスト。

エッジが実際に投げる順番どおりに叩いて、応答がコード側の期待を満たすか確かめる。
期待の根拠は apiCanonCCAPI.cpp / deviceDiscovery.cpp の該当行(各テストのコメント)。
"""
import json
import re
import socket
import struct
import sys
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.4"
NG = []


def ok(cond, msg):
    print(("  OK   " if cond else "  NG   ") + msg)
    if not cond:
        NG.append(msg)


def get(url, headers=None):
    req = urllib.request.Request(url, headers=headers or {})
    with urllib.request.urlopen(req, timeout=3) as r:
        return r.status, r.read(), dict(r.headers)


def put(url, obj):
    req = urllib.request.Request(url, data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def post(url, obj):
    req = urllib.request.Request(url, data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


# ---------------------------------------------------------------- 1. SSDP
print("[1] SSDP M-SEARCH (deviceDiscovery.cpp:14-20 と同じ問い合わせ)")
q = ("M-SEARCH * HTTP/1.1\r\n"
     "HOST: 239.255.255.250:1900\r\n"
     'MAN: "ssdp:discover"\r\n'
     "ST: ssdp:all\r\n"
     "MX: 1\r\n\r\n").encode()
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(HOST))
s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 4)
s.settimeout(2.0)
s.sendto(q, ("239.255.255.250", 1900))
found = {}
try:
    while True:
        data, addr = s.recvfrom(4096)
        t = data.decode("ascii", "ignore")
        if "ICPO-CameraControlAPIService" not in t or "schemas-canon-com" not in t:
            continue
        loc = re.search(r"(?im)^location:\s*(.+)$", t)
        usn = re.search(r"(?im)^usn:\s*(.+)$", t)
        if loc and usn:
            found[usn.group(1).strip()] = loc.group(1).strip()
except socket.timeout:
    pass
s.close()
ok(len(found) > 0, "キーワード2語を含む応答が返る (detectCanonCCapi.cpp:13)")
locs = sorted(set(found.values()))
print("      LOCATION: %s" % locs)
for u in found:
    has = ("uuid:" in u) and ("urn:" in u) and (("device:" in u) or ("service:" in u))
    if not has:
        ok(False, "USN に uuid/urn/(device|service) が揃う: %s" % u)
ok(all((("uuid:" in u) and ("urn:" in u) and (("device:" in u) or ("service:" in u)))
       for u in found), "USN が analizeUsn を通る (deviceDiscovery.cpp:87-108)")

if not locs:
    print("SSDP が返らないので以降は既定ポートで続行します")
    locs = ["http://%s:49201/upnp/CameraDevDesc.xml" % HOST]

# ------------------------------------------------------- 2. ディスクリプタ
print("[2] UPnP ディスクリプタ (apiCanonCCAPI.cpp:217-246)")
loc = locs[0]
st, body, _ = get(loc)
xml = body.decode("utf-8")
ok(st == 200, "200 で返る")


def tag(name):
    """tool::getXmlTagValue と同じ探し方(前方一致・'>' は含めない)。"""
    i = xml.find("<" + name)
    if i < 0:
        return ""
    cs = xml.find(">", i) + 1
    e = xml.find("</" + name + ">", cs)
    return xml[cs:e] if e > 0 else ""


ok(tag("modelName") != "", "modelName = %r" % tag("modelName"))
ok(tag("serialNumber") != "", "serialNumber = %r" % tag("serialNumber"))
ok(tag("manufacturer") == "Canon",
   "manufacturer = %r (manufacturerURL より前に置くこと)" % tag("manufacturer"))
ok(tag("ns:X_deviceNickname") != "", "ns:X_deviceNickname = %r" % tag("ns:X_deviceNickname"))
access = tag("ns:X_accessURL")
ok(access.startswith("http://"), "ns:X_accessURL = %r" % access)

# -------------------------------------------------------------- 3. カタログ
print("[3] CCAPI カタログ (apiCanonCCAPI.cpp:122, 318-367)")
st, body, hdr = get(access)
ok(st == 200, "200 で返る")
ok("Content-Length" in hdr, "Content-Length がある (net.cpp:399-402 で必須)")
cat = json.loads(body)
paths = [e["path"] for v in cat.values() for e in v]
ok(all("ccapi" in p for p in paths), "全 path が 'ccapi' を含む (:341 の splice が壊れない)")


def has(surfix, verb):
    for v in cat.values():
        for e in v:
            if e["path"].endswith(surfix) and e.get(verb):
                return True
    return False


ok(has("control/shutterbutton", "post"), "SHOT がある (init 成功の必須条件 :150)")
for sfx, vb in [("/shooting/settings/av", "get"), ("/shooting/settings/av", "put"),
                ("/shooting/settings/tv", "get"), ("/shooting/settings/tv", "put"),
                ("/shooting/settings/iso", "get"), ("/shooting/settings/iso", "put"),
                ("/shooting/liveview", "post"), ("/shooting/liveview/flipdetail", "get"),
                ("/event/polling", "get"), ("/event/polling", "delete"),
                ("settings/shootingmodedial", "put")]:
    ok(has(sfx, vb), "%s (%s)" % (sfx, vb))

base = access.rsplit("/ccapi", 1)[0]

# --------------------------------------------------- 4. deviceinformation
print("[4] deviceinformation (apiCanonCCAPI.cpp:198-206)")
st, body, _ = get(base + "/ccapi/ver100/deviceinformation")
d = json.loads(body)
ok(d.get("manufacturer") == "Canon", "manufacturer")
ok(bool(d.get("productname")), "productname = %r" % d.get("productname"))
ok(bool(d.get("serialnumber")), "serialnumber = %r" % d.get("serialnumber"))

# --------------------------------------------------------- 5. 撮影開始
print("[5] startShooting / M固定 (apiCanonCCAPI.cpp:462-491, 766-838)")
st, _ = post(base + "/ccapi/ver100/shooting/liveview",
             {"liveviewsize": "small", "cameradisplay": "keep"})
ok(st in (200, 204), "liveview POST が 1発目('keep')で通る -> %d" % st)

st, body, _ = get(base + "/ccapi/ver100/functions/autopoweroff")
apo = json.loads(body).get("value")
ok(apo != "disable", "autopoweroff の初期値が disable 以外 (%r) -> PUT 経路を通る" % apo)
st, _ = put(base + "/ccapi/ver100/functions/autopoweroff", {"value": "disable"})
ok(st in (200, 204), "autopoweroff PUT disable -> %d" % st)

st, body, _ = get(base + "/ccapi/ver100/shooting/settings/shootingmodedial")
mode = json.loads(body).get("value")
ok(mode != "m", "モードダイアルの初期値が m 以外 (%r) -> M固定の PUT を通る" % mode)
st, _ = post(base + "/ccapi/ver100/shooting/control/ignoreshootingmodedialmode", {"action": "on"})
ok(st in (200, 204), "ignoreshootingmodedialmode on -> %d" % st)
st, _ = put(base + "/ccapi/ver100/shooting/settings/shootingmodedial", {"value": "m"})
ok(st in (200, 204), "shootingmodedial m -> %d" % st)

# ------------------------------------------------------- 6. 設定可能値
print("[6] getSettings (apiCanonCCAPI.cpp:657-699, 950-978)")
abil = {}
for k, sfx in [("iso", "iso"), ("tv", "tv"), ("av", "av")]:
    st, body, _ = get(base + "/ccapi/ver100/shooting/settings/" + sfx)
    j = json.loads(body)
    ok("ability" in j and isinstance(j["ability"], list) and len(j["ability"]) > 0,
       "%s: ability が非空の配列" % k)
    ok(all(isinstance(x, str) for x in j["ability"]),
       "%s: ability が全部文字列 (数値だと例外)" % k)
    ok(isinstance(j.get("value"), str), "%s: value が文字列 (%r)" % (k, j.get("value")))
    abil[k] = j["ability"]
ok(all(x.startswith("f") for x in abil["av"]),
   "av の ability が 'f' 始まり (先頭1文字が捨てられる :57)")

# --------------------------------------------------------- 7. 露出の設定
print("[7] 露出 PUT (apiCanonCCAPI.cpp:704-753)")
st, _ = put(base + "/ccapi/ver100/shooting/settings/av", {"value": abil["av"][3]})
ok(st in (200, 204), "av PUT (刻みにある値) -> %d" % st)
st, _ = put(base + "/ccapi/ver100/shooting/settings/tv", {"value": abil["tv"][5]})
ok(st in (200, 204), "tv PUT -> %d" % st)
st, _ = put(base + "/ccapi/ver100/shooting/settings/iso", {"value": abil["iso"][2]})
ok(st in (200, 204), "iso PUT -> %d" % st)
st, _ = put(base + "/ccapi/ver100/shooting/settings/av", {"value": "f99.9"})
ok(st == 400, "刻みに無い値は 400 で断る -> %d (丸めの検証に使う)" % st)

# ------------------------------------------------- 8. シャッターと測光
print("[8] シャッター -> event/polling -> サムネイル (:560-573, 1910-, 1988-)")
st, _ = post(base + "/ccapi/ver100/shooting/control/shutterbutton", {"af": False})
ok(st in (200, 204), "shutterbutton POST -> %d" % st)
st, body, _ = get(base + "/ccapi/ver100/event/polling")
j = json.loads(body)
ok("addedcontents" in j and j["addedcontents"], "addedcontents が返る: %s" % j.get("addedcontents"))
cpath = j["addedcontents"][-1]
ok(cpath.startswith("/"), "パスが '/' 始まり (apiHostBase と連結するため)")
ok(cpath.upper().endswith(".JPG"), "JPG が選ばれる (pickThumbPath :1867-1893)")
st, body, hdr = get(base + cpath + "?kind=thumbnail")
ok(st == 200 and body[:2] == b"\xff\xd8", "サムネイルが JPEG で返る (%d bytes)" % len(body))
st, body, _ = get(base + "/ccapi/ver100/event/polling")
ok(not json.loads(body).get("addedcontents"), "2回目のポーリングは空(回収済み)")

# --------------------------------------------- 9. ライブビュー付帯情報
print("[9] flipdetail?kind=info の独自バイナリ枠 (:519-530, 1332-1411)")
st, body, _ = get(base + "/ccapi/ver100/shooting/liveview/flipdetail?kind=info")
ok(st == 200, "200 で返る")
ok(len(body) >= 10, "10バイト以上 (%d)" % len(body))
ok(body[0] == 0xFF and body[1] == 0x00 and body[2] == 0x01,
   "先頭が FF 00 01 (%02X %02X %02X)" % (body[0], body[1], body[2]))
ln = struct.unpack(">I", body[3:7])[0]
ok(ln <= len(body) - 9, "長さフィールド %d <= size-9 (%d)" % (ln, len(body) - 9))
try:
    lv = json.loads(body[7:len(body) - 2].decode("utf-8"))
    h = lv["liveviewdata"]["histogram"]
    ok(len(h) == 4, "histogram が4配列 (%d)" % len(h))
    ok(len(set(len(x) for x in h)) == 1, "各配列が同じ長さ")
    ok(len(h[0]) % 256 == 0, "長さが256の倍数 (%d)" % len(h[0]))
    ok("sec" in lv["liveviewdata"]["systemtime"], "systemtime.sec がある")
except Exception as e:
    ok(False, "JSON 部の解析: %s" % e)

# ------------------------------------------------------------- 10. Range
print("[10] EXIF 読み Range: bytes=0-65535 (:634-653)")
st, body, hdr = get(base + cpath + "?kind=main", {"Range": "bytes=0-65535"})
ok(st in (200, 206), "206/200 で返る -> %d" % st)

# ------------------------------------------------------------- 11. keepAlive
print("[11] keepAlive (:894-905)")
st, body, _ = get(base + "/ccapi/ver100/event/polling")
ok(st == 200, "event/polling GET が 200")

print()
if NG:
    print("=== NG %d 件 ===" % len(NG))
    for m in NG:
        print("  - " + m)
    sys.exit(1)
print("=== すべて PASS ===")
