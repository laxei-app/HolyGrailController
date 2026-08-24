#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""偽 CCAPI カメラ(検証用)。

目的
----
エッジ端末(M5Stack)に「カメラが N 台居る」と思わせて、台数を増やしたときの
内部RAM/ソケットの消費を**実機のカメラ無しで**測るための道具。

なぜ作れるか
------------
・エッジは SSDP(M-SEARCH, ST: ssdp:all)の応答から LOCATION を拾い、
  UPnP ディスクリプタ XML の <ns:X_accessURL> を **そのまま** CCAPI の土台にする
  (apiCanonCCAPI.cpp:242)。つまり host:port はこちらが自由に決められる。
・よって IP を増やせなくても、**1つの IP でポートを変えれば別カメラとして扱わせられる**。
  (このPCでは管理者権限が無く IP エイリアスを足せないため、この方式を採る)

台ごとに割り当てるポート
------------------------
  カメラ n: ディスクリプタ = DESC_PORT_BASE + n, CCAPI = CCAPI_PORT_BASE + n
  ※ ディスクリプタを 49152 に**置かない**のは意図的。エッジの IP 直結経路
    (detectCanonCCapi::identifyAt)は :49152 決め打ちで探すので、そこに置くと
    全台が同じ1台に見えてしまう。空けておけば必ず SSDP 経路を通る。

忠実でない点(測定結果を読むときの注意)
--------------------------------------
・全台が同じ IP なので、エッジが serial→IP を覚える経路(noteConnected)では
  全台が同じ IP を指す。次回起動の「前回IP直結」は当てにならない。
・露光を待たない(シャッターPOSTは即200)。実機の busy は再現しない。
  RAM/ソケットの測定が目的なので、そこは割り切る。

使い方
------
  python fake_ccapi.py --count 8 --host 192.168.1.4
  停止は Ctrl-C。統計は終了時に出る。
