# -*- coding: utf-8 -*-
# 撮影→記録待ち→サムネ取得 を延々繰り返して、カメラが固まるかを確かめる耐久試験。
#
# 【何のためか】2026-07-30、R10 が撮影中に「記録中(During shooting or recording)」から
#  抜けられなくなり、撮影エンジンだけが固まる事象が繰り返し発生した。
#  同じソフト・同じRAW設定・同じ15秒周期で R100 は3時間平気だったので、R10 固有の疑い。
#  SDカードを入れ替えて切り分けたいが、夕方以降はアプリを動かすと夜間撮影に入ってしまい
#  露出制御が絡んで条件が濁る。そこでアプリと同じ叩き方だけを PC から再現する。
#
# 【アプリと同じにしてあるところ】
#   1. POST shooting/control/shutterbutton {"af":false}
#      503 の間はそのコマの締め切りまで再試行(アプリの fireShutter と同じ)
#   2. 露光終了後 --settle 秒待ってからカードに触る(アプリの kCardSettleMs=3秒)
#   3. contents の総数(?type=all&kind=number)を --gap ms 間隔で監視し、増えたら
#   4. ?type=all&kind=list&page=N で最新のパスを取り、
#   5. そのファイルの ?kind=thumbnail を取得する
#   露出は一切触らない(測光・露出補正はこの試験の対象外。ファイル操作だけを見る)。
#
# 使い方:
#   python soak_shoot.py 192.168.1.3 --min 180 --tag r10_cardB
#   → soak_r10_cardB_192.168.1.3.csv に1コマ1行で記録し、異常は画面にも出す
#
# 途中で Ctrl+C を押せば、そこまでの集計を出して終了する。
import argparse, csv, json, os, socket, sys, time, urllib.error, urllib.request


def now_hms():
    return time.strftime("%H:%M:%S")


class Cam:
    def __init__(self, ip, port=8080, timeout=10):
        self.base = f"http://{ip}:{port}"
        self.ip = ip
        self.port = port
        self.timeout = timeout

    def _req(self, path, method="GET", body=None, timeout=None):
        """return (status, ms, bytes). status=-1 は応答なし(接続失敗/タイムアウト)。"""
        url = path if path.startswith("http") else self.base + path
        data = json.dumps(body).encode() if body is not None else None
        hdr = {"Content-Type": "application/json"} if data else {}
        t = time.perf_counter()
        try:
            r = urllib.request.Request(url, method=method, data=data, headers=hdr)
            x = urllib.request.urlopen(r, timeout=timeout or self.timeout)
            return x.status, (time.perf_counter() - t) * 1000.0, x.read()
        except urllib.error.HTTPError as e:
            return e.code, (time.perf_counter() - t) * 1000.0, e.read()
        except Exception:
            return -1, (time.perf_counter() - t) * 1000.0, b""

    def get(self, path, timeout=None):
        return self._req(path, "GET", None, timeout)

    def post(self, path, body, timeout=None):
        return self._req(path, "POST", body, timeout)

    def catalog(self):
        st, _, b = self.get("/ccapi")
        if st != 200:
            raise RuntimeError(f"/ccapi HTTP{st}")
        return json.loads(b)

    def find(self, cat, suffix):
        hit = [a["path"] for v, l in cat.items() if isinstance(l, list)
               for a in l if a.get("path", "").endswith(suffix)]
        return hit[-1] if hit else None

    def last_path(self, url):
        """{"path":[...]} の最後の要素を絶対URLで返す。"""
        st, _, b = self.get(url)
        if st != 200:
            return None
        try:
            p = json.loads(b)["path"][-1]
        except Exception:
            return None
        return p if p.startswith("http") else self.base + p


