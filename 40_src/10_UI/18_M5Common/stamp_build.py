# -*- coding: utf-8 -*-
"""ビルドした firmware.bin にその場で「名前・版数・検査値」を刻む(2026-09-05)。

【なぜ要るか】スマホから USB で焼くとき、端末に今入っているものの素性を
アプリ先頭 +0x20(esp_app_desc_t)から読んで判断している。素の firmware.bin は
`name=arduino-lib-builder` / `version=<gitハッシュ>` / 検査値=0 のままなので、
**PC から `pio run -t upload` で焼いた端末はスマホから見ると「素性が読めない」**。
すると土台ごとの書き直し(FULL)になり、**Wi-Fi の SSID・パスワードなど NVS の設定が
まるごと消える**。実機で起きた(2026-09-05)。

公開用の `publish_fw.py` は `stamp_fw.py` を通していたので、公開物から焼いた端末は
読めていた。**同じ物なのに焼き方で結果が変わる**のが元凶なので、刻印をビルドの側へ
移し、PC から焼いても公開物から焼いても同じ端末になるようにする。

土台(ブートローダ・区切り)は元々一致している。PlatformIO は書き込み時に qio を dio へ
直して先頭の書式を合わせるので、`merge_bin --flash_mode dio --flash_freq 80m` で作る
公開イメージと**バイト単位で同じ**になる(2026-09-05 に照合して確認)。

刻む中身と番地は `50_tools/edge/stamp_fw.py` が唯一の出どころ。ここはそれを呼ぶだけ。
"""
Import("env")
import os
import sys
import importlib.util

# extra_scripts は exec() で読まれるので __file__ が無い。プロジェクト位置から辿る。
#   PROJECT_DIR = 40_src/90_Target/<機種>  →  リポジトリの根は3つ上。
PROJ = env.subst("$PROJECT_DIR")
ROOT = os.path.abspath(os.path.join(PROJ, "..", "..", ".."))
STAMP_PY = os.path.join(ROOT, "50_tools", "edge", "stamp_fw.py")
APP_NAME = "HolyGrailEdge"


def _load(path):
    spec = importlib.util.spec_from_file_location("stamp_fw", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def stampFirmware(source, target, env):
    binPath = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    if not os.path.exists(binPath):
        return
    # 【失敗は黙らせない】刻めていないことに気づかないまま焼くと、次にスマホから
    #  焼いたときに設定が消える。ビルドを止めて知らせる。
    if not os.path.exists(STAMP_PY):
        sys.stderr.write("[stamp] %s が見つかりません\n" % STAMP_PY)
        env.Exit(1)
        return
    sf = _load(STAMP_PY)
    ver = sf.read_version_from_header(ROOT)
    if not ver:
        sys.stderr.write("[stamp] edgeVersion.h から版数を読めません\n")
        env.Exit(1)
        return
    data = open(binPath, "rb").read()
    out, crc = sf.stamp(data, APP_NAME, ver)
    open(binPath, "wb").write(out)
    print("[stamp] %s %s を刻みました (検査値 0x%08X)" % (APP_NAME, ver, crc))


env.AddPostAction("$BUILD_DIR/firmware.bin", stampFirmware)
