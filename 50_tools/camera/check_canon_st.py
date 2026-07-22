# カメラが起きている状態で、アプリと同じ Canon 独自 ST に応答するかを単独で確かめる。
#  (先の検索で「応答なし」に見えたのは、カメラが寝ていた順序の影響ではないかの検証)
import socket, sys, re, time

LOCAL = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.3"
ST = "urn:schemas-canon-com:service:ICPO-SmartPhoneEOSSystemService:1"


def msearch(st, rounds=3, timeout=6.0):
    msg = ("M-SEARCH * HTTP/1.1\r\n"
           "HOST: 239.255.255.250:1900\r\n"
           "MAN: \"ssdp:discover\"\r\n"
           "MX: 3\r\n"
           "ST: %s\r\n\r\n" % st).encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((LOCAL, 0))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(LOCAL))
    s.settimeout(timeout)
    hits = {}
    try:
        for _ in range(rounds):
            s.sendto(msg, ("239.255.255.250", 1900))
            time.sleep(0.3)
        while True:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                break
            t = data.decode("utf-8", "replace")
            m = re.search(r"^ST\s*:\s*(.+)$", t, re.I | re.M)
            hits.setdefault(addr[0], set()).add((m.group(1).strip() if m else "?"))
    finally:
        s.close()
    return hits


for trial in (1, 2):
    print("--- 試行%d: Canon独自ST ---" % trial)
    h = msearch(ST)
    if not h:
        print("  応答なし")
    for ip, sts in sorted(h.items()):
        print("  %-15s ST=%s" % (ip, ", ".join(sts)))
    time.sleep(1)