def characterize(ip):
    """固まったカメラの様子を記録する。ping相当(TCP)・UPnP・CCAPI の生死を見る。"""
    out = []
    for port in (8080, 49152):
        s = socket.socket()
        s.settimeout(3.0)
        t = time.perf_counter()
        try:
            s.connect((ip, port))
            out.append(f"port{port}=OPEN({(time.perf_counter()-t)*1000:.0f}ms)")
        except ConnectionRefusedError:
            out.append(f"port{port}=refused")
        except Exception as e:
            out.append(f"port{port}={type(e).__name__}")
        finally:
            s.close()
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--min", type=float, default=180.0, help="試験時間[分]")
    ap.add_argument("--interval", type=float, default=15.0, help="撮影周期[秒]")
    ap.add_argument("--settle", type=float, default=3.0, help="露光後カードに触るまでの待ち[秒]")
    ap.add_argument("--gap", type=int, default=200, help="総数ポーリングの間隔[ms]")
    ap.add_argument("--budget", type=float, default=10.0, help="新規画像を待つ上限[秒]")
    ap.add_argument("--shutter-budget", type=float, default=8.0, help="シャッター再試行の上限[秒]")
    ap.add_argument("--no-thumb", action="store_true", help="サムネ取得をしない(比較用)")
    ap.add_argument("--tag", default="")
    a = ap.parse_args()

    cam = Cam(a.ip)
    cat = cam.catalog()
    p_shut = cam.find(cat, "control/shutterbutton")
    p_cont = cam.find(cat, "/contents")
    if not p_shut or not p_cont:
        print("shutterbutton か contents が見つかりません", file=sys.stderr)
        return 2

    # /contents → カード → ディレクトリ(アプリと同じ辿り方。card1/sd の違いを吸収する)
    card = cam.last_path(cam.base + p_cont)
    dirp = cam.last_path(card) if card else None
    if not dirp:
        print("保存先ディレクトリを特定できません", file=sys.stderr)
        return 2

    st, _, b = cam.get("/ccapi/ver100/deviceinformation")
    model = serial = "?"
    if st == 200:
        j = json.loads(b)
        model, serial = j.get("productname", "?"), j.get("serialnumber", "?")

    tag = a.tag or a.ip.replace(".", "_")
    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            f"soak_{tag}_{a.ip}.csv")
    print(f"# {model} serial={serial} ip={a.ip}")
    print(f"# dir={dirp}")
    print(f"# 周期{a.interval}s 待ち{a.settle}s サムネ{'なし' if a.no_thumb else 'あり'} "
          f"{a.min}分  → {csv_path}")
    print("# n  時刻      shut  wait   polls list  thumb  size    note")

    f = open(csv_path, "w", newline="", encoding="utf-8")
    w = csv.writer(f)
    w.writerow(["n", "time", "elapsed_s", "shutter_ms", "shutter_try", "shutter_status",
                "wait_ms", "polls", "list_ms", "thumb_ms", "thumb_bytes", "note"])

    t_end = time.time() + a.min * 60.0
    t0 = time.time()
    n = 0
    base_num = None
    first_busy = None
    first_recording = None
    first_dead = None
    dead_since = None
    stats = {"ok": 0, "shutter_busy": 0, "shutter_dead": 0, "wait_timeout": 0, "thumb_fail": 0}

    def count():
        st, ms, b = cam.get(dirp + "?type=all&kind=number")
        if st != 200:
            return None, None, st, ms, b[:80].decode("utf-8", "replace")
        try:
            j = json.loads(b)
            return int(j["contentsnumber"]), int(j.get("pagenumber", 1)), st, ms, ""
        except Exception:
            return None, None, st, ms, "parse"

    try:
        while time.time() < t_end:
            n += 1
            frame_start = time.perf_counter()
            note = []

            # --- 1) シャッター(503 の間は締め切りまで再試行) ---
            t_sh = time.perf_counter()
            tries = 0
            sh_status = 0
            while True:
                tries += 1
                sh_status, ms, body = cam.post(p_shut, {"af": False})
                if 200 <= sh_status <= 204:
                    break
                if (time.perf_counter() - t_sh) >= a.shutter_budget:
                    break
                time.sleep(0.3)
            shutter_ms = (time.perf_counter() - t_sh) * 1000.0
            if not (200 <= sh_status <= 204):
                msg = body[:60].decode("utf-8", "replace") if body else ""
                if sh_status == 503:
                    stats["shutter_busy"] += 1
                    note.append(f"shutter503:{msg}")
                    if first_busy is None:
                        first_busy = time.time()
                        print(f"!! {now_hms()} 初回 shutter 503 ({msg})  n={n}")
                else:
                    stats["shutter_dead"] += 1
                    note.append(f"shutter{sh_status}")
                    if first_dead is None:
                        first_dead = time.time()
                        print(f"!! {now_hms()} 初回 shutter 応答なし  n={n}  {characterize(a.ip)}")

            # --- 2) 記録中はカードに触らない ---
            time.sleep(a.settle)

            # --- 3) 総数が増えるのを待つ ---
            t_w = time.perf_counter()
            polls = 0
            page = 1
            grew = False
            while (time.perf_counter() - t_w) < a.budget:
                polls += 1
                num, pg, st, ms, err = count()
                if num is None:
                    if st == 503 and "recording" in err.lower():
                        if first_recording is None:
                            first_recording = time.time()
                            print(f"!! {now_hms()} 初回 contents 503 During shooting or recording  n={n}")
                        note.append("contents503")
                    elif st == -1:
                        note.append("contents_dead")
                        if first_dead is None:
                            first_dead = time.time()
                            print(f"!! {now_hms()} 初回 contents 応答なし  n={n}  {characterize(a.ip)}")
                else:
                    page = pg
                    if base_num is None:
                        base_num = num
                        grew = True
                        break
                    if num > base_num:
                        base_num = num
                        grew = True
                        break
                time.sleep(a.gap / 1000.0)
            wait_ms = (time.perf_counter() - t_w) * 1000.0
            if not grew:
                stats["wait_timeout"] += 1
                note.append("nogrow")
                if dead_since is None:
                    dead_since = time.time()
            else:
                dead_since = None

            # --- 4) 最新のパス / 5) サムネ取得 ---
            list_ms = thumb_ms = 0.0
            thumb_bytes = 0
            if grew:
                st, list_ms, b = cam.get(f"{dirp}?type=all&kind=list&page={page}")
                path = None
                if st == 200:
                    try:
                        j = json.loads(b)
                        key = "url" if "url" in j else "path"
                        path = j[key][-1]
                    except Exception:
                        note.append("list_parse")
                else:
                    note.append(f"list{st}")
                if path and not a.no_thumb:
                    u = path if path.startswith("http") else cam.base + path
                    st, thumb_ms, b = cam.get(u + "?kind=thumbnail", timeout=15)
                    if st == 200:
                        thumb_bytes = len(b)
                        stats["ok"] += 1
                    else:
                        stats["thumb_fail"] += 1
                        note.append(f"thumb{st}")
                elif path:
                    stats["ok"] += 1

            el = time.time() - t0
            row = [n, now_hms(), f"{el:.0f}", f"{shutter_ms:.0f}", tries, sh_status,
                   f"{wait_ms:.0f}", polls, f"{list_ms:.0f}", f"{thumb_ms:.0f}",
                   thumb_bytes, ";".join(note)]
            w.writerow(row)
            f.flush()
            if note or n % 20 == 1:
                print(f"{n:5d} {now_hms()} {shutter_ms:5.0f} {wait_ms:6.0f} {polls:4d} "
                      f"{list_ms:5.0f} {thumb_ms:6.0f} {thumb_bytes:7d}  {';'.join(note)}")

            # 完全に沈黙して2分たったら、これ以上叩いても意味がないので終える
            if dead_since and (time.time() - dead_since) > 120:
                print(f"\n!! {now_hms()} 2分間まったく記録が進みません。中断します。")
                print(f"   カメラの様子: {characterize(a.ip)}")
                break

            rest = a.interval - (time.perf_counter() - frame_start)
            if rest > 0:
                time.sleep(rest)
    except KeyboardInterrupt:
        print("\n中断しました")
    finally:
        f.close()

    dur = (time.time() - t0) / 60.0
    print(f"\n===== 集計  {model} {a.ip} =====")
    print(f"  {n}コマ / {dur:.1f}分   正常 {stats['ok']}")
    print(f"  シャッター503 {stats['shutter_busy']} / 応答なし {stats['shutter_dead']}")
    print(f"  記録待ち時間切れ {stats['wait_timeout']} / サムネ失敗 {stats['thumb_fail']}")
    for lab, t in (("初回 shutter 503", first_busy),
                   ("初回 contents 503(記録中)", first_recording),
                   ("初回 応答なし", first_dead)):
        if t:
            print(f"  {lab}: {time.strftime('%H:%M:%S', time.localtime(t))} "
                  f"(開始から {(t-t0)/60.0:.1f}分)")
    print(f"  終了時のカメラ: {characterize(a.ip)}")
    print(f"  CSV: {csv_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
