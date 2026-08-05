# -*- coding: utf-8 -*-
# エッジの当日ログを ETP(C_LOG_LIST / C_LOG_READ)でネットワーク越しに取る。
# Stick01 は SD が無くシリアルの 'l' が応答しないことがあるため、こちらを使う。
#
# 使い方: python edge_getlog.py 192.168.1.11 [出力パス]
import sys
from edge_reports import request, decode, encode, M_GET, M_ACK   # ETPの組立/解釈を流用

C_LOG_LIST, C_LOG_READ = 11, 12


def main():
    ip = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else "edge.log"

    m, d = request(ip, C_LOG_LIST, M_GET)
    if m != M_ACK:
        print("C_LOG_LIST 失敗 method=%d" % m)
        return 1
    import json
    names = json.loads(d or b"[]")
    if not names:
        print("ログ無し")
        return 1
    name = sorted(names)[-1]          # 一番新しい日付のもの
    print("取得: %s" % name)

    buf = b""
    off = 0
    while True:
        m, chunk = request(ip, C_LOG_READ, M_GET, "%s\t%d" % (name, off))
        if m != M_ACK:
            print("C_LOG_READ 失敗 method=%d offset=%d" % (m, off))
            break
        if chunk.endswith(b"\x01"):    # 末尾の番兵を除去(空白が消えないようにする印)
            chunk = chunk[:-1]
        if not chunk:
            break
        buf += chunk
        off += len(chunk)
    with open(out, "wb") as f:
        f.write(buf)
    print("%d bytes -> %s" % (len(buf), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
