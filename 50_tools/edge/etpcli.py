#!/usr/bin/env python3
# エッジ端末と ETP(TCP 50506)で直接話す診断ツール。
#
# 【なぜ要るか】エッジの様子を見る手段はこれまで2つしか無かった。
#   ・シリアル … デバッグビルドだとログ量が UART の速度を超えて**取りこぼす**(実測で行が混ざる)
#   ・アプリの「ログ取得」 … 撮影中はメニューが無効で押せない。画面操作も要る
# どちらも「撮影中に何が起きているか」を見たいときに使えない。ETP を直接叩けば、
# 撮影を止めずに、状態・ログ・レポートをそのまま取れる。
#
# 【書式】データ構造仕様書43 §6 / 40_src/20_HolyGrailEntity/10_common/src/etp.h
#   header(u16=0x8080) cmd(u16) method(u16) length(u32) data[length] terminal(u32=0x01234567) sum(u32)
#   すべてリトルエンディアン。sum は header..terminal を 4 バイトごとの u32 として
#   総和した値の 2 の補数。data は 4 の倍数に空白で詰める。
#
# 使い方:
#   python etpcli.py <ip> info                 状態(名前/IP/保持している計画/セッション)
#   python etpcli.py <ip> logs                 ログファイル名の一覧
#   python etpcli.py <ip> log <name> [出力先]   ログ本文(分割転送を結合)
#   python etpcli.py <ip> reports              撮影レポートの一覧
#   python etpcli.py <ip> report <name>        撮影レポート1件
#   python etpcli.py find [開始 終了]           LAN からエッジを探す(既定 192.168.1.2-60)
import json
import socket
import struct
import sys

HEADER, TERMINAL, PORT = 0x8080, 0x01234567, 50506
C_SEARCH, C_LOG_LIST, C_LOG_READ = 1000, 11, 12
C_REPORT_LIST, C_REPORT_READ = 14, 15
M_GET, M_ACK, M_NAK = 1, 100, 200
CHUNK = 4096			# エッジ側 C_LOG_READ の 1 回あたり最大バイト数


def _sum_words(b):
    s = 0
    for i in range(0, len(b), 4):
        w = b[i:i + 4]
        s = (s + struct.unpack('<I', w + b'\x00' * (4 - len(w)))[0]) & 0xFFFFFFFF
    return s


def encode(cmd, method, data=b''):
    d = data + b' ' * ((-len(data)) % 4)		# 4 バイト境界へ空白で詰める
    v = struct.pack('<HHHI', HEADER, cmd, method, len(d)) + d + struct.pack('<I', TERMINAL)
    return v + struct.pack('<I', (-_sum_words(v)) & 0xFFFFFFFF)


def decode(buf):
    """先頭から 1 パケット取り出す。return (packet|None, 消費バイト数) / (None,-1)=不正"""
    if len(buf) < 10:
        return None, 0
    hdr, cmd, method, length = struct.unpack('<HHHI', buf[:10])
    if hdr != HEADER:
        return None, -1
    total = 10 + length + 8
    if len(buf) < total:
        return None, 0
    # 末尾の詰め空白は落とす(etp::decode と同じ)
    return (cmd, method, buf[10:10 + length].rstrip(b' ')), total


class session:
    """1本の接続で何度もやり取りする。

    エッジは単一クライアント方式(etpEdge.cpp の g_client)なので、要求のたびに接続を張り直すと、
    前の接続がまだ生きているとみなされて新しい接続が弾かれる(RST)。ログは1回 4096 バイトずつ
    なので数百KBだと張り直しが数十回になり、必ず踏む。開いたまま順に投げれば起きない
    (スマホの edgeClient も1接続で C_TIME→計画→開始と続けている)。
    """
    def __init__(self, ip, timeout=15):
        self.s = socket.create_connection((ip, PORT), timeout)
        self.s.settimeout(timeout)
        self.buf = b""

    def call(self, cmd, method, data=b""):
        self.s.sendall(encode(cmd, method, data))
        while True:
            p, n = decode(self.buf)
            if n < 0:
                return None
            if p:
                self.buf = self.buf[n:]
                return p
            try:
                c = self.s.recv(8192)
            except socket.timeout:
                return None
            if not c:
                return None
            self.buf += c

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def call(ip, cmd, method, data=b"", timeout=15):
    with session(ip, timeout) as se:
        return se.call(cmd, method, data)


