#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""ビルドした物に開発機のパスを埋めない(2026-09-27)。

Arduino のログ macro(log_e など)は `__FILE__` をそのまま文字列にするので、
何もしないと **配布するファームの中に開発機の絶対パスが残る**。実際
`C:/Users/<ユーザー名>/.platformio/packages/...` が 33 か所入っていた。
公開するファームにも、LGPL の再リンク材料にも、これを載せたくない。

`-ffile-prefix-map=<元>=<置換後>` は `__FILE__` と デバッグ情報の両方を書き換える
(GCC 8 以降。いまの toolchain は 13.2)。動く物は変わらず、文字列だけが短くなる。

  <PlatformIO のパッケージ置き場> → /pkg
  <このリポジトリの根>            → /app

【注意】`__FILE__` を比べている箇所があると結果が変わる。このプロジェクトには無い。
検査のしかた: ビルドした firmware.bin に開発機のパスが出ないことを見る。
  python -c "d=open('firmware.bin','rb').read(); print(d.count(b'/Users/'))"
"""
Import("env")   # noqa: F821
import os


def fwd(p):
    # コンパイラが `__FILE__` に入れるのは前向きスラッシュなので、こちらも揃える。
    #  食い違うと置き換えが起きず、黙って素通りする。
    return os.path.abspath(p).replace("\\", "/")


# PlatformIO のパッケージ置き場(framework / toolchain がここに居る)
pkgs = fwd(env.subst("$PROJECT_PACKAGES_DIR"))      # noqa: F821
# 90_Target/<機種> から3つ上がリポジトリの根
repo = fwd(os.path.join(env.subst("$PROJECT_DIR"), "..", "..", ".."))   # noqa: F821

flags = ["-ffile-prefix-map=%s=/pkg" % pkgs,
         "-ffile-prefix-map=%s=/app" % repo]

# CCFLAGS は C と C++ の両方に乗る。ASFLAGS にも入れておく(アセンブラ経由の .S 用)。
env.Append(CCFLAGS=flags, ASFLAGS=flags)            # noqa: F821
print("strip_paths: %s -> /pkg" % pkgs)
print("strip_paths: %s -> /app" % repo)
