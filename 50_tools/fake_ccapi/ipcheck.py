#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""エッジのAPで IP が重複していないかを調べる。

やること
--------
1. エッジの DHCP 貸出表を読む(シリアル 'n')
2. AP のサブネットを総当たりして、UPnP 記述子(:49152)を返す=カメラ を列挙する
   (記述子は認証不要なので、カメラの CCAPI セッションを乱さない)
3. PC 自身の IP も見て、**カメラと同じアドレスを使っていないか**を判定する

重複の見つけ方
--------------
・PC が持っている IP でカメラの記述子が返る → その IP は二重に使われている
・貸出表に無いのに応答する IP がある → 次に来た機器へ配られる = 重複の予備軍

  python ipcheck.py            # 調べるだけ
  python ipcheck.py --reset N  # エッジを N 秒止めてから調べる(N=0 は瞬時リセット)
"""
import argparse
import re
import subprocess
import sys
import time
import urllib.request

import serial

HERE_PS = "apwifi.ps1"


def edge_leases(com="COM4"):
    """エッジの DHCP 貸出表(= esp_netif_dhcps_get_clients_by_mac の結果)。"""
    sp = serial.Serial()
    sp.port = com; sp.baudrate = 115200; sp.timeout = 0.5
    sp.dtr = False; sp.rts = False          # 触るとリセットするので触らない
    sp.open(); time.sleep(1.0)
    sp.reset_input_buffer(); sp.write(b"n")
    out = ""
    t0 = time.time()
    while time.time() - t0 < 5:
        b = sp.read(4096)
        if b:
            out += b.decode("utf-8", "replace")
    sp.close()
    m = re.search(r"NEIGHBOR\]\s*\d+ client\(s\):([^\r\n]*)", out)
    return sorted(m.group(1).split()) if m else []


def reset_edge(com="COM4", hold_sec=0.0):
    """EN を落として保持する(電池切れ相当)。hold=0 なら瞬時リセット。"""
    sp = serial.Serial(com, 115200, timeout=0.5)
    sp.dtr = False
    sp.rts = True
    time.sleep(max(0.2, hold_sec))
    sp.rts = False
    sp.close()


def wifi_reconnect():
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", HERE_PS, "-Ssid", "edge00"], capture_output=True)


def pc_ip():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "(Get-NetIPAddress -InterfaceAlias 'Wi-Fi' -AddressFamily IPv4"
                        " -ErrorAction SilentlyContinue).IPAddress"], capture_output=True)
    return r.stdout.decode("utf-8", "ignore").strip()


def scan_cameras(lo=2, hi=12):
    """記述子(:49152)を返すアドレスを列挙する。戻り: {ip: (nickname, serial)}"""
    found = {}
    for i in range(lo, hi + 1):
        ip = "192.168.4.%d" % i
        try:
            x = urllib.request.urlopen(
                "http://%s:49152/upnp/CameraDevDesc.xml" % ip, timeout=2
            ).read().decode("utf-8", "ignore")
            n = re.search(r"X_deviceNickname[^>]*>([^<]*)", x)
            s = re.search(r"<serialNumber>([^<]*)", x)
            found[ip] = (n.group(1) if n else "?", s.group(1) if s else "?")
        except Exception:
            pass
    return found


def report(tag, com):
    leases = edge_leases(com)
    mine = pc_ip()
    cams = scan_cameras()
    print("--- %s ---" % tag)
    print("  PC の IP        : %s" % mine)
    print("  DHCP 貸出表     : %s" % (", ".join(leases) if leases else "(なし)"))
    print("  実際に居るカメラ:")
    for ip in sorted(cams, key=lambda a: int(a.split(".")[-1])):
        nick, ser = cams[ip]
        mark = "  ★PCと同じIP!" if ip == mine else ""
        seen = "" if ip in leases else "  (貸出表に無い)"
        print("    %-14s %-18s %s%s%s" % (ip, nick, ser, seen, mark))
    dup = [ip for ip in cams if ip == mine]
    ghost = [ip for ip in cams if ip not in leases]
    print("  判定: 重複=%s / 貸出表に無い応答=%s"
          % ("あり " + dup[0] if dup else "なし", ", ".join(ghost) if ghost else "なし"))
    return cams, leases, mine


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--com", default="COM4")
    ap.add_argument("--reset", type=float, default=None,
                    help="エッジを何秒止めてから調べるか(0=瞬時リセット)")
    ap.add_argument("--wait", type=float, default=75, help="復帰後に待つ秒数")
    args = ap.parse_args()

    wifi_reconnect()
    before = report("リセット前", args.com)

    if args.reset is None:
        return

    print("\nエッジを %.1f 秒止めます..." % args.reset)
    reset_edge(args.com, args.reset)
    print("復帰を %.0f 秒待ちます..." % args.wait)
    time.sleep(args.wait)
    wifi_reconnect()
    print("")
    report("リセット後(停止 %.1f 秒)" % args.reset, args.com)


if __name__ == "__main__":
    main()
