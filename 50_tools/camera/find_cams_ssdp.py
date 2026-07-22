# SSDP M-SEARCH でカメラを探す。
#  ※ポート1900にbindするとNOTIFY洪水でバッファが溢れ、カメラの応答を取りこぼす
#    (エッジ側で実証済みの不具合)。必ず ephemeral ポートから送る。
import socket, sys, re, json, urllib.request

MCAST = ("239.255.255.250", 1900)
TARGETS = [
    "urn:schemas-canon-com:service:ICPO-SmartPhoneEOSSystemService:1",
    "upnp:rootdevice",
    "ssdp:all",
]


LOCAL = None   # 送信に使うローカルIP(複数NICがある環境で必須)


def msearch(st, wait=3, timeout=6.0):
    msg = ("M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: %d\r\n"
           "ST: %s\r\n\r\n" % (wait, st)).encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((LOCAL or "", 0))              # ephemeral(1900をbindしない)。複数NIC環境は送信元IPを固定する
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    if LOCAL:   # マルチキャストの送出インターフェースを明示(Hyper-V等の別NICへ出るのを防ぐ)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(LOCAL))
    s.settimeout(timeout)
    found = {}
    try:
        for _ in range(3):
            s.sendto(msg, MCAST)
        while True:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                break
            txt = data.decode("utf-8", "replace")
            found.setdefault(addr[0], set()).add(txt)
    finally:
        s.close()
    return found


def header(txt, name):
    m = re.search(r"^%s\s*:\s*(.+)$" % name, txt, re.I | re.M)
    return m.group(1).strip() if m else ""


def probe_ccapi(ip):
    try:
        with urllib.request.urlopen("http://%s:8080/ccapi/ver100/deviceinformation" % ip, timeout=5) as r:
            return json.loads(r.read().decode())
    except Exception as e:
        return {"_err": str(e)}


def main():
    global LOCAL
    if len(sys.argv) > 1:
        LOCAL = sys.argv[1]
    print("送信元IP:", LOCAL or "(既定)")
    all_hosts = {}
    for st in TARGETS:
        print("--- M-SEARCH ST=%s ---" % st)
        got = msearch(st)
        for ip, msgs in got.items():
            for m in msgs:
                srv = header(m, "SERVER")
                loc = header(m, "LOCATION")
                usn = header(m, "USN")
                key = (ip, loc)
                if key in all_hosts:
                    continue
                all_hosts[key] = True
                if "canon" in (srv + loc + usn).lower() or ":8080" in loc:
                    print("  %-15s  %s" % (ip, loc or srv))
        if not got:
            print("  応答なし")

    ips = sorted({ip for (ip, _) in all_hosts})
    print("\n=== SSDP応答のあったホスト: %d 件 ===" % len(ips))
    for ip in ips:
        info = probe_ccapi(ip)
        if "_err" in info:
            print("  %-15s CCAPI: %s" % (ip, info["_err"]))
        else:
            print("  %-15s ★ %s  serial=%s  fw=%s"
                  % (ip, info.get("productname"), info.get("serialnumber"), info.get("firmwareversion")))


if __name__ == "__main__":
    main()
