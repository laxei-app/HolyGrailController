#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ETP(スマホ⇄エッジ)の最小クライアント。PC から計画を送って撮影を開始させる。

なぜ要るか
----------
台数を変えて何度も測るのに、そのつどスマホの画面を叩くのは再現性が無い
(計画リストは開始時刻の降順で並び替わるので座標決め打ちも危ない)。
プロトコルは etp.h/etp.cpp に全部書いてあるので、そこだけ写して PC から直接送る。

パケットの形(etp.cpp:46-66)
---------------------------
  u16 HEADER=0x8080 | u16 cmd | u16 method | u32 length | data | u32 TERMINAL=0x01234567 | u32 sum
  ※ すべてリトルエンディアン。data は4バイト境界まで空白詰め。
  ※ sum = 先頭から terminal までを 4バイト u32(LE) として総和した値の 2の補数。

開始の手順(edgeClient.cpp:401-457)
----------------------------------
  1) C_TIME    (M_PUT)  {"datetime":"...","utcOffsetMin":540}
  2) C_CAPTURE_PLAN (M_PUT) "<planId>\\t<plan json>"
  3) C_ACTION  (M_POST) "<planId>"
"""
import argparse
import io
import json
import socket
import struct
import sys
import time

HEADER = 0x8080
TERMINAL = 0x01234567

M_GET, M_PUT, M_POST, M_DELETE, M_ACK, M_NAK = 1, 2, 3, 4, 100, 200
C_ACTION, C_STOP, C_PROGRESS = 1, 2, 3
C_CAPTURE_PLAN, C_TIME, C_SEARCH = 5, 7, 1000
C_DELETE_PLAN = 13


def _sum_words(b):
    s = 0
    for i in range(0, len(b), 4):
        w = 0
        for k in range(4):
            if i + k < len(b):
                w |= b[i + k] << (8 * k)
        s = (s + w) & 0xFFFFFFFF
    return s


def encode(cmd, method, data):
    d = data.encode("utf-8") if isinstance(data, str) else data
    while len(d) % 4:
        d += b" "
    v = struct.pack("<HHHI", HEADER, cmd, method, len(d)) + d + struct.pack("<I", TERMINAL)
    return v + struct.pack("<I", (0 - _sum_words(v)) & 0xFFFFFFFF)


def decode(buf):
    """1パケット解釈。戻り: (消費バイト数, cmd, method, data) / (0,..)=不足"""
    if len(buf) < 10:
        return 0, 0, 0, b""
    hdr, cmd, method, ln = struct.unpack("<HHHI", buf[:10])
    if hdr != HEADER:
        return -1, 0, 0, b""
    total = 10 + ln + 8
    if len(buf) < total:
        return 0, 0, 0, b""
    return total, cmd, method, buf[10:10 + ln].rstrip(b" \x00")


class Edge:
    def __init__(self, host, port=50506, timeout=20.0):
        self.s = socket.create_connection((host, port), timeout=6.0)
        self.s.settimeout(timeout)
        self.rx = b""

    def xchg(self, cmd, method, data=""):
        self.s.sendall(encode(cmd, method, data))
        t0 = time.time()
        while time.time() - t0 < self.s.gettimeout():
            n, c, m, d = decode(self.rx)
            if n > 0:
                self.rx = self.rx[n:]
                return m, d.decode("utf-8", "ignore")
            if n < 0:
                self.rx = self.rx[1:]
                continue
            chunk = self.s.recv(8192)
            if not chunk:
                break
            self.rx += chunk
        raise TimeoutError("cmd=%d の応答が来ない" % cmd)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=50506)
    ap.add_argument("--plan", help="送る計画の JSON ファイル")
    ap.add_argument("--id", default="measure01", help="計画 id")
    ap.add_argument("--action", choices=["start", "stop", "search", "progress", "delete"],
                    default="start")
    args = ap.parse_args()

    e = Edge(args.host, args.port)
    try:
        if args.action == "search":
            m, d = e.xchg(C_SEARCH, M_GET)
            print("C_SEARCH ->", m, d)
            return

        if args.action == "progress":
            m, d = e.xchg(C_PROGRESS, M_GET, args.id)
            print("C_PROGRESS ->", m, d)
            return

        # 1) 時刻同期
        off = -time.timezone // 60
        if time.localtime().tm_isdst:
            off = -time.altzone // 60
        tj = json.dumps({"datetime": now_iso(), "utcOffsetMin": off})
        m, d = e.xchg(C_TIME, M_PUT, tj)
        print("C_TIME -> %s %s" % ("ACK" if m == M_ACK else "NAK(%d)" % m, d))
        if m != M_ACK:
            sys.exit(1)

        if args.action == "stop":
            m, d = e.xchg(C_STOP, M_POST, args.id)
            print("C_STOP -> %s %s" % ("ACK" if m == M_ACK else "NAK(%d)" % m, d))
            return
        if args.action == "delete":
            m, d = e.xchg(C_DELETE_PLAN, M_DELETE, args.id)
            print("C_DELETE_PLAN -> %s %s" % ("ACK" if m == M_ACK else "NAK(%d)" % m, d))
            return

        # 2) 計画
        plan = io.open(args.plan, encoding="utf-8").read()
        body = args.id + "\t" + plan
        print("C_CAPTURE_PLAN 送信 %d バイト..." % len(body))
        m, d = e.xchg(C_CAPTURE_PLAN, M_PUT, body)
        print("C_CAPTURE_PLAN -> %s %s" % ("ACK" if m == M_ACK else "NAK(%d)" % m, d))
        if m != M_ACK:
            sys.exit(1)

        # 3) 開始
        m, d = e.xchg(C_ACTION, M_POST, args.id)
        print("C_ACTION -> %s %s" % ("ACK" if m == M_ACK else "NAK(%d)" % m, d))
    finally:
        e.close()


if __name__ == "__main__":
    main()
