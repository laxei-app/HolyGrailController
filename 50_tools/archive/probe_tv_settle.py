# シャッターを振って、ライブビューのヒストグラム中央値が「本当に追従しないのか」を
# フレームの新鮮さ(liveviewdata.systemtime)を見ながら測り直す。
#
#   前回「1秒で頭打ち」と結論したが、設定直後の“古いフレーム”を読んでいた可能性がある。
#   それならカメラの限界ではなくこちらのタイミングの問題。
#   各設定で systemtime が更新された新しいフレームだけを採り、値が落ち着くまでの推移も出す。
import sys, json, time, urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.12"
BASE = "http://%s:8080/ccapi" % HOST
SETTLE_SEC = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

TVS = ["1/1000", "1/250", "1/60", "1/15", "1/4", "1\"", "2\"", "4\"", "8\""]


def get(p, t=20):
    with urllib.request.urlopen(BASE + p, timeout=t) as r:
        return r.status, r.read()


def put(p, obj, t=20):
    req = urllib.request.Request(BASE + p, data=json.dumps(obj).encode(),
                                 headers={"Content-Type": "application/json"}, method="PUT")
    with urllib.request.urlopen(req, timeout=t) as r:
        return r.status, r.read()


def frame():
    """(systemtime_ms, Y中央値0-1) を返す。取れなければ None。"""
    for _ in range(4):
        try:
            st, b = get("/ver100/shooting/liveview/flipdetail?kind=info")
        except Exception:
            time.sleep(0.6); continue
        s = b.decode("utf-8", "replace")
        i = s.find('{"liveviewdata"')
        if i < 0:
            i = s.find("{")
        j = s.rfind("}")
        try:
            lv = json.loads(s[i:j + 1]).get("liveviewdata", {})
        except Exception:
            time.sleep(0.4); continue
        h = lv.get("histogram")
        stm = lv.get("systemtime") or {}
        if not h:
            time.sleep(0.4); continue
        y = h[0]; n = len(y); tot = float(sum(y))
        if tot <= 0:
            time.sleep(0.4); continue
        c = 0.0; med = n - 1
        for idx, v in enumerate(y):
            c += v
            if c >= tot / 2.0:
                med = idx; break
        ms = int(stm.get("sec", 0)) * 1000 + int(stm.get("subsec", 0))
        return ms, med / float(n - 1)
    return None


def main():
    for disp in ("keep", "on", "off"):
        try:
            req = urllib.request.Request(BASE + "/ver100/shooting/liveview",
                                         data=json.dumps({"liveviewsize": "small", "cameradisplay": disp}).encode(),
                                         headers={"Content-Type": "application/json"}, method="POST")
            urllib.request.urlopen(req, timeout=15).read()
            break
        except Exception:
            pass

    try:
        orig = json.loads(get("/ver100/shooting/settings/tv")[1].decode())["value"]
    except Exception:
        orig = "8\""
    print("元のTv:", orig)
    print("\n%-8s | %s" % ("Tv", "新フレームごとの Y中央値の推移 (→ 最終値)"))
    print("-" * 78)

    results = []
    for tv in TVS:
        try:
            put("/ver100/shooting/settings/tv", {"value": tv})
        except Exception as e:
            print("%-8s | 設定失敗 %s" % (tv, e)); continue

        f0 = frame()
        base_ms = f0[0] if f0 else 0
        seen, traj = set(), []
        t0 = time.time()
        while time.time() - t0 < SETTLE_SEC:
            f = frame()
            if f is None:
                continue
            ms, med = f
            if ms <= base_ms or ms in seen:   # 設定前の古いフレームは捨てる
                time.sleep(0.3); continue
            seen.add(ms); traj.append(med)
            if len(traj) >= 10:
                break
            time.sleep(0.3)

        if not traj:
            print("%-8s | 新しいフレームが来ない" % tv); continue
        shown = " ".join("%.4f" % v for v in traj[:8])
        final = sum(traj[-3:]) / len(traj[-3:])
        print("%-8s | %s  → %.4f" % (tv, shown, final))
        results.append((tv, final))

    try:
        put("/ver100/shooting/settings/tv", {"value": orig})
        print("\n元のTv(%s)へ戻しました。" % orig)
    except Exception as e:
        print("\n戻し失敗:", e)

    print("\n=== まとめ: 1段変えたら中央値は何倍になったか ===")
    for k in range(1, len(results)):
        pt, pv = results[k - 1]
        ct, cv = results[k]
        r = (cv / pv) if pv > 0 else float("inf")
        print("  %-7s → %-7s : %.4f → %.4f  (×%.2f)" % (pt, ct, pv, cv, r))
    print("\n※ライブビューが露出を正しく反映しているなら、1段(2倍)ごとに中央値も概ね倍々に増える。")
    print("  途中から増えなくなる=そこが頭打ち。最後まで増える=頭打ちは無く、前回の結論が誤り。")


if __name__ == "__main__":
    main()
