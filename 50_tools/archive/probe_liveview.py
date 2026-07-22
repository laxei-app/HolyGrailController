# -*- coding: utf-8 -*-
# CCAPI ライブビュー(flipdetail?kind=info)の生応答を確認する調査スクリプト。
# アプリ/エッジは一切変更しない。Windowsから直接カメラのCCAPIを叩くだけ。
#
# 使い方:
#   python probe_liveview.py            … 192.168.1.0/24 を走査してCanon機を列挙し R100 を自動選択
#   python probe_liveview.py 192.168.1.7 … IP直指定
#   python probe_liveview.py 192.168.1.7 R10  … モデル部分一致で選ぶ(既定 R100)
#
# 前提: 対象カメラを他(スマホ/エッジ)が掴んでいないこと(ライブビューの取り合いを避ける)。
#       レンズキャップを付けた状態で実行し、flipdetail が 200バイナリ(ヒストグラム)か
#       503 JSON({"message":...}) のどちらを返すかを見る。

import sys, socket, json, time, http.client
from concurrent.futures import ThreadPoolExecutor

PORT = 8080
SUBNET = "192.168.1."          # 環境に合わせる(このPCは .3)
WANT_MODEL = "R100"

def http_req(ip, method, path, body=None, timeout=4):
    """生の (status, headers(dict), body(bytes)) を返す。5xx でも例外にしない。"""
    conn = http.client.HTTPConnection(ip, PORT, timeout=timeout)
    try:
        headers = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        conn.request(method, path, body=data, headers=headers)
        r = conn.getresponse()
        raw = r.read()
        return r.status, dict(r.getheaders()), raw
    finally:
        conn.close()

def get_deviceinfo(ip):
    try:
        st, hd, raw = http_req(ip, "GET", "/ccapi/ver100/deviceinformation", timeout=1.5)
        if st == 200:
            j = json.loads(raw.decode("utf-8", "replace"))
            return j.get("productname", "?"), j.get("serialnumber", "?")
    except Exception:
        pass
    return None

def scan_subnet():
    found = []
    def probe(n):
        ip = SUBNET + str(n)
        di = get_deviceinfo(ip)
        if di:
            found.append((ip, di[0], di[1]))
    with ThreadPoolExecutor(max_workers=64) as ex:
        ex.map(probe, range(1, 255))
    return sorted(found)

def find_ccapi_paths(ip):
    """/ccapi から shooting/liveview と shooting/liveview/flipdetail の URL(最新版)を得る。"""
    st, hd, raw = http_req(ip, "GET", "/ccapi")
    api = json.loads(raw.decode("utf-8", "replace"))
    live = flip = None
    # api = {"ver100":[{path,get,post,...}], "ver110":[...], ...}
    for ver in sorted(api.keys()):
        for e in api[ver]:
            p = e.get("path", "")
            if p.endswith("/shooting/liveview"):          live = p
            if p.endswith("/shooting/liveview/flipdetail"): flip = p
    return live, flip

def hist_summary(raw):
    """200 の info チャンク(0xFF0001|size|JSON|0xFFFF)からヒストYのY分布を要約。"""
    try:
        jb = raw[7:-2]                       # alzMetering と同じ: 先頭7B・末尾2Bを外す
        j = json.loads(jb.decode("utf-8", "replace"))
        y = j["liveviewdata"]["histogram"][0]  # Yチャンネル(256 or 256の倍数)
        tot = sum(y) or 1
        n = len(y)
        dark = sum(y[:max(1, n // 32)])      # 最暗 約3% のビンに入る画素割合
        # 重心(0..255相当)
        binw = n / 256.0
        centroid = sum(i * v for i, v in enumerate(y)) / tot / binw
        return f"histY: bins={n} 総画素={tot} 最暗側={100.0*dark/tot:4.1f}% 重心={centroid:5.1f}/255"
    except Exception as e:
        return f"histParse失敗: {e}"

def dump(tag, st, hd, raw):
    ct = hd.get("Content-Type", "?")
    head = raw[:16]
    hexs = " ".join(f"{b:02x}" for b in head)
    print(f"[{tag}] status={st}  Content-Type={ct}  len={len(raw)}")
    print(f"        first16(hex)= {hexs}")
    if "json" in ct.lower() or raw[:1] in (b"{", b"["):
        try:
            print(f"        JSON= {json.loads(raw.decode('utf-8','replace'))}   ← アプリはこれをNOT_LIVE_DETAILで弾く")
        except Exception:
            print(f"        text= {raw[:120]!r}")
    elif len(head) >= 3 and head[0] == 0xFF and head[1] == 0x00:
        typ = {0: "image", 1: "info", 2: "event"}.get(head[2], f"?{head[2]}")
        print(f"        -> 正規バイナリ chunk type=0x{head[2]:02x}({typ})  ★取得OK  {hist_summary(raw)}")
    else:
        print(f"        -> 先頭が 0xFF00 でない(アプリはここで弾く)")

def main():
    ip = None
    want = WANT_MODEL
    args = [a for a in sys.argv[1:]]
    if args and args[0][0].isdigit():
        ip = args[0]; args = args[1:]
    if args:
        want = args[0]

    if ip is None:
        print(f"== {SUBNET}0/24 を走査してCanon機を探索中… ==")
        cams = scan_subnet()
        if not cams:
            print("Canon機が見つからない。カメラの電源/Wi-Fi/同一LANを確認。IP直指定でも可。")
            return
        for cip, model, serial in cams:
            print(f"  {cip}  {model}  serial={serial}")
        pick = [c for c in cams if want.lower() in c[1].lower()]
        if not pick:
            print(f"\n'{want}' を含む機種が無い。上の一覧からIP直指定で再実行して。")
            return
        ip = pick[0][0]
        print(f"\n== 対象: {ip} ({pick[0][1]}) ==")
    else:
        di = get_deviceinfo(ip)
        print(f"== 対象: {ip} ({di[0] if di else '?'}) ==")

    live, flip = find_ccapi_paths(ip)
    print(f"liveview  = {live}")
    print(f"flipdetail= {flip}")
    if not flip:
        print("flipdetail エンドポイントが無い。"); return

    # 1) ライブビュー開始 (R100 は cameradisplay:'keep' を 400 で拒否 → on/off も試す)
    print("\n-- ライブビュー開始 (POST liveview) --")
    started = False
    for disp in ("keep", "on", "off"):
        try:
            st, hd, raw = http_req(ip, "POST", live, {"liveviewsize": "small", "cameradisplay": disp})
            print(f"  cameradisplay={disp:4s} -> status={st}  body={raw[:120]!r}")
            if 200 <= st <= 204:
                started = True; break
        except Exception as e:
            print(f"  cameradisplay={disp:4s} -> 例外 {e}")
    print(f"  => ライブビュー開始 {'成功' if started else '失敗(それでもGETは試す)'}")

    # 2) flipdetail?kind=info を連続取得して生応答を見る
    print("\n-- flipdetail?kind=info を10回取得 (各1秒) --")
    for i in range(10):
        try:
            st, hd, raw = http_req(ip, "GET", flip + "?kind=info")
            dump(f"#{i+1}", st, hd, raw)
        except Exception as e:
            print(f"[#{i+1}] 例外 {e}")
        time.sleep(1.0)

if __name__ == "__main__":
    main()
