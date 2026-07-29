# -*- coding: utf-8 -*-
# 「撮影を繰り返すと wait(シャッター→新ファイル登録の通知まで)が伸びるか」を切り分ける実験。
# 2026-07-29: Edje00/R10 が測光開始8分後から wait=1.0秒→3.6秒と伸び、15分で無応答になった。
# 撮影を伴わない polling 連打(実験A)では再現しなかったため、撮影しながら条件を変えて比較する。
#
# 条件:
#   poll   … event/polling(無指定)で addedcontents を待つ ＝ 現行アプリと同じ
#   nopoll … event/polling を一切使わず contents の総数増加で検知する ＝ 代替案
#   abandon… 先に continue=on を3秒で放棄してから poll する ＝ 現行アプリのバグ再現
#
# 使い方: python exp_waitgrow.py <ip> <poll|nopoll|abandon> <分>
import sys, json, time, urllib.request, urllib.error, socket

IP   = sys.argv[1]
MODE = sys.argv[2] if len(sys.argv) > 2 else "poll"
MINS = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0
BASE = f"http://{IP}:8080"

def get(path, timeout=15):
    return urllib.request.urlopen(BASE + path, timeout=timeout).read()

def post(path, body, timeout=15):
    req = urllib.request.Request(BASE + path, method="POST",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(req, timeout=timeout).read()

def put(path, body, timeout=15):
    req = urllib.request.Request(BASE + path, method="PUT",
                                 data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    return urllib.request.urlopen(req, timeout=timeout).read()

cat = json.loads(get("/ccapi"))
def find(sfx):
    hit = [a["path"] for v, l in cat.items() if isinstance(l, list)
           for a in l if a.get("path", "").endswith(sfx)]
    return hit[-1] if hit else None

P_SHUT = find("control/shutterbutton")
P_POLL = find("/event/polling")
P_CONT = find("/contents")

# コンテンツの総数を数える(nopoll方式の検知に使う)
def contents_dir():
    card = json.loads(get(P_CONT))["path"][-1]
    return json.loads(get(card))["path"][-1]
DIR = contents_dir()
def total():
    return json.loads(get(DIR + "?type=all&kind=number"))["contentsnumber"]

# 放棄されたロングポールを掃除してから始める
try:
    req = urllib.request.Request(BASE + P_POLL, method="DELETE")
    urllib.request.urlopen(req, timeout=10).read()
except Exception:
    pass

print(f"# ip={IP} mode={MODE} {MINS}分  shutter={P_SHUT}\n# n,elapsed_s,wait_ms,note")
t_end = time.time() + MINS * 60
n = 0
base_total = total() if MODE == "nopoll" else None
t0_all = time.time()

while time.time() < t_end:
    n += 1
    # --- バグ再現: continue=on を3秒で放棄する(現行アプリの初回probeと同じ) ---
    if MODE == "abandon":
        try:
            urllib.request.urlopen(BASE + P_POLL + "?continue=on", timeout=3).read()
        except Exception:
            pass   # 3秒で放棄。カメラは掴んだまま
    # --- シャッター ---
    t_sh = time.perf_counter()
    try:
        post(P_SHUT, {"af": False})
    except Exception as e:
        print(f"{n},{time.time()-t0_all:.0f},,shutter_err:{type(e).__name__}")
        time.sleep(2); continue
    # --- 新ファイルが見えるまで待つ ---
    note = "ok"
    if MODE == "nopoll":
        want = (base_total or 0) + 1
        while time.perf_counter() - t_sh < 20:
            try:
                if total() >= want: base_total = want; break
            except Exception: pass
            time.sleep(0.2)
        else: note = "timeout"
    else:
        while time.perf_counter() - t_sh < 20:
            try:
                b = get(P_POLL, timeout=15)
                if b'"addedcontents"' in b: break
            except Exception: pass
            time.sleep(0.2)
        else: note = "timeout"
    wait_ms = (time.perf_counter() - t_sh) * 1000
    print(f"{n},{time.time()-t0_all:.0f},{wait_ms:.0f},{note}", flush=True)
    # 15秒周期に合わせる
    rest = 15.0 - (time.perf_counter() - t_sh)
    if rest > 0: time.sleep(rest)
