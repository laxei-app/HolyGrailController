#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ファームの決まった場所に「アプリ名・版数・検査値」を刻む(2026-08-26)。

なぜ要るか
----------
スマホから USB で焼くとき、**端末に今入っているものが何か**を知りたい。同じ版数なら
焼く必要が無いし、違う版数なら本体だけ入れ替えて設定を残せる。中身が読めない/壊れて
いれば土台ごと書き直すしかない。そのために、決まった番地に名前と版数を置く。

どこに置くか
------------
ESP-IDF のアプリイメージは **先頭 +0x20 に esp_app_desc_t** を必ず持っている。
番地が仕様で決まっているので、こちらで場所を作る必要が無い。フラッシュ上では
アプリが 0x10000 から始まるので 0x10020 になる。

  desc+0    magic_word    4  0xABCD5432(これで場所の正しさが分かる)
  desc+16   version      32  ← ここへ私たちの版数
  desc+48   project_name 32  ← ここへ私たちの名前
  desc+176  min/max_efuse_blk_rev_full … **起動条件。絶対に触らないこと**
  desc+180  reserv2[0]    4  ← ここへ検査値(CRC32)

既定では arduino-lib-builder の版数と名前が入っている。私たちには使えない値なので、
本来の用途どおり自分たちの値へ書き換える。端末側の esp_app_get_description() も
これを返すようになるので、都合がよい。

検査値
------
version と project_name は隣り合っている(desc+16 から 64 バイト)。その 64 バイトの
CRC32 を reserv2[0] へ置く。読み出した値がこの検査値と合わなければ、そこは私たちの
ファームではないか壊れているので、土台ごと書き直す判断に使う。

【置き場所に注意】ESP-IDF 5.3 で esp_app_desc_t に min/max_efuse_blk_rev_full が足され、
desc+176 はもう予約領域ではない。ここへ書くとブートローダが
「Image requires efuse blk rev >= v328.11」と言って**起動しなくなる**。実機で壊した。
しかも esptool image_info の検査(checksum / validation hash)は通ってしまうので気づけない。
そのため、刻んだあとに起動条件が変わっていないことを必ず確かめている。

末尾のハッシュ
--------------
イメージには末尾 32 バイトに全体の SHA-256 が付いており(先頭+23 が hash_appended)、
ブートローダが起動時に照合する。**中身を書き換えたら必ず付け直すこと。**
忘れると起動しなくなる。

  python stamp_fw.py firmware.bin --name HolyGrailEdge --version 0.1.427
  python stamp_fw.py firmware.bin -o stamped.bin   (版数はファームから拾えないので必須)
"""
import argparse
import binascii
import hashlib
import io
import os
import re
import sys

APP_DESC_OFF = 0x20
MAGIC = 0xABCD5432
OFF_VERSION = 16
OFF_NAME = 48
LEN_FIELD = 32
# 【検査値は desc+180 へ。176 は空いていない(2026-08-26 実機で壊して判明)】
#  ESP-IDF 5.3 で esp_app_desc_t に min/max_efuse_blk_rev_full(u16 が2つ)が足され、
#  desc+176..180 はもう予約領域ではない。ここへ書くとブートローダが
#  「Image requires efuse blk rev >= v328.11」と言って**起動しなくなる**。
#  予約領域(reserv2)はその後ろの desc+180 から。
OFF_EFUSE_REV = 176      # ここは絶対に触らないこと
OFF_CRC = 180
CRC_SPAN = (OFF_VERSION, OFF_VERSION + LEN_FIELD * 2)   # version+project_name の 64 バイト


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def put_field(buf, off, text, size):
    """NUL 終端の固定長欄へ書く。長すぎるものは黙って切らずに弾く。"""
    raw = text.encode("ascii")
    if len(raw) >= size:
        raise SystemExit("'%s' は %d バイトの欄に収まりません" % (text, size))
    buf[off:off + size] = raw + b"\x00" * (size - len(raw))


def stamp(data, name, version):
    buf = bytearray(data)
    if buf[0] != 0xE9:
        raise SystemExit("アプリイメージではありません(先頭が 0xE9 でない)")
    d = APP_DESC_OFF
    if u32(buf, d) != MAGIC:
        raise SystemExit("+0x20 に app_desc がありません(magic=0x%08X)" % u32(buf, d))

    put_field(buf, d + OFF_VERSION, version, LEN_FIELD)
    put_field(buf, d + OFF_NAME, name, LEN_FIELD)

    crc = binascii.crc32(bytes(buf[d + CRC_SPAN[0]: d + CRC_SPAN[1]])) & 0xFFFFFFFF
    buf[d + OFF_CRC: d + OFF_CRC + 4] = crc.to_bytes(4, "little")

    # 起動条件(eFuse リビジョンの下限・上限)を巻き込んでいないことを必ず確かめる。
    #  ここを壊すと「イメージの検査には通るのに起動しない」という一番たちの悪い形になる。
    if bytes(buf[d + OFF_EFUSE_REV: d + OFF_EFUSE_REV + 4]) != data[d + OFF_EFUSE_REV: d + OFF_EFUSE_REV + 4]:
        raise SystemExit("起動条件(efuse rev)を書き換えてしまっています。置き場所を見直すこと")

    # 【守りが二重にある】中身を触ったので両方とも付け直す。片方でも忘れると起動しない。
    #  ・XOR の検査バイト … セグメントの中身を 0xEF から順に排他的論理和したもの
    #  ・末尾 32 バイトの SHA-256 … それより前すべての要約(先頭+23 が 1 のときだけ付く)
    hashed = (buf[23] == 1)
    ck_pos = len(buf) - 33 if hashed else len(buf) - 1
    buf[ck_pos] = segment_xor(buf)
    if hashed:
        buf[-32:] = hashlib.sha256(bytes(buf[:-32])).digest()
    return bytes(buf), crc


def segment_xor(buf):
    """セグメントの中身だけを 0xEF から排他的論理和する(イメージの検査バイトの作り方)。"""
    o = 24
    x = 0xEF
    for _ in range(buf[1]):
        ln = int.from_bytes(buf[o + 4:o + 8], "little")
        o += 8
        for b in buf[o:o + ln]:
            x ^= b
        o += ln
    return x


def read_version_from_header(repo_root):
    """40_src/10_UI/18_M5Common/edgeVersion.h から版数を拾う。"""
    p = os.path.join(repo_root, "40_src", "10_UI", "18_M5Common", "edgeVersion.h")
    if not os.path.exists(p):
        return None
    m = re.search(r'HGC_EDGE_VERSION\s+"([^"]+)"', io.open(p, encoding="utf-8-sig").read())
    return m.group(1) if m else None


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", ".."))

    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="アプリイメージ(firmware.bin)")
    ap.add_argument("-o", "--out", help="書き出し先(既定は上書きしない .stamped.bin)")
    ap.add_argument("--name", default="HolyGrailEdge")
    ap.add_argument("--version", default=None, help="既定は edgeVersion.h から拾う")
    args = ap.parse_args()

    version = args.version or read_version_from_header(repo)
    if not version:
        raise SystemExit("版数が決められません(--version で指定してください)")

    data = open(args.image, "rb").read()
    out, crc = stamp(data, args.name, version)
    dst = args.out or (os.path.splitext(args.image)[0] + ".stamped.bin")
    open(dst, "wb").write(out)

    print("刻みました: %s" % dst)
    print("  名前   : %s" % args.name)
    print("  版数   : %s" % version)
    print("  検査値 : 0x%08X (version+name の CRC32)" % crc)
    print("  末尾のハッシュ: %s" % ("付け直した" if data[23] == 1 else "無し"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
