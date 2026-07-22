# -*- coding: utf-8 -*-
# 8秒露光の「直後」にライブビュー(測光=flipdetail?kind=info)がいつ復帰するかを実測する。
# アプリは触らない。R100が既に M / SS=8" の状態である前提(本体ダイヤル)。
#   python probe_shot_recovery.py 192.168.1.7   [shots]
import sys, time, json
import probe_liveview as p

IP = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1][0].isdigit() else "192.168.1.7"
SHOTS = int(sys.argv[2]) if len(sys.argv) > 2 else 3
LIVE = "/ccapi/ver100/shooting/liveview"
FLIP = "/ccapi/ver100/shooting/liveview/flipdetail?kind=info"
SHUT = "/ccapi/ver100/shooting/control/shutterbutton"

def flip_once():
    st, hd, raw = p.http_req(IP, "GET", FLIP)
    if st == 200 and raw[:3] == b"\xff\x00\x01":
        return 200, "OK(" + p.hist_summary(raw) + ")"
    if "json" in hd.get("Content-Type", "").lower():
        try:
            return st, json.loads(raw.decode("utf-8", "replace")).get("message", raw[:80])
        except Exception:
            return st, raw[:80]
    return st, f"len={len(raw)} head={raw[:4].hex()}"

# 現在設定を確認
for ep in ("shootingmodedial", "tv", "iso"):
    st, hd, raw = p.http_req(IP, "GET", "/ccapi/ver100/shooting/settings/" + ep)
    print(f"  {ep}= {json.loads(raw.decode()).get('value')}")

# ライブビュー開始
for disp in ("on", "off", "keep"):
    st, hd, raw = p.http_req(IP, "POST", LIVE, {"liveviewsize": "small", "cameradisplay": disp})
    if 200 <= st <= 204:
        print(f"liveview開始 OK (cameradisplay={disp})"); break

print("\n-- ベースライン(撮影前) flipdetail 3回 --")
for _ in range(3):
    st, msg = flip_once(); print(f"   {st}  {msg}"); time.sleep(0.5)

for s in range(SHOTS):
    print(f"\n===== SHOT {s+1}/{SHOTS} : 8秒露光 → 直後から測光ポーリング =====")
    t0 = time.monotonic()
    st, hd, raw = p.http_req(IP, "POST", SHUT, {"af": False}, timeout=20)
    tpost = time.monotonic() - t0
    print(f"   shutterbutton POST -> status={st} body={raw[:60]!r}  (POST所要={tpost:.1f}s)")
    # POST完了(=露光完了とみなす)時刻を基準に、測光がいつ戻るかを測る
    tbase = time.monotonic()
    first_ok = None
    while True:
        el = time.monotonic() - tbase
        st2, msg = flip_once()
        tag = "OK " if st2 == 200 and msg.startswith("OK") else "NG "
        print(f"   +{el:4.1f}s  {tag}{st2}  {msg}")
        if st2 == 200 and msg.startswith("OK") and first_ok is None:
            first_ok = el
        if el > 22.0:
            break
        time.sleep(0.4)
    print(f"   => 露光後、測光が最初に成功した時刻: {('+%.1fs' % first_ok) if first_ok is not None else '8秒以内に復帰せず'}")
    time.sleep(1.0)
