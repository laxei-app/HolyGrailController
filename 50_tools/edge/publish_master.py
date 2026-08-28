#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""機材マスタ(カメラ/レンズ)を公開リポジトリへ出す(2026-08-27)。

元は **アプリ同梱の assets/master/*.json**。ここを直してこの道具を走らせると、
公開リポジトリへ写して版を1つ進め、目録(manifest.json)を書き直す。
同梱側にも同じ目録を置くので、アプリは「手元(同梱)より公開が新しいか」を見るだけで済む。

版は2つ持つ
----------
  schema   … 構造の版。項目を増やす・意味を変えたときに上げる。
             アプリは自分が読める版を超えていたら**取り込まない**(古いままで動く)。
             機種を1台足すたびにアプリの更新を強いるのは無理があるので、中身とは分ける。
  revision … 中身の版。機種を足す・値を直すたびに1つ進める。単調増加の整数。
             日付そのものを版にすると同じ日に2回出したときに困るので、整数にして
             日付は updated に別で持つ。

大きさと SHA256 も入れる。途中で切れた一覧で上書きすると機材が選べなくなるため、
アプリは必ず照合してから採る。

  python publish_master.py            # 版を1つ進めて写す
  python publish_master.py --same-rev # 版はそのまま(書き間違いの直し等)

【公開リポジトリは指示があるまで更新しない(2026-08-28 ユーザー指示)】
この道具は走らせた時点で hgc-master を書き換える。普段は使わないこと。
機材マスタを直したいだけなら assets/master/*.json を直せばアプリはそれを使う
(同梱のほうが版が新しければ取り込む)。公開へ出すのはリリースのときだけ。
"""
import argparse
import hashlib
import io
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "40_src", "90_Target", "20_Android", "app", "src", "main", "assets", "master")
DST = os.path.abspath(os.path.join(REPO, "..", "hgc-master", "master"))

SCHEMA = 1
FILES = ["cameras.json", "lenses.json"]


def count_of(path):
    """一覧の件数(人が目録を見て気づけるように入れておく)。"""
    try:
        j = json.load(io.open(path, encoding="utf-8-sig"))
        if isinstance(j, dict):
            j = next(v for v in j.values() if isinstance(v, list))
        return len(j)
    except Exception:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--same-rev", action="store_true", help="版を進めない")
    ap.add_argument("--updated", default=None, help="日付(既定は目録の値を据え置き)")
    args = ap.parse_args()

    if not os.path.isdir(DST):
        os.makedirs(DST)
    mpath = os.path.join(DST, "manifest.json")
    old = json.load(io.open(mpath, encoding="utf-8")) if os.path.exists(mpath) else {}
    rev = int(old.get("revision", 0))
    if not args.same_rev:
        rev += 1

    files = []
    for n in FILES:
        s = os.path.join(SRC, n)
        if not os.path.exists(s):
            raise SystemExit("元が見つかりません: %s" % s)
        shutil.copyfile(s, os.path.join(DST, n))
        d = open(s, "rb").read()
        files.append({"name": n, "size": len(d),
                      "sha256": hashlib.sha256(d).hexdigest(),
                      "count": count_of(s)})
        print("  %-14s %7d バイト  %d 件" % (n, len(d), files[-1]["count"]))

    man = {"schema": SCHEMA, "revision": rev,
           "updated": args.updated or old.get("updated", ""),
           "files": files}
    body = json.dumps(man, ensure_ascii=False, indent=2) + "\n"
    io.open(mpath, "w", encoding="utf-8", newline="\n").write(body)
    # 同梱側にも同じ目録を置く。アプリはこれを「手元の版」として公開と比べる。
    io.open(os.path.join(SRC, "manifest.json"), "w", encoding="utf-8", newline="\n").write(body)

    print("版 %d / schema %d を書き出した" % (rev, SCHEMA))
    print("  公開: %s" % mpath)
    print("  同梱: %s" % os.path.join(SRC, "manifest.json"))
    print("※ push はしていません。中身を確かめてから手で push してください。")


if __name__ == "__main__":
    main()
