# -*- coding: utf-8 -*-
# エッジに溜まった撮影レポートを ETP で覗く/引き取る検証ツール(2026-08-05)。
#
# 【何のためか】スマホの30秒スイープがやっている「検索応答の reports を見る →
#  C_REPORT_LIST/READ で引き取る → 保存できたら C_REPORT_DELETE で消す」を、
#  スマホを介さずPCから同じ手順で確かめる。プロトコル側だけを切り分けたいときに使う。
#
# 使い方:
#   python edge_reports.py 192.168.1.11            # 件数と一覧を見るだけ(消さない)
#   python edge_reports.py 192.168.1.11 --pull DIR # DIR へ保存する(消さない)
#   python edge_reports.py 192.168.1.11 --pull DIR --delete  # 保存できたものを消す
import argparse, json, os, socket, struct, sys

HEADER, TERMINAL = 0x8080, 0x01234567
C_SEARCH, C_REPORT_LIST, C_REPORT_READ, C_REPORT_DELETE = 1000, 14, 15, 16
M_GET, M_DELETE, M_ACK = 1, 4, 100
PORT_DISCOVERY, PORT_CONTROL = 50505, 50506


def _sum_words(b):
    s = 0
    for i in range(0, len(b), 4):
        w = b[i:i + 4] + b"\x00" * (4 - len(b[i:i + 4]))
        s += struct.unpack("<I", w)[0]
    return s & 0xFFFFFFFF


def encode(cmd, method, data=b""):
    if isinstance(data, str):
        data = data.encode()
    while len(data) % 4:
        data += b" "
    v = struct.pack("<HHHI", HEADER, cmd, method, len(data)) + data + struct.pack("<I", TERMINAL)
    return v + struct.pack("<I", (-_sum_words(v)) & 0xFFFFFFFF)


def decode(buf):
    """return (cmd, method, data) or None(不足/不正)"""
    if len(buf) < 10:
        return None
    hdr, cmd, method, length = struct.unpack("<HHHI", buf[:10])
    if hdr != HEADER or length % 4 or len(buf) < 18 + length:
        return None
    if struct.unpack("<I", buf[10 + length:14 + length])[0] != TERMINAL:
        return None
    data = buf[10:10 + length].rstrip(b" \x00")
    return cmd, method, data


def request(ip, cmd, method, data=b"", timeout=8.0):
    """TCP制御ポートへ1コマンド投げて応答を返す。return (method, data) / (0, b'')"""
    s = socket.create_connection((ip, PORT_CONTROL), timeout=timeout)
    try:
        s.sendall(encode(cmd, method, data))
        rx = b""
        for _ in range(64):
            r = decode(rx)
            if r:
                return r[1], r[2]
            chunk = s.recv(4096)
            if not chunk:
                break
            rx += chunk
    finally:
        s.close()
    return 0, b""


def search(ip, timeout=3.0):
    """UDP検索で edgeInfo を得る(reports 件数はここに載る)。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.sendto(encode(C_SEARCH, M_GET), (ip, PORT_DISCOVERY))
        buf, _ = s.recvfrom(4096)
        r = decode(buf)
        return json.loads(r[2]) if r else None
    except Exception:
        return None
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--pull", metavar="DIR", help="保存先。指定すると中身を取得して書く")
    ap.add_argument("--delete", action="store_true", help="保存できたものをエッジから消す")
    a = ap.parse_args()

    info = search(a.ip)
    if info is None:
        print("検索応答なし(UDP)。TCPで一覧だけ試す")
    else:
        print("edgeInfo: name=%s state=%s reports=%s" %
              (info.get("name"), info.get("state"), info.get("reports", 0)))

    m, d = request(a.ip, C_REPORT_LIST, M_GET)
    if m != M_ACK:
        print("C_REPORT_LIST 失敗 method=%d" % m)
        return 1
    try:
        lst = json.loads(d or b"[]")
    except Exception as e:
        print("一覧のJSONが壊れている: %s / %r" % (e, d[:200]))
        return 1
    print("レポート %d 件" % len(lst))
    for e in lst:
        print("  %-46s %s / %s  %s枚 所見%s" %
              (e.get("name", ""), e.get("plan", ""), e.get("camera", ""),
               e.get("frames", 0), e.get("noteCount", 0)))

    if not a.pull:
        return 0
    os.makedirs(a.pull, exist_ok=True)
    for e in lst:
        name = e.get("name", "")
        if not name:
            continue
        m, body = request(a.ip, C_REPORT_READ, M_GET, name)
        if m != M_ACK or not body:
            print("  読めない: %s (method=%d)" % (name, m))
            continue
        try:
            json.loads(body)  # スマホと同じく、読めることを確かめてから保存する
        except Exception as ex:
            print("  JSONとして壊れている: %s (%s)" % (name, ex))
            continue
        with open(os.path.join(a.pull, name), "wb") as f:
            f.write(body)
        print("  保存: %s (%d bytes)" % (name, len(body)))
        if a.delete:
            m, _ = request(a.ip, C_REPORT_DELETE, M_DELETE, name)
            print("    削除: %s" % ("OK" if m == M_ACK else "NAK(method=%d)" % m))
    return 0


if __name__ == "__main__":
    sys.exit(main())
