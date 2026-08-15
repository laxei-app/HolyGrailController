# カメラの nc(ノンスカウンタ)受理条件を、認証失敗を最小限にして確かめる。
#   ① 認証なし → nonce をもらう
#   ② nc=1 で通ることを確認(失敗0)
#   ③ 大きく飛ばした nc で通るか(飛びを許すか)      ← 設計の分かれ目
#   ④ 戻した nc が弾かれるか(1回だけ意図的に失敗させる)
import hashlib, socket, sys
# 使い方: ncprobe.py <ip> <user> <pass>   資格情報をここに書かないこと。
if len(sys.argv) < 4:
    print("usage: ncprobe.py <ip> <user> <pass>"); sys.exit(2)
ip, USER, PW = sys.argv[1], sys.argv[2], sys.argv[3]
port, URI = 8080, "/ccapi/ver100/deviceinformation"

def md5(s): return hashlib.md5(s.encode()).hexdigest()
def req(auth=None):
    s = socket.create_connection((ip, port), 6)
    h = "GET %s HTTP/1.1\r\nHost: %s:%d\r\n" % (URI, ip, port)
    if auth: h += "Authorization: " + auth + "\r\n"
    h += "Connection: close\r\n\r\n"
    s.sendall(h.encode()); b = b""
    while True:
        c = s.recv(4096)
        if not c: break
        b += c
    s.close()
    head, _, body = b.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    wa = next((l.split(":", 1)[1].strip() for l in lines if l.lower().startswith("www-authenticate")), "")
    return lines[0], wa, body[:50].decode("latin1")
def parse(w):
    out = {}
    for p in w[7:].split(","):
        if "=" in p:
            k, v = p.split("=", 1); out[k.strip().lower()] = v.strip().strip('"')
    return out
def build(p, nc):
    ncs, cn = "%08x" % nc, "abcdef0123456789"
    ha1 = md5("%s:%s:%s" % (USER, p["realm"], PW)); ha2 = md5("GET:" + URI)
    resp = md5("%s:%s:%s:%s:auth:%s" % (ha1, p["nonce"], ncs, cn, ha2))
    return ('Digest username="%s", realm="%s", nonce="%s", uri="%s", algorithm=MD5, '
            'qop=auth, nc=%s, cnonce="%s", response="%s", opaque="%s"'
            % (USER, p["realm"], p["nonce"], URI, ncs, cn, resp, p.get("opaque", "")))

st, wa, _ = req()
print("① 認証なし        :", st)
if not wa:
    print("   チャレンジが来ない(締め出し中)。カメラの設定を入れ直してください。"); sys.exit(1)
p = parse(wa)
print("   nonce =", p["nonce"])

st, _, body = req(build(p, 1))
print("② nc=1            :", st, body[:40])
if "200" not in st:
    print("   nonce に使用済みの履歴が残っている(=接続をまたいで nc を覚えている)"); sys.exit(0)

st, _, body = req(build(p, 2))
print("③ nc=2 (連番)     :", st, body[:40])

st, _, body = req(build(p, 100000))
print("④ nc=100000 (飛び):", st, body[:40], "  <- 飛びを許すか")
big_ok = "200" in st

st, _, body = req(build(p, 50))
print("⑤ nc=50 (巻き戻し):", st, body[:40], "  <- 巻き戻しは弾かれるはず")

print()
print("結論: 飛び%s / 巻き戻し%s" % ("OK" if big_ok else "NG",
      "NG(弾かれる)" if "200" not in st else "OK(素通り)"))