"""

import argparse
import io
import json
import logging
import math
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

# --------------------------------------------------------------------------
# 定数
# --------------------------------------------------------------------------
SSDP_ADDR = "239.255.255.250"
SSDP_PORT = 1900
DESC_PORT_BASE = 49200      # 49152 は使わない(上のコメント参照)
CCAPI_PORT_BASE = 8100

LOG = logging.getLogger("fake")

# SSDP 応答の間隔[秒]と、1台あたり何通返すか。エッジの受信取りこぼしを調べるため可変にする。
SSDP_GAP = 0.035
SSDP_BOTH_ST = True

# 露出の設定可能値。機種混在を再現するため2種類用意する。
# CCAPI の生表記: 秒は 8" / 0"5、F は f5.6、ISO はそのまま。
ISO_FULL = ["100", "125", "160", "200", "250", "320", "400", "500", "640", "800",
            "1000", "1250", "1600", "2000", "2500", "3200", "4000", "5000",
            "6400", "8000", "10000", "12800", "16000", "20000", "25600"]
ISO_COARSE = ["100", "200", "400", "800", "1600", "3200", "6400", "12800", "25600"]

SS_FAST = ["1/4000", "1/3200", "1/2500", "1/2000", "1/1600", "1/1250", "1/1000",
           "1/800", "1/640", "1/500", "1/400", "1/320", "1/250", "1/200",
           "1/160", "1/125", "1/100", "1/80", "1/60", "1/50", "1/40", "1/30",
           "1/25", "1/20", "1/15", "1/13", "1/10", "1/8", "1/6", "1/5", "1/4"]
SS_SLOW_THIRD = ['0"3', '0"4', '0"5', '0"6', '0"8', '1"', '1"3', '1"6', '2"',
                 '2"5', '3"2', '4"', '5"', '6"', '8"', '10"', '13"', '15"',
                 '20"', '25"', '30"']
SS_SLOW_HALF = ['0"3', '0"5', '0"7', '1"', '1"5', '2"', '3"', '4"', '6"',
                '8"', '11"', '15"', '22"', '30"']

FN_THIRD = ["f1.4", "f1.6", "f1.8", "f2.0", "f2.2", "f2.5", "f2.8", "f3.2",
            "f3.5", "f4.0", "f4.5", "f5.0", "f5.6", "f6.3", "f7.1", "f8.0",
            "f9.0", "f10", "f11", "f13", "f14", "f16", "f18", "f20", "f22"]
FN_HALF = ["f1.4", "f1.7", "f2.0", "f2.4", "f2.8", "f3.3", "f4.0", "f4.8",
           "f5.6", "f6.7", "f8.0", "f9.5", "f11", "f13", "f16", "f19", "f22"]

def ss_to_ccapi(v):
    """内部表記の ss を CCAPI の生表記へ。apiCanonCCAPI.cpp:22-31 の ssToCcapi と同じ規則。

      "1/4000" -> そのまま / "0.5" -> 0"5 / "8" -> 8"
    ability はカメラが広告する生の文字列なので、この形で返さないと
    PUT で送り返された値と突き合わせられない。
    """
    if not v or "/" in v:
        return v
    if v.lower() == "bulb":
        return "bulb"
    if "." in v:
        return v.replace(".", '\"', 1)
    return v + '\"'


def fn_list_for(kind):
    """F値の ability。機材マスタは F を持たないので刻みだけ用意する。"""
    return FN_HALF if kind else FN_THIRD


# 機種のひな型。(モデル名, iso, ss, fn)
PROFILES = [
    ("Canon EOS R10",   ISO_FULL,   SS_FAST + SS_SLOW_THIRD, FN_THIRD),
    ("Canon EOS R100",  ISO_FULL,   SS_FAST + SS_SLOW_THIRD, FN_THIRD),
    ("Canon EOS R50 V", ISO_COARSE, SS_FAST + SS_SLOW_HALF,  FN_HALF),
]


# --------------------------------------------------------------------------
# 1台ぶんの状態
# --------------------------------------------------------------------------
class FakeCamera:
    """1台ぶんの状態と応答。機種ごとに設定可能値(刻み)を変えられる。"""

    def __init__(self, index, host, model, nickname, serial, iso_list, ss_list, fn_list):
        self.index = index
        self.host = host
        self.model = model                  # 例 "Canon EOS R10"
        self.nickname = nickname            # ns:X_deviceNickname (愛称)
        self.serial = serial
        self.uuid = "00000000-0000-0000-0001-%012X" % (0xF00000000000 + index)
        self.desc_port = DESC_PORT_BASE + index
        self.ccapi_port = CCAPI_PORT_BASE + index

        self.iso_list = iso_list
        self.ss_list = ss_list
        self.fn_list = fn_list

        # 現在値(CCAPI 生表記)
        self.av = fn_list[len(fn_list) // 2]
        self.tv = "1/125"
        self.iso = "400"
        self.dial_ignore = False
        self.mode = "av"          # M固定の経路を実際に通すため既定は M 以外
        self.autopoweroff = "30"  # 同上。撮影開始で "disable" に変えに来る
        self.lv_seq = 0           # ライブビュー情報の systemtime(毎回進める)

        self.shots = 0
        self.added = []                     # 未回収の addedcontents
        self.last_image = None
        self.lock = threading.Lock()
        self.req_count = 0
        self.put_count = 0
        self.shot_count = 0
        # パス別の回数。エッジが何を要求して止まっているかを見るため。
        self.paths = {}

    # ---- URL -------------------------------------------------------------
    @property
    def location(self):
        return "http://%s:%d/upnp/CameraDevDesc.xml" % (self.host, self.desc_port)

    @property
    def access_url(self):
        return "http://%s:%d/ccapi" % (self.host, self.ccapi_port)

    # ---- SSDP ------------------------------------------------------------
    def ssdp_responses(self):
        """deviceDiscovery::analizeUsn は USN に uuid/urn/(device|service) の
        3つが揃っていることを要求する(deviceDiscovery.cpp:86-)。
        ifaces のキーワードは "ICPO-CameraControlAPIService" と
        "schemas-canon-com" の**両方**が本文に要る(detectCanonCCapi.cpp:13)。
        """
        sts = ["urn:schemas-canon-com:service:ICPO-CameraControlAPIService:1"]
        if SSDP_BOTH_ST:
            sts.insert(0, "urn:schemas-canon-com:device:ICPO-CameraControlAPIService:1")
        out = []
        for st in sts:
            out.append(
                "HTTP/1.1 200 OK\r\n"
                "Cache-Control: max-age=1800\r\n"
                "EXT:\r\n"
                "Location: %s\r\n"
                "Server: Camera OS/1.0 UPnP/1.0 Canon Device Discovery/1.0\r\n"
                "ST: %s\r\n"
                "USN: uuid:%s::%s\r\n"
                "\r\n" % (self.location, st, self.uuid, st))
        return [s.encode("ascii") for s in out]

    # ---- UPnP ディスクリプタ ---------------------------------------------
    def descriptor_xml(self):
        """コードが読むのは modelName / serialNumber / manufacturer /
        ns:X_deviceNickname / ns:X_accessURL / URLBase(apiCanonCCAPI.cpp:230-245)。"""
        return (
            '<?xml version="1.0"?>\n'
            '<root xmlns="urn:schemas-upnp-org:device-1-0">\n'
            '  <specVersion><major>1</major><minor>0</minor></specVersion>\n'
            '  <URLBase>http://%(host)s:%(dport)d/upnp/</URLBase>\n'
            '  <device>\n'
            '    <deviceType>urn:schemas-canon-com:device:ICPO-CameraControlAPIService:1</deviceType>\n'
            '    <friendlyName>%(model)s</friendlyName>\n'
            '    <manufacturer>Canon</manufacturer>\n'
            '    <manufacturerURL>http://www.canon.com/</manufacturerURL>\n'
            '    <modelDescription>Canon Digital Camera</modelDescription>\n'
            '    <modelName>%(model)s</modelName>\n'
            '    <serialNumber>%(serial)s</serialNumber>\n'
            '    <UDN>uuid:%(uuid)s</UDN>\n'
            '    <serviceList>\n'
            '      <service>\n'
            '        <serviceType>urn:schemas-canon-com:service:ICPO-CameraControlAPIService:1</serviceType>\n'
            '        <serviceId>urn:schemas-canon-com:serviceId:ICPO-CameraControlAPIService-1</serviceId>\n'
            '        <SCPDURL>CameraSvcDesc.xml</SCPDURL>\n'
            '        <controlURL>control/CanonCamera/</controlURL>\n'
            '        <eventSubURL/>\n'
            '        <ns:X_targetId xmlns:ns="urn:schemas-canon-com:schema-upnp">uuid:%(uuid)s</ns:X_targetId>\n'
            '        <ns:X_onService xmlns:ns="urn:schemas-canon-com:schema-upnp">0</ns:X_onService>\n'
            '        <ns:X_accessURL xmlns:ns="urn:schemas-canon-com:schema-upnp">%(access)s</ns:X_accessURL>\n'
            '        <ns:X_deviceUsbId xmlns:ns="urn:schemas-canon-com:schema-upnp">32f8</ns:X_deviceUsbId>\n'
            '        <ns:X_deviceNickname xmlns:ns="urn:schemas-canon-com:schema-upnp">%(nick)s</ns:X_deviceNickname>\n'
            '      </service>\n'
            '    </serviceList>\n'
            '    <presentationURL>/</presentationURL>\n'
            '  </device>\n'
            '</root>\n'
            % {"host": self.host, "dport": self.desc_port, "model": self.model,
               "serial": self.serial, "uuid": self.uuid,
               "access": self.access_url, "nick": self.nickname})


# --------------------------------------------------------------------------
# CCAPI カタログ(実機 EOS R10 の /ccapi をそのまま)
# --------------------------------------------------------------------------
def build_catalog():
    def e(path, get=False, post=False, put=False, delete=False):
        return {"path": path, "get": get, "post": post, "put": put, "delete": delete}

    return {
        "ver100": [
            e("/ccapi/ver100/deviceinformation", get=True),
            e("/ccapi/ver100/devicestatus/battery", get=True),
            e("/ccapi/ver100/devicestatus/lens", get=True),
            e("/ccapi/ver100/devicestatus/temperature", get=True),
            e("/ccapi/ver100/functions/datetime", get=True, put=True),
            e("/ccapi/ver100/functions/autopoweroff", get=True, put=True),
            e("/ccapi/ver100/shooting/control/shutterbutton", post=True),
            e("/ccapi/ver100/shooting/control/shutterbutton/manual", post=True),
            e("/ccapi/ver100/shooting/control/ignoreshootingmodedialmode", get=True, post=True),
            e("/ccapi/ver100/shooting/control/af", post=True),
            e("/ccapi/ver100/shooting/settings", get=True),
            e("/ccapi/ver100/shooting/settings/shootingmodedial", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/av", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/tv", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/iso", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/exposure", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/metering", get=True, put=True),
            e("/ccapi/ver100/shooting/settings/drive", get=True, put=True),
            e("/ccapi/ver100/shooting/information/recordable", get=True),
            e("/ccapi/ver100/shooting/liveview", post=True),
            e("/ccapi/ver100/shooting/liveview/flip", get=True),
            e("/ccapi/ver100/shooting/liveview/flipdetail", get=True),
            e("/ccapi/ver100/shooting/liveview/scroll", get=True, delete=True),
            e("/ccapi/ver100/shooting/liveview/scrolldetail", get=True, delete=True),
            e("/ccapi/ver100/event/polling", get=True, delete=True),
            e("/ccapi/ver100/event/monitoring", get=True, delete=True),
        ],
        "ver110": [
            e("/ccapi/ver110/devicestatus/storage", get=True),
            e("/ccapi/ver110/devicestatus/currentstorage", get=True),
            e("/ccapi/ver110/devicestatus/currentdirectory", get=True),
            e("/ccapi/ver110/devicestatus/batterylist", get=True),
            e("/ccapi/ver110/shooting/settings", get=True),
            e("/ccapi/ver110/shooting/settings/stillimagequality", get=True, put=True),
            e("/ccapi/ver110/shooting/settings/shuttermode", get=True, put=True),
        ],
        "ver130": [
            e("/ccapi/ver130/contents", get=True, put=True, delete=True),
        ],
    }


CATALOG = build_catalog()


# --------------------------------------------------------------------------
# サムネイル生成(測光用)
# --------------------------------------------------------------------------
_THUMB_CACHE = {}


def make_thumbnail(level):
    """level: 0.0(黒)〜1.0(白) の一様な JPEG。中央値が 0 だと測光が失敗扱いに
    なる(thumbMeterCore の out.ok 判定)ので、呼ぶ側で 0 を渡さないこと。"""
    v = max(1, min(255, int(round(level * 255))))
    if v in _THUMB_CACHE:
        return _THUMB_CACHE[v]
    img = Image.new("RGB", (160, 120), (v, v, v))
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=85)
    data = buf.getvalue()
    _THUMB_CACHE[v] = data
    return data


# --------------------------------------------------------------------------
# ライブビュー付帯情報(flipdetail?kind=info)の独自バイナリフレーム
# --------------------------------------------------------------------------
def make_lv_info(level, seq):
    """checkLiveViewInfo / alzMetering が要求する形(apiCanonCCAPI.cpp:519-530, 1332-1411)。

      [0]=0xFF [1]=0x00 [2]=0x01
      [3..6]  = JSON のバイト数(ビッグエンディアン32bit)
      [7..]   = JSON 本体
      末尾2バイト = 余白(sax_parse が end()-2 まで読むため)

    JSON は histogram を **4配列 × 256の整数倍**で持ち、systemtime を毎回進める
    (進みが足りないと「古いフレーム」として捨てられ再取得される)。
    """
    v = max(1, min(255, int(round(level * 255))))
    hist = [0] * 256
    # 実機のように山を作る(中央値が v になるよう v を中心に置く)
    for i in range(max(0, v - 8), min(256, v + 9)):
        hist[i] = 4000
    one = json.dumps(hist, separators=(",", ":"))
    body = ('{"liveviewdata":{"histogram":[%s,%s,%s,%s],'
            '"systemtime":{"sec":%d,"subsec":%d}}}'
            % (one, one, one, one, seq // 1000, seq % 1000))
    raw = body.encode("utf-8")
    # 0xFF 0x00 0x01 + JSON長(ビッグエンディアン32bit) + JSON + 末尾2バイト
    head = bytes([0xFF, 0x00, 0x01])
    return head + struct.pack(">I", len(raw)) + raw + bytes([0x00, 0x00])


# --------------------------------------------------------------------------
# HTTP ハンドラ
# --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"       # keep-alive。実機同様に接続を維持させる
    cam = None                          # サーバ生成時に差し込む

    # ---- 送信補助 --------------------------------------------------------
    def _send(self, code, body=b"", ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj), "application/json")

    def _err(self, code, msg):
        self._json({"message": msg}, code)

    def log_message(self, fmt, *a):
        LOG.debug("[%s] %s", self.cam.nickname, fmt % a)

    def _note(self, method):
        """パス別に回数を数える(クエリは kind= だけ残す)。"""
        path = self.path.split("?", 1)[0]
        q = self.path.split("?", 1)[1] if "?" in self.path else ""
        if "kind=" in q:
            path += "?" + [x for x in q.split("&") if x.startswith("kind=")][0]
        key = method + " " + path
        with self.cam.lock:
            self.cam.paths[key] = self.cam.paths.get(key, 0) + 1

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n <= 0:
            return {}
        raw = self.rfile.read(n)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    # ---- ルーティング ----------------------------------------------------
    def do_GET(self):
        self._note("GET")
        c = self.cam
        with c.lock:
            c.req_count += 1
        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        # UPnP ディスクリプタ(認証不要)
        if path == "/upnp/CameraDevDesc.xml":
            return self._send(200, c.descriptor_xml(), "text/xml; charset=utf-8")

        # CCAPI カタログ
        if path == "/ccapi":
            return self._json(CATALOG)

        if path == "/ccapi/ver100/deviceinformation":
            return self._json({
                "manufacturer": "Canon",
                "productname": c.model,
                "guid": c.uuid.replace("-", ""),
                "serialnumber": c.serial,
                "macaddress": "00:00:00:00:%02X:%02X" % (c.index, c.index),
                "firmwareversion": "1.0.0",
            })

        if path == "/ccapi/ver100/shooting/settings/av":
            return self._json({"value": c.av, "ability": c.fn_list})
        if path == "/ccapi/ver100/shooting/settings/tv":
            return self._json({"value": c.tv, "ability": c.ss_list})
        if path == "/ccapi/ver100/shooting/settings/iso":
            return self._json({"value": c.iso, "ability": c.iso_list})
        if path == "/ccapi/ver100/shooting/settings/shootingmodedial":
            return self._json({"value": c.mode,
                               "ability": ["m", "av", "tv", "p", "auto"]})
        if path == "/ccapi/ver100/shooting/control/ignoreshootingmodedialmode":
            return self._json({"status": "on" if c.dial_ignore else "off"})

        if path == "/ccapi/ver100/devicestatus/battery":
            return self._json({"name": "LP-E17", "kind": "battery",
                               "level": "100", "quality": "good"})
        if path == "/ccapi/ver100/devicestatus/temperature":
            return self._json({"status": "normal"})
        if path == "/ccapi/ver100/devicestatus/lens":
            return self._json({"mount": True, "name": "RF16mm F2.8 STM"})
        if path == "/ccapi/ver110/devicestatus/storage":
            return self._json({"storagelist": [{
                "name": "sd", "url": "/ccapi/ver130/contents/sd",
                "accesscapability": "readwrite", "maxsize": 32000000000,
                "spacesize": 30000000000, "contentsnumber": c.shots}]})
        if path == "/ccapi/ver110/devicestatus/currentstorage":
            return self._json({"path": "/ccapi/ver130/contents/sd"})
        if path == "/ccapi/ver110/devicestatus/currentdirectory":
            return self._json({"path": "/ccapi/ver130/contents/sd/100CANON"})
        if path == "/ccapi/ver100/shooting/information/recordable":
            return self._json({"stillimage": 9999, "movie": 999})
        if path == "/ccapi/ver100/functions/autopoweroff":
            return self._json({"value": c.autopoweroff,
                               "ability": ["disable", "30", "1min", "2min", "4min"]})

        # 撮影画像の登録通知。溜まっていれば返して空にする。
        if path == "/ccapi/ver100/event/polling":
            with c.lock:
                added = c.added
                c.added = []
            return self._json({"addedcontents": added} if added else {})

        # サムネイル取得。露出から明るさを作って返す。
        if path.startswith("/ccapi/ver130/contents/"):
            jpg = make_thumbnail(self._scene_level())
            # EXIF 読み(readSensorSpec)は Range: bytes=0-65535 で先頭だけ取りに来る。
            # 実機は 206 を返す。ここは中身を作り込まないので同じ絵を返すだけ。
            if self.headers.get("Range"):
                self.send_response(206)
                self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Range", "bytes 0-%d/%d" % (len(jpg) - 1, len(jpg)))
                self.send_header("Content-Length", str(len(jpg)))
                self.end_headers()
                return self.wfile.write(jpg)
            return self._send(200, jpg, "image/jpeg")

        # ライブビュー付帯情報。?kind=info は **JPEGではなく独自バイナリ枠**
        # (apiCanonCCAPI.cpp:519-530 checkLiveViewInfo)。初期収束の測光がここを見る。
        if path == "/ccapi/ver100/shooting/liveview/flipdetail":
            with c.lock:
                c.lv_seq += 137          # 毎回進める(進まないと「古いフレーム」で捨てられる)
                seq = int(time.time() * 1000) + c.lv_seq
            return self._send(200, make_lv_info(self._scene_level(), seq),
                              "application/octet-stream")
        if path in ("/ccapi/ver100/shooting/liveview/flip",
                    "/ccapi/ver100/shooting/liveview/scroll",
                    "/ccapi/ver100/shooting/liveview/scrolldetail"):
            return self._send(200, make_thumbnail(self._scene_level()), "image/jpeg")

        return self._err(404, "Not Found")

    def do_PUT(self):
        self._note("PUT")
        c = self.cam
        with c.lock:
            c.req_count += 1
            c.put_count += 1
        body = self._read_body()
        val = body.get("value")
        path = self.path.split("?", 1)[0]

        def accept(field, allowed):
            if val not in allowed:
                # 実機と同じく「刻みに無い値」は 400 で断る。丸めが効いているかの検証になる。
                self._err(400, "Invalid parameter")
                return False
            setattr(c, field, val)
            self._json({"value": val})
            return True

        if path == "/ccapi/ver100/shooting/settings/av":
            return None if accept("av", c.fn_list) else None
        if path == "/ccapi/ver100/shooting/settings/tv":
            return None if accept("tv", c.ss_list) else None
        if path == "/ccapi/ver100/shooting/settings/iso":
            return None if accept("iso", c.iso_list) else None
        if path == "/ccapi/ver100/shooting/settings/shootingmodedial":
            return None if accept("mode", ["m", "av", "tv", "p", "auto"]) else None
        if path == "/ccapi/ver100/functions/autopoweroff":
            c.autopoweroff = val
            return self._json({"value": val})
        return self._err(404, "Not Found")

    def do_POST(self):
        self._note("POST")
        c = self.cam
        with c.lock:
            c.req_count += 1
        body = self._read_body()
        path = self.path.split("?", 1)[0]

        if path in ("/ccapi/ver100/shooting/control/shutterbutton",
                    "/ccapi/ver100/shooting/control/shutterbutton/manual"):
            with c.lock:
                c.shots += 1
                c.shot_count += 1
                name = "/ccapi/ver130/contents/sd/100CANON/IMG_%04d.JPG" % (c.shots % 10000)
                c.added.append(name)
                c.last_image = name
            return self._json({})

        if path == "/ccapi/ver100/shooting/liveview":
            return self._json({})
        if path == "/ccapi/ver100/shooting/control/ignoreshootingmodedialmode":
            c.dial_ignore = (body.get("action") == "on")
            return self._json({})
        if path == "/ccapi/ver100/shooting/control/af":
            return self._json({})
        return self._err(404, "Not Found")

    def do_DELETE(self):
        self._note("DELETE")
        c = self.cam
        with c.lock:
            c.req_count += 1
        return self._json({})

    # ---- 明るさモデル ----------------------------------------------------
    def _scene_level(self):
        """いま設定されている露出から「写る明るさ」を作る。

        露出が明るいほど白く写る、という当たり前の関係だけ再現する。
        露出制御の追従が動いているかを見るのに要る(暗ければ明るくしに来る)。
        """
        c = self.cam
        try:
            iso = float(c.iso)
        except Exception:
            iso = 400.0
        fn = float(c.av[1:]) if c.av.startswith("f") else 5.6
        tv = c.tv
        if "/" in tv:
            sec = 1.0 / float(tv.split("/")[1])
        else:
            sec = float(tv.replace('"', ".").rstrip(".") or "1")
        # 露出量(段) = log2(ISO) + log2(t) - 2*log2(N)。適当な基準で 0..1 へ。
        import math
        ev = math.log2(max(iso, 1) / 100.0) + math.log2(max(sec, 1e-6)) - 2 * math.log2(max(fn, 0.5))
        # ev = -6 で真っ暗、+2 で真っ白あたりにする
        return max(0.02, min(0.98, (ev + 8.0) / 10.0))


# --------------------------------------------------------------------------
# SSDP 応答スレッド
# --------------------------------------------------------------------------
class SsdpResponder(threading.Thread):
    daemon = True

    def __init__(self, cams, host):
        super().__init__(name="ssdp")
        self.cams = cams
        self.host = host
        self.stop_flag = threading.Event()
        self.answered = 0

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", SSDP_PORT))
        mreq = struct.pack("4s4s", socket.inet_aton(SSDP_ADDR), socket.inet_aton(self.host))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        s.settimeout(1.0)
        LOG.info("SSDP 待受開始 (0.0.0.0:%d, if=%s)", SSDP_PORT, self.host)
        while not self.stop_flag.is_set():
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            text = data.decode("ascii", "ignore")
            if not text.startswith("M-SEARCH"):
                continue
            LOG.info("M-SEARCH from %s:%d -> %d台ぶん応答", addr[0], addr[1], len(self.cams))
            # 【間隔を空ける理由(2026-08-25 実測)】全台ぶんを一気に投げると、エッジの
            #  SSDP受信バッファが溢れて 8台中2台しか届かなかった。実機のカメラは M-SEARCH の
            #  MX(=1秒)に従って各自ばらばらに返すので、こちらも散らす。
            threading.Thread(target=self._reply, args=(s, addr), daemon=True).start()
            self.answered += 1
        s.close()

    def _reply(self, sock, addr):
        """1台ずつ間を空けて応答する(受信側のバッファ溢れを避ける)。"""
        for cam in self.cams:
            for pkt in cam.ssdp_responses():
                try:
                    sock.sendto(pkt, addr)
                except OSError as e:
                    LOG.warning("SSDP 応答送信 NG: %s", e)
                time.sleep(SSDP_GAP)


# --------------------------------------------------------------------------
# 起動
# --------------------------------------------------------------------------
def make_server(cam, port):
    handler = type("H_%d" % cam.index, (Handler,), {"cam": cam})
    srv = ThreadingHTTPServer(("0.0.0.0", port), handler)
    srv.daemon_threads = True
    return srv


def main():
    ap = argparse.ArgumentParser(description="偽 CCAPI カメラ(検証用)")
    ap.add_argument("--count", type=int, default=8, help="立てる台数")
    ap.add_argument("--host", default="192.168.1.4", help="このPCのLAN側IP")
    ap.add_argument("--spec", help="make_spec.py が書いた spec.json")
    ap.add_argument("--ssdp-gap", type=float, default=0.035, help="SSDP応答の間隔[秒]")
    ap.add_argument("--ssdp-one-st", action="store_true", help="1台1通だけ返す")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    global SSDP_GAP, SSDP_BOTH_ST
    SSDP_GAP = args.ssdp_gap
    SSDP_BOTH_ST = not args.ssdp_one_st

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(message)s", datefmt="%H:%M:%S")

    cams = []
    if args.spec:
        # make_spec.py が書いた仕様書どおりに装う。スマホの所持カメラの控えと
        # 機種名/シリアル/愛称が一致するので、撮影計画がそのまま当たる。
        spec = json.load(io.open(args.spec, encoding="utf-8"))
        for i, e in enumerate(spec[:args.count], start=1):
            cam = FakeCamera(
                index=i, host=args.host, model=e["model"],
                nickname=e.get("nickname") or ("FAKE%02d" % i),
                serial=e.get("serial") or ("9900000000%02d" % i),
                iso_list=e.get("isoList") or ISO_FULL,
                ss_list=[ss_to_ccapi(v) for v in (e.get("ssList") or [])]
                        or (SS_FAST + SS_SLOW_THIRD),
                fn_list=fn_list_for(i % 2))
            cams.append(cam)
    else:
        for i in range(1, args.count + 1):
            model, iso, ss, fn = PROFILES[(i - 1) % len(PROFILES)]
            cam = FakeCamera(
                index=i, host=args.host, model=model,
                nickname="FAKE%02d" % i,
                serial="9900000000%02d" % i,
                iso_list=iso, ss_list=ss, fn_list=fn)
            cams.append(cam)

    servers = []
    for cam in cams:
        servers.append(make_server(cam, cam.desc_port))     # ディスクリプタ用
        servers.append(make_server(cam, cam.ccapi_port))    # CCAPI 用
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()

    for cam in cams:
        LOG.info("cam%02d %-16s nick=%s serial=%s desc=:%d ccapi=:%d",
                 cam.index, cam.model, cam.nickname, cam.serial,
                 cam.desc_port, cam.ccapi_port)

    ssdp = SsdpResponder(cams, args.host)
    ssdp.start()

    LOG.info("準備完了。%d台。Ctrl-C で終了。", len(cams))
    try:
        while True:
            time.sleep(10)
            busy = [c for c in cams if c.req_count]
            if busy:
                LOG.info("状況: " + " | ".join(
                    "%s req=%d put=%d shot=%d" % (c.nickname, c.req_count, c.put_count, c.shot_count)
                    for c in busy))
                # 直近で何を要求されているかを出す(エッジがどこで止まっているかの手掛かり)
                for c in busy[:2]:
                    with c.lock:
                        top = sorted(c.paths.items(), key=lambda x: -x[1])[:12]
                    if top:
                        LOG.info("   %s: %s" % (c.nickname,
                                 ", ".join("%s x%d" % (k, v) for k, v in top)))
    except KeyboardInterrupt:
        pass
    finally:
        LOG.info("=== 集計 ===")
        LOG.info("M-SEARCH 応答回数: %d", ssdp.answered)
        for c in cams:
            LOG.info("%s: req=%d put=%d shot=%d", c.nickname, c.req_count, c.put_count, c.shot_count)
            for k, v in sorted(c.paths.items(), key=lambda x: -x[1]):
                LOG.info("     %5d  %s", v, k)
        ssdp.stop_flag.set()
        for s in servers:
            s.shutdown()


if __name__ == "__main__":
    main()
