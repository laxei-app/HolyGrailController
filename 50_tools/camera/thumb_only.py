# -*- coding: utf-8 -*-
# 同じ1枚のサムネイルを取り続けるだけの試験。撮影も露出設定も一切しない。
#
# 【何のためか】2026-07-30〜31 の切り分けで、R10 だけが数十分で沈黙する事象が残った。
#  カード交換でも症状は R10 本体に残り、R100 は同条件で240分960コマ完走した。
#  残る変数は「記録直後のカード読み出し」。ここでは **書き込みを完全に外して読み出しだけ**を
#  繰り返し、読み出し単独でカメラが落ちるのかを見る。
#   ・落ちる   → 読み出しそのものが原因。サムネ測光は R10 では使えない
#   ・落ちない → 読み出し単独では無害。書き込みと読み出しの競合が原因
#
# 使い方:
#   python thumb_only.py 192.168.1.3 --count 1000 --interval 15
import argparse, csv, json, os, socket, sys, time, urllib.error, urllib.request


def hms():
    return time.strftime("%H:%M:%S")


def req(url, timeout=15):
    """return (status, ms, bytes). status=-1 は応答なし。"""
    t = time.perf_counter()
    try:
        x = urllib.request.urlopen(url, timeout=timeout)
        return x.status, (time.perf_counter() - t) * 1000.0, x.read()
    except urllib.error.HTTPError as e:
        return e.code, (time.perf_counter() - t) * 1000.0, e.read()
    except Exception:
        return -1, (time.perf_counter() - t) * 1000.0, b""


def characterize(ip):
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


def last_path(base, url):
    st, _, b = req(url)
    if st != 200:
        return None
    try:
        p = json.loads(b)["path"][-1]
    except Exception:
        return None
    return p if p.startswith("http") else base + p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--interval", type=float, default=15.0)
    ap.add_argument("--tag", default="")
    a = ap.parse_args()

    base = f"http://{a.ip}:8080"
    st, _, b = req(base + "/ccapi")
    if st != 200:
        print(f"/ccapi HTTP{st}", file=sys.stderr)
        return 2
    cat = json.loads(b)
    p_cont = [x["path"] for v, l in cat.items() if isinstance(l, list)
              for x in l if x.get("path", "").endswith("/contents")][-1]

    # /contents → カード → ディレクトリ → 先頭ページの1件目を固定して使う
    card = last_path(base, base + p_cont)
    dirp = last_path(base, card) if card else None
    if not dirp:
        print("保存先が見つかりません", file=sys.stderr)
        return 2
    st, _, b = req(dirp + "?type=all&kind=list&page=1")
    if st != 200:
        print(f"一覧 HTTP{st}", file=sys.stderr)
        return 2
    j = json.loads(b)
    key = "url" if "url" in j else "path"
    if not j.get(key):
        print("ファイルが1枚もありません", file=sys.stderr)
        return 2
    target = j[key][0]
    if not target.startswith("http"):
        target = base + target
    url = target + "?kind=thumbnail"

    st, _, b = req(base + "/ccapi/ver100/deviceinformation")
    model = json.loads(b).get("productname", "?") if st == 200 else "?"

    tag = a.tag or a.ip.replace(".", "_")
    csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            f"thumbonly_{tag}_{a.ip}.csv")
    print(f"# {model} {a.ip}")
    print(f"# 対象: {target}")
    print(f"# {a.count}回 / {a.interval}秒周期  (撮影も露出設定もしない。読み出しだけ)")
    print(f"# → {csv_path}")
    print("# n  時刻      ms     bytes   note")

    f = open(csv_path, "w", newline="", encoding="utf-8")
    w = csv.writer(f)
    w.writerow(["n", "time", "elapsed_s", "status", "ms", "bytes", "note"])

    t0 = time.time()
    ng = 0
    first_bad = None
    size0 = None
    for n in range(1, a.count + 1):
        t_f = time.perf_counter()
        st, ms, b = req(url)
        note = ""
        if st == 200:
            if size0 is None:
                size0 = len(b)
            elif len(b) != size0:
                note = f"size{len(b)}!={size0}"
        else:
            ng += 1
            note = (b[:60].decode("utf-8", "replace") if b else "応答なし")
            if first_bad is None:
                first_bad = time.time()
                print(f"!! {hms()} 初回の異常 n={n} HTTP={st} {note}  {characterize(a.ip)}")
        w.writerow([n, hms(), f"{time.time()-t0:.0f}", st, f"{ms:.0f}", len(b), note])
        f.flush()
        if note or n % 50 == 1:
            print(f"{n:5d} {hms()} {ms:6.0f} {len(b):7d}  {note}")
        # 連続10回失敗したら、これ以上続けても意味がないので終える
        if ng >= 10 and first_bad and (time.time() - first_bad) > 120:
            print(f"\n!! {hms()} 2分以上失敗が続いています。中断します。")
            break
        rest = a.interval - (time.perf_counter() - t_f)
        if rest > 0:
            time.sleep(rest)
    f.close()

    dur = (time.time() - t0) / 60.0
    print(f"\n===== 集計 {model} {a.ip} =====")
    print(f"  {n}回 / {dur:.1f}分   失敗 {ng}")
    if first_bad:
        print(f"  初回の異常: {time.strftime('%H:%M:%S', time.localtime(first_bad))} "
              f"(開始から {(first_bad-t0)/60.0:.1f}分)")
    else:
        print("  異常なし")
    print(f"  終了時のカメラ: {characterize(a.ip)}")
    print(f"  CSV: {csv_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