def read_file(ip, cmd, name):
    """分割転送を結合する。各チャンクの末尾には番兵 0x01 が付く(末尾空白を守るため)。"""
    out, off = b'', 0
    se = session(ip, 20)	# 1本の接続で読み切る(張り直すと弾かれる)
    while True:
        p = se.call(cmd, M_GET, ("%s\t%d" % (name, off)).encode())
        if p is None or p[1] == M_NAK:
            break
        body = p[2]
        if body.endswith(b'\x01'):
            body = body[:-1]			# 番兵を外す
        if not body:
            break						# 空チャンク = EOF
        out += body
        off += len(body)
        if len(body) < CHUNK:
            break
        if off > 8 * 1024 * 1024:
            sys.stderr.write("8MB を超えたので打ち切りました\n")
            break
    se.close()
    return out


def find(lo=2, hi=60, net="192.168.1"):
    found = []
    for i in range(lo, hi + 1):
        ip = "%s.%d" % (net, i)
        s = socket.socket()
        s.settimeout(0.3)
        try:
            s.connect((ip, PORT))
            found.append(ip)
        except OSError:
            pass
        finally:
            s.close()
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__ or "usage: etpcli.py <ip> <info|logs|log|reports|report> ...")
        return 2
    if sys.argv[1] == 'find':
        lo = int(sys.argv[2]) if len(sys.argv) > 2 else 2
        hi = int(sys.argv[3]) if len(sys.argv) > 3 else 60
        for ip in find(lo, hi):
            p = call(ip, C_SEARCH, M_GET)
            name = '?'
            if p:
                try:
                    name = json.loads(p[2].decode('utf-8', 'replace')).get('name', '?')
                except ValueError:
                    pass
            print("%-15s %s" % (ip, name))
        return 0

    ip = sys.argv[1]
    what = sys.argv[2] if len(sys.argv) > 2 else 'info'

    if what == 'info':
        p = call(ip, C_SEARCH, M_GET)
        if not p:
            print("応答がありません"); return 1
        d = json.loads(p[2].decode('utf-8', 'replace'))
        print("name     :", d.get('name'))
        print("ip       :", d.get('ip'), " state:", d.get('state'))
        print("fw       :", d.get('fw'))
        print("plans    :", d.get('heldPlans'))
        print("sessions :", d.get('sessions'))
        if 'reports' in d:
            print("reports  :", d.get('reports'))
    elif what == 'logs':
        p = call(ip, C_LOG_LIST, M_GET)
        if not p:
            print("応答がありません"); return 1
        for n in json.loads(p[2].decode('utf-8', 'replace')):
            print(n)
    elif what == 'log':
        body = read_file(ip, C_LOG_READ, sys.argv[3])
        if len(sys.argv) > 4:
            open(sys.argv[4], 'wb').write(body)
            print("%d バイト -> %s" % (len(body), sys.argv[4]))
        else:
            sys.stdout.buffer.write(body)
    elif what == 'reports':
        p = call(ip, C_REPORT_LIST, M_GET)
        print(p[2].decode('utf-8', 'replace') if p else "応答がありません")
    elif what == 'report':
        p = call(ip, C_REPORT_READ, M_GET, sys.argv[3].encode())
        if not p or p[1] == M_NAK:
            print("取得できません(名前違い、または大きすぎる)"); return 1
        sys.stdout.buffer.write(p[2])
    else:
        print("不明な指示:", what); return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
