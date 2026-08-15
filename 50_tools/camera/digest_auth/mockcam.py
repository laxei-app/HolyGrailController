# EOS R50 V の認証まわりを真似たモックサーバ。実測した挙動をそのまま再現する。
#   ・チャレンジは実機と同じ形(空白なしカンマ区切り、algorithm はクォート無し)
#   ・nc(ノンスカウンタ)は厳密に増加を要求し、使用済み/逆順は 401 で弾く(リプレイ検知)
#   ・認証失敗が続くと 403 {"message":"Not access"} で完全に締め出す(本体設定を入れ直すまで戻らない)
import hashlib, socket, sys, threading, time

# 資格情報は引数で渡す(既定はダミー)。実機のパスワードをここに書かないこと。
USER = sys.argv[2] if len(sys.argv) > 2 else "testuser"
PASS = sys.argv[3] if len(sys.argv) > 3 else "testpass"
REALM  = "CameraControlApi"
OPAQUE = "0123456789abcdef0123456789"		# 形だけ真似たダミー
NONCE  = "16805f0b2a8151fbadc71b8fdf3890d888701603"
LOCKOUT_AFTER = 3          # 認証失敗がこれだけ続いたら締め出す

lock      = threading.Lock()
seen_nc   = set()
max_nc    = 0
fail_run  = 0
locked    = False
stats     = {"ok": 0, "401": 0, "403": 0, "replay": 0}

def md5(s): return hashlib.md5(s.encode()).hexdigest()

def parse(h):
    out, i = {}, 0
    for part in h.split(","):
        if "=" not in part: continue
        k, v = part.split("=", 1)
        out[k.strip().lower()] = v.strip().strip('"')
    return out

def check(auth, method, uri):
    """返り値: (通ったか, 理由)"""
    global max_nc, fail_run
    if not auth or not auth.lower().startswith("digest "):
        return False, "no-auth"
    p = parse(auth[7:])
    if p.get("nonce") != NONCE:
        return False, "bad-nonce"
    nc = p.get("nc", "")
    try: ncv = int(nc, 16)
    except ValueError: return False, "bad-nc"
    with lock:
        # 実機と同じ: 使用済み/巻き戻った nc はリプレイとして弾く
        if nc in seen_nc or ncv <= max_nc:
            stats["replay"] += 1
            return False, "replay(nc=%s max=%d)" % (nc, max_nc)
        ha1 = md5("%s:%s:%s" % (USER, REALM, PASS))
        ha2 = md5("%s:%s" % (method, uri))
        want = md5("%s:%s:%s:%s:%s:%s" % (ha1, NONCE, nc, p.get("cnonce",""), p.get("qop",""), ha2))
        if p.get("response") != want:
            return False, "bad-response"
        seen_nc.add(nc); max_nc = ncv
    return True, "ok"

def handle(conn):
    global fail_run, locked
    try:
        conn.settimeout(5)
        data = b""
        while b"\r\n\r\n" not in data:
            c = conn.recv(4096)
            if not c: return
            data += c
        head = data.split(b"\r\n\r\n")[0].decode("latin1")
        lines = head.split("\r\n")
        method, uri = lines[0].split()[0], lines[0].split()[1]
        auth = ""
        for l in lines[1:]:
            if l.lower().startswith("authorization:"): auth = l.split(":", 1)[1].strip()

        with lock:
            if locked:
                stats["403"] += 1
                body = b'{"message":"Not access"}'
                conn.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))
                return

        good, why = check(auth, method, uri)
        with lock:
            if good:
                fail_run = 0; stats["ok"] += 1
            else:
                fail_run += 1; stats["401"] += 1
                if fail_run >= LOCKOUT_AFTER:
                    locked = True
                    print("  !! 締め出し発動 (連続失敗=%d, 直前の理由=%s)" % (fail_run, why), flush=True)
        if good:
            body = b'{"productname":"Canon EOS R50 V"}'
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))
        else:
            wa = ('Digest realm="%s",nonce="%s",domain="http://127.0.0.1:8080/ccapi",'
                  'opaque="%s",algorithm=MD5,qop="auth"' % (REALM, NONCE, OPAQUE)).encode()
            conn.sendall(b"HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: " + wa + b"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    except Exception:
        pass
    finally:
        try: conn.close()
        except Exception: pass

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099   # 使い方: mockcam.py [port] [user] [pass]
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(64)
print("mock EOS R50 V on 127.0.0.1:%d" % port, flush=True)
srv.settimeout(1)
end = time.time() + 600
last_report = time.time()
while time.time() < end:
    if time.time() - last_report > 5:
        last_report = time.time()
        print("  stats:", stats, flush=True)
    try: c, _ = srv.accept()
    except socket.timeout:
        if locked: break
        continue
    threading.Thread(target=handle, args=(c,), daemon=True).start()
time.sleep(0.3)
print("stats:", stats, "locked:", locked, flush=True)
