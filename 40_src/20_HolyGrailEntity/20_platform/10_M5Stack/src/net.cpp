#include "commonM5.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <mutex>
#include "httpAuth.h"		// ダイジェスト認証(401 を受けてから対応する)
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_idf_version.h>	// ESP_IDF_VERSION_MAJOR(2.x=IDF4 / 3.x=IDF5 の分岐に使用)
#include <lwip/sockets.h>	// 非ブロッキング connect + select による :8080 バッチ探索(§3.3 tier3)
#include <cstring>
#include <cstdio>
#include <vector>
#include <cerrno>
#include "net.h"
#include "debugOut.h"

namespace net
{

static void httpInit(void);
static void httpDeInit(void);
bool init()     {return true;}
bool deInit()   {return true;}

// 利用可能な全NICのIPアドレスを取得
std::vector<std::string> getLocalIpList() 
{
    std::vector<std::string> ips;
    if (WiFi.status() == WL_CONNECTED) {
        ips.push_back(WiFi.localIP().toString().c_str());
    }
    // 【APモードの自局も返す(2026-08-19)】この一覧は SSDP の M-SEARCH を投げる回数(NIC の数)に
    //  使われる。APモードでは STA として繋がっていないので従来は**空**になり、M-SEARCH が
    //  1回も飛ばなかった。撮影開始の探索は接続局IPを列挙する別経路を持つので気づかれなかったが、
    //  在否監視(identifyTargets)は SSDP だけなので、APモードでは常に0台=カメラはずっとオフライン
    //  扱いになっていた(待機中に×が出たまま戻らない。実機 Edge00/Edge01 で発生)。
    if ((WiFi.getMode() & WIFI_MODE_AP) != 0) {
        String ap = WiFi.softAPIP().toString();
        if (ap.length() > 0 && ap != "0.0.0.0") { ips.push_back(ap.c_str()); }
    }
    return ips;
}

// APモード時、自SoftAPに接続中のクライアントのIP一覧を返す(SSDP不使用のカメラ発見用)。
// エッジがDHCPサーバなので、接続局のMAC→IPを esp_netif から引ける。APでない時は空。
std::vector<std::string> apClientIps()
{
    std::vector<std::string> ips;
    if ((WiFi.getMode() & WIFI_MODE_AP) == 0) { return ips; }	// APモード時のみ
#if ESP_IDF_VERSION_MAJOR >= 5
    // ESP-IDF 5.x(Arduino 3.x)では esp_netif_get_sta_list / esp_netif_sta_list_t が削除された。
    // 代替: 接続局のMAC一覧(esp_wifi_ap_get_sta_list)→DHCPサーバのリース(get_clients_by_mac)でIPを引く。
    wifi_sta_list_t staList = {};
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK || staList.num <= 0) { return ips; }
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == nullptr) { return ips; }
    esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM] = {};
    int n = (staList.num > ESP_WIFI_MAX_CONN_NUM) ? ESP_WIFI_MAX_CONN_NUM : staList.num;
    for (int i = 0; i < n; ++i) { std::memcpy(pairs[i].mac, staList.sta[i].mac, 6); }
    if (esp_netif_dhcps_get_clients_by_mac(ap, n, pairs) != ESP_OK) { return ips; }
    for (int i = 0; i < n; ++i)
    {
        char buf[16] = {0};
        esp_ip4addr_ntoa(&pairs[i].ip, buf, sizeof(buf));
        if (buf[0] && std::strcmp(buf, "0.0.0.0") != 0) { ips.push_back(buf); }
    }
#else
    wifi_sta_list_t staList = {};
    if (esp_wifi_ap_get_sta_list(&staList) != ESP_OK) { return ips; }
    esp_netif_sta_list_t netifList = {};
    if (esp_netif_get_sta_list(&staList, &netifList) != ESP_OK) { return ips; }
    for (int i = 0; i < netifList.num; ++i)
    {
        char buf[16] = {0};
        esp_ip4addr_ntoa(&netifList.sta[i].ip, buf, sizeof(buf));
        if (buf[0] && std::strcmp(buf, "0.0.0.0") != 0) { ips.push_back(buf); }
    }
#endif
    if (!ips.empty())
    {
        std::string joined; for (auto& s : ips) { joined += s + " "; }
        DBGLN(col::CYN, "apClientIps: %d client(s): %s", (int)ips.size(), joined.c_str());
    }
    return ips;
}

// 限定サブネットのバッチ探索(§3.3 tier3)。自IP+マスクからホスト範囲を割り出し、非ブロッキング
// connect をバッチ並行して :port が開いているホストのIPを返す。生存かつサービス有りのIPだけが残る。
std::vector<std::string> scanSubnetPort(int port, int timeoutMs, int maxHosts)
{
    std::vector<std::string> found;
    if (WiFi.status() != WL_CONNECTED) { return found; }
    // 自IP/サブネットマスク(いずれも s_addr = ネットワークバイト順の値)からホスト順の整数へ。
    uint32_t ownH  = ntohl((uint32_t)WiFi.localIP());
    uint32_t maskH = ntohl((uint32_t)WiFi.subnetMask());
    if (maskH == 0) { return found; }
    uint32_t netH   = ownH & maskH;
    uint32_t bcastH = netH | ~maskH;
    // ホスト候補(ネットワーク/ブロードキャスト/自IPを除外)。maxHosts で上限。
    std::vector<uint32_t> cands;
    for (uint32_t h = netH + 1; h < bcastH && (int)cands.size() < maxHosts; ++h)
    {
        if (h == ownH) { continue; }
        cands.push_back(h);
    }
    auto ipStr = [](uint32_t h) -> std::string {
        char b[16]; std::snprintf(b, sizeof(b), "%u.%u.%u.%u",
            (unsigned)((h >> 24) & 0xFF), (unsigned)((h >> 16) & 0xFF),
            (unsigned)((h >> 8) & 0xFF), (unsigned)(h & 0xFF));
        return std::string(b);
    };
    const int BATCH = 10;	// lwIP のソケット数上限に配慮(既存のWiFi/HTTP用も消費するため控えめ)
    for (size_t i = 0; i < cands.size(); i += BATCH)
    {
        int fds[BATCH]; uint32_t ips[BATCH]; int n = 0, maxfd = -1;
        fd_set wf; FD_ZERO(&wf);
        for (int k = 0; k < BATCH && i + (size_t)k < cands.size(); ++k)
        {
            uint32_t hh = cands[i + k];
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) { continue; }
            int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
            struct sockaddr_in sa; std::memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(hh);
            int r = connect(s, (struct sockaddr*)&sa, sizeof(sa));
            if (r == 0) { found.push_back(ipStr(hh)); close(s); continue; }	// 即接続(稀)=開いている
            if (r < 0 && errno != EINPROGRESS) { close(s); continue; }		// 即エラー(到達不能等)
            fds[n] = s; ips[n] = hh; FD_SET(s, &wf); if (s > maxfd) { maxfd = s; } ++n;
        }
        if (n <= 0) { continue; }
        struct timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(maxfd + 1, nullptr, &wf, nullptr, &tv);
        for (int k = 0; k < n; ++k)
        {
            if (sel > 0 && FD_ISSET(fds[k], &wf))
            {	// 接続成立(SO_ERROR==0)なら :port が開いている。
                int err = 0; socklen_t el = sizeof(err);
                if (getsockopt(fds[k], SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) { found.push_back(ipStr(ips[k])); }
            }
            close(fds[k]);
        }
    }
    if (!found.empty())
    {
        std::string joined; for (auto& s : found) { joined += s + " "; }
        DBGLN(col::CYN, "scanSubnetPort(:%d): %d host(s): %s", port, (int)found.size(), joined.c_str());
    }
    return found;
}

// SSDP探索開始 (特定のNICを指定してUDP送信)
void* ssdpStart(const std::string& query, const std::string& localIp)
{
    WiFiUDP* udp = new WiFiUDP();

    // ★重要: M-SEARCHクライアントは ephemeral ローカルポートに bind する(1900へは bind しない)。
    //   以前は beginMulticast(239.255.255.250, 1900) で 1900 に bind + グループ参加していたが、混雑LANでは
    //   1900 に全機器の周期NOTIFYマルチキャストが殺到し、ESP32/lwIP の小さいUDP受信バッファが溢れて
    //   カメラのユニキャストM-SEARCH応答を取りこぼす(→NOCAMERA/間欠発見不能)。スマホ(Android)は
    //   ephemeralポート(sin_port=0)にbindするので1900の洪水と無縁で常に発見できていた。これに合わせる。
    //   マルチキャスト宛の「送信」にグループ参加は不要。応答はユニキャストで ephemeral ポートに返る。
    //   ※受動待ち受け(ssdpListenStart)は 1900 bind + IGMP参加のままで正しい(NOTIFY受信用)。
    udp->begin(0);   // ephemeral ローカルポートに bind(0 = lwIP が空きポートを自動割当)

    // M-SEARCH開始
    udp->beginPacket("239.255.255.250", 1900);
    udp->write((const uint8_t*)query.c_str(), query.length());
    DBGLN(col::YEL , "======================================");
    DBGLN(col::MAG, query.c_str());
    udp->endPacket();
    
    return (void*)udp;
}

bool ssdpRead(void* handle, std::string& answer) 
{
    if (!handle) return "";
    WiFiUDP* udp = (WiFiUDP*)handle;
    int packetSize = udp->parsePacket();
    if (packetSize) {
        answer.resize(packetSize + 1);
        int len = udp->read(answer.data(), packetSize);
        answer[len] = '\0';
        return true;
    }
    return false;
}

void ssdpClose(void* handle)
{
    if (handle) {
        WiFiUDP* udp = (WiFiUDP*)handle;
        udp->stop();
        delete udp;
    }
}

// SSDP受動待ち受け: M-SEARCH を送らず 1900 で待ち受け 239.255.255.250 の NOTIFY を受ける。
// beginMulticast が 1900 への bind + グループ参加(LWIP IGMP)を行う。読み/破棄は ssdpRead/ssdpClose を流用。
void* ssdpListenStart(void)
{
    WiFiUDP* udp = new WiFiUDP();
    if (!udp->beginMulticast(IPAddress(239, 255, 255, 250), 1900)) { delete udp; return nullptr; }
    return (void*)udp;
}

bool ssdpListenRead(void* handle, std::string& answer) { return ssdpRead(handle, answer); }
void ssdpListenClose(void* handle) { ssdpClose(handle); }
// 直前の http*() が受け取った HTTP ステータス。0=応答なし(接続失敗等)。
//  失敗が「カメラが 503 等で断った」のか「そもそも届かなかった」のかをログで区別するため。
//  Arduino HTTPClient は接続失敗を負値で返すので、その場合は 0 に丸めて「応答なし」とする。
static int g_lastHttpStatus = 0;
static inline int noteHttpStatus(int code) { g_lastHttpStatus = (code > 0) ? code : 0; return code; }
int lastHttpStatus(void) { return g_lastHttpStatus; }

// 失敗(code<=0)の理由を呼び出し側の response へ載せる(2026-08-05 診断)。
// status は 0 に丸められるため、上位では「応答なし」としか分からず、
//  ・TCP接続そのものができていない(エッジのソケット枯渇など、こちら側の問題)
//  ・接続はできたがカメラが返してこない(カメラ側の問題)
// を区別できなかった。HTTPClient の生の負値を残せばこれが分かる。
//  例) -1=CONNECTION_REFUSED / -11=READ_TIMEOUT(繋がったが無返答)
//
// 【重要】-1 は「相手が拒否した」とは限らない。Arduino の HTTPClient は
//  WiFiClient::connect() が false を返しただけで -1 にするので、こちら側で
//  ソケットや lwIP の資源が取れなかった場合も同じ -1 になる。区別するために errno も残す:
//   ECONNREFUSED(111)=相手が拒否(カメラ側) / ENOMEM・ENOBUFS・EMFILE・EADDRNOTAVAIL=こちら側の枯渇
//  errno は connect 失敗直後なら意味を持つ(それ以外では前の値が残っていることがある)。
static void noteHttpError(int code, std::string& response)
{
    if (code > 0) { return; }
    HTTPClient tmp;	// errorToString は静的な文字列表を引くだけ
    response = "err=" + std::to_string(code) + " " + std::string(tmp.errorToString(code).c_str())
             + " errno=" + std::to_string(errno);
}

// TCP接続を使い回す(keep-alive)。戻すときはこの行をコメントアウトするだけでよい。
//
// 【なぜ使い回すか(2026-08-05 実測)】リクエストごとに接続を張り直していたため、初期収束の
//  9秒間に約40本の接続を開閉していた。その密度だと connect() が 1.5秒(setConnectTimeout)
//  経っても完了しない回が繰り返し出る:
//    適用ms = 1504!, 311, 1504!, 259, 1504!   ('!'=失敗。1504=接続タイムアウト満了)
//  失敗しているのは常に接続の段階で、送受信そのものは健全(成功回は259〜311ms)。
//  接続が1回で済めば、失敗しうる箇所が40個から1個に減る。
//  撮影が始まると周期15秒に数本なので元々起きない(撮影中の set= は常に高速)。
#define     USE_KEEP_ALIVE
#if defined(USE_KEEP_ALIVE)
    ///////////////////////////////////////////////////////////////
    // keep-alive を使う http通信。応答を読んでもソケットを閉じず、次の要求で使い回す。
    //  ・HTTP関連はすべて netThread の単一ワーカーから呼ばれるので、静的インスタンス1本で足りる。
    //  ・使い回しているソケットを相手が黙って閉じていることがある。失敗したら1度だけ
    //    張り直して再送する(スマホ側 edgeClient の firstReq と同じ考え方)。
    #include <HTTPClient.h>
    #include <WiFi.h>

    // --- 接続は宛先ごとに1本ずつ持つ ---
    // 【1本しか持たない実装をやめた理由(2026-08-15)】
    //  ① HTTPClient は接続先を見ず connected() だけで使い回すため、宛先が変わったら閉じるしか
    //     なかった。カメラ2台を1台のエッジで回すと**毎リクエスト**再接続になり、以前 keep-alive で
    //     直した「connect が詰まる」状態(9秒に約40本で不通)へ逆戻りする。接続が 80〜5000ms と
    //     ばらつく機種(EOS R50 V 実測)では成立せず、HTTPS を載せれば毎回ハンドシェイクになる。
    //  ② **排他が無かった**。netThread のワーカーは2本(kWorkerCount)あるので、カメラAの通信中に
    //     別スレッドがカメラB宛で prepare() を呼ぶと、使用中の接続を横から end() していた。
    // 借りている間は busy にして、他スレッドから触らせない。
    constexpr int kMaxKeepConns = 3;	// ワーカー2本+別宛先1つぶんの余裕
    struct httpSlot
    {
        HTTPClient  cli;
        std::string ep;					// "http://host:port"
        bool        open = false;
        bool        busy = false;
    };
    static httpSlot   g_slots[kMaxKeepConns];
    static const char* kAuthHeaders[] = { "WWW-Authenticate" };	// 401 のチャレンジを読むため
    static std::mutex g_slotMtx;

    // "http://host:port/..." から host:port までを取り出す(接続先が変わったかの判定用)。
    static std::string endpointOf(const std::string& url)
    {
        const size_t p = url.find("://");
        if (p == std::string::npos) { return url; }
        const size_t s = url.find('/', p + 3);
        return url.substr(0, (s == std::string::npos) ? url.size() : s);
    }

    // 返す。dead=この接続はもう使えない(閉じる)。
    static void release(int slot, bool dead)
    {
        if (slot < 0) { return; }
        std::lock_guard<std::mutex> lk(g_slotMtx);
        if (dead)
        {
            g_slots[slot].cli.end();
            g_slots[slot].open = false;
            g_slots[slot].ep.clear();
        }
        g_slots[slot].busy = false;
    }

    // 借りる。同じ宛先の空きがあれば使い回し、無ければ空きスロットで張る。-1=借りられない。
    static int acquire(const std::string& url)
    {
        const std::string ep = endpointOf(url);
        int slot = -1;
        {
            std::lock_guard<std::mutex> lk(g_slotMtx);
            for (int i = 0; i < kMaxKeepConns; ++i)		// ① 同じ宛先の空き
            {
                if (!g_slots[i].busy && g_slots[i].open && g_slots[i].ep == ep) { slot = i; break; }
            }
            if (slot < 0)								// ② 未使用スロット
            {
                for (int i = 0; i < kMaxKeepConns; ++i)
                {
                    if (!g_slots[i].busy && !g_slots[i].open) { slot = i; break; }
                }
            }
            if (slot < 0)								// ③ 空いている他宛先を畳んで空ける
            {
                for (int i = 0; i < kMaxKeepConns; ++i)
                {
                    if (g_slots[i].busy) { continue; }
                    g_slots[i].cli.end(); g_slots[i].open = false; g_slots[i].ep.clear();
                    slot = i; break;
                }
            }
            if (slot < 0) { return -1; }				// 全部使用中(枠>ワーカー数なので通常起きない)
            g_slots[slot].busy = true;
        }
        httpSlot& sl = g_slots[slot];
        if (!sl.cli.begin(url.c_str())) { release(slot, true); return -1; }
        sl.cli.setReuse(true);							// 応答後もソケットを閉じない
        sl.cli.setConnectTimeout(kHttpConnectTimeoutMs);	// スマホと同じ値(net.h)
        sl.cli.setTimeout(kHttpIoTimeoutMs);
        sl.ep = ep; sl.open = true;
        return slot;
    }

    // その要求に付ける Authorization(まだ 401 を受けていない相手なら空)。
    //  ダイジェスト認証は事前判定が要らない。要求はサーバ(カメラ)から 401 で来る。
    static void addAuthHeader(HTTPClient& cli, const std::string& url, const char* method)
    {
        const std::string host = endpointOf(url);
        std::string path = url;
        const size_t p = url.find("://");
        if (p != std::string::npos)
        {
            const size_t s2 = url.find('/', p + 3);
            path = (s2 == std::string::npos) ? std::string("/") : url.substr(s2);
        }
        const std::string a = httpAuth::authorization(host, method, path);
        if (!a.empty()) { cli.addHeader("Authorization", a.c_str()); }
        cli.collectHeaders(kAuthHeaders, 1);	// 401 のときチャレンジを読むため
    }

    // 401 を受けた。チャレンジを覚えられたら true(=同じ要求を投げ直す価値がある)。
    static bool learnAuth(HTTPClient& cli, const std::string& url)
    {
        const std::string wa = cli.header("WWW-Authenticate").c_str();
        if (wa.empty()) { return false; }
        return httpAuth::learn(endpointOf(url), wa);
    }

    void httpInit(void){}
    void httpDeInit(void)
    {
        std::lock_guard<std::mutex> lk(g_slotMtx);
        for (int i = 0; i < kMaxKeepConns; ++i)
        {
            g_slots[i].cli.end(); g_slots[i].open = false; g_slots[i].ep.clear(); g_slots[i].busy = false;
        }
    }

    // ボディを伴う要求(POST/PUT/DELETE)。返り値=HTTPステータス(0=応答なし)。
    static int requestWithBody(const char* method, const std::string& url,
                               const std::string* body, std::string& response)
    {
        // 認証が要る相手へは1本ずつ投げる。nc(ノンスカウンタ)は到着順に増えていないと
        //  リプレイと見なされ、以後どれだけ正しく作っても 401 になる(EOS R50 V 実測)。
        //  錠は host ごとなので2台同時撮影の並行性は落ちない。
        httpAuth::hostGuard authLock(endpointOf(url));
        int code = 0;
        for (int authTry = 0; authTry < 2; ++authTry)		// 401 を受けたら1度だけ認証を付けて投げ直す
        {
            bool learned = false;
            code = 0;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                const int slot = acquire(url);
                if (slot < 0) { code = 0; break; }
                HTTPClient& cli = g_slots[slot].cli;
                if (body != nullptr) { cli.addHeader("Content-Type", "application/json"); }
                addAuthHeader(cli, url, method);
                code = (body != nullptr)
                     ? cli.sendRequest(method, reinterpret_cast<uint8_t*>(const_cast<char*>(body->c_str())),
                                       body->length())
                     : cli.sendRequest(method);
                if (code > 0)
                {
                    response = cli.getString().c_str();		// 401 でも読み切る(使い回すソケットを壊さない)
                    if (code == 401 && authTry == 0) { learned = learnAuth(cli, url); }
                    // 認証付きで通ったら実績を残す(次の同一 nonce の 401 を誤判定しないため)
                    else if (code != 401) { httpAuth::noteSuccess(endpointOf(url)); }
                    release(slot, false);
                    break;
                }
                release(slot, true);	// 使い回しが死んでいたかもしれない → 張り直して1度だけ再送
            }
            if (!learned) { break; }		// 401 でない、または資格情報が無い → そのまま返す
        }
        noteHttpStatus(code);
        noteHttpError(code, response);	// 失敗なら理由(接続不可か無返答か)を残す
        return code;
    }

    // null を含むデータ(ライブビュー等)を扱うため、GET は本文をストリームから長さ分だけ読む。
    // 使い回すソケットでは「Content-Length ぶん読み切る」ことが必須(読み残すと次の要求が壊れる)。
    // GET の実体。rangeHdr が非nullなら Range ヘッダを付ける(部分取得)。
    //  撮影画像のEXIFを読むのに先頭だけ取りたい。本体は数MB〜数十MBあるので全部は落とせない。
    static bool httpGetInner(const std::string& url, std::string& answer, const char* rangeHdr)
    {
        httpAuth::hostGuard authLock(endpointOf(url));	// 認証時は1本ずつ(nc の順序を守る)
        int code = 0;
        int slot = -1;			// 借りているスロット(本文を読み終えるまで手放さない)
        for (int authTry = 0; authTry < 2; ++authTry)		// 401 を受けたら1度だけ認証を付けて投げ直す
        {
            code = 0; slot = -1;
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                slot = acquire(url);
                if (slot < 0) { code = 0; break; }
                addAuthHeader(g_slots[slot].cli, url, "GET");
                if (rangeHdr != nullptr) { g_slots[slot].cli.addHeader("Range", rangeHdr); }
                code = g_slots[slot].cli.GET();
                if (code > 0) { break; }
                release(slot, true); slot = -1;	// 張り直して1度だけ再送
            }
            if (code != 401 && slot >= 0) { httpAuth::noteSuccess(endpointOf(url)); }
            if (code != 401 || authTry > 0 || slot < 0) { break; }
            // 401。本文を読み切ってからチャレンジを覚え、同じ要求を投げ直す。
            HTTPClient& cli = g_slots[slot].cli;
            const bool learned = learnAuth(cli, url);
            (void)cli.getString();
            release(slot, false); slot = -1;
            if (!learned) { break; }		// 資格情報が無い → 401 のまま上へ返す
        }
        noteHttpStatus(code);
        bool success = false;
        // 206 = 部分応答(Range を受け入れた)。200 と同じ手順で本文を読む。
        if ((code == 200 || code == 206) && slot >= 0)
        {
            HTTPClient& cli = g_slots[slot].cli;
            const int len = cli.getSize();
            if (len >= 0)
            {
                answer.resize(static_cast<size_t>(len));
                WiFiClient* stream = cli.getStreamPtr();
                const int recvd = (stream != nullptr) ? stream->readBytes(answer.data(), len) : 0;
                success = (recvd >= len);
                release(slot, !success);	// 読み残しは次の要求を壊すので切る
            }
            else
            {	// 長さ不明(chunked等)。使い回せないので読み切って閉じる。
                answer = cli.getString().c_str();
                success = true;
                release(slot, true);
            }
            slot = -1;
        }
        else
        {
            DBGLN(col::RED,"%s:url?(%s).",__func__, url.c_str());
        }
        release(slot, true);	// 上で返していなければここで捨てる(slot=-1 なら何もしない)
        return success;
    }

    bool httpGet(const std::string& url, std::string& answer)
    {
        return httpGetInner(url, answer, nullptr);
    }

    bool httpGetRange(const std::string& url, long from, long to, std::string& answer)
    {
        char hdr[48];
        std::snprintf(hdr, sizeof(hdr), "bytes=%ld-%ld", from, to);
        return httpGetInner(url, answer, hdr);
    }

    bool httpPost(const std::string& url, const std::string& body, std::string& response)
    {
        const int code = requestWithBody("POST", url, &body, response);
        return (code == 200 || code == 204);
    }

    bool httpPut(const std::string& url, const std::string& body, std::string& response)
    {
        const int code = requestWithBody("PUT", url, &body, response);
        return (code >= 200 && code <= 204);
    }

    bool httpDelete(const std::string& url, std::string& response)
    {
        const int code = requestWithBody("DELETE", url, nullptr, response);
        return (code == 200 || code == 202 || code == 204);
    }

#else
    ///////////////////////////////////////////////////////////////
    // 標準的な http 通信。毎回セッションをつなぎなおす。
    void httpInit(void){}
    void httpDeInit(void){};

    // null を含んだデータを扱えるようにストリームで受信する。
    // answer にあらかじめ受信するサイズ以上の容量を確保してあればメモリの
    // 再確保は発生しない。
    // 特に histogram 受信時はあらかじめ十分な容量を確保してから実行することを推奨する。
    // url    : サーバーurl
    // answer : 受信したデータを格納する領域
    // return : true:成功
    bool httpGet(const std::string& url, std::string& answer) 
    {
        bool success = false;

        HTTPClient http;
        http.begin(url.c_str());
        http.setConnectTimeout(kHttpConnectTimeoutMs);	// スマホと同じ値(net.h)
        http.setTimeout(kHttpIoTimeoutMs);
        int code = noteHttpStatus(http.GET());
        if(code == 200)
        {
            int len = http.getSize();
            int lenRecv = 0;
            answer.resize(len);
            WiFiClient* stream = http.getStreamPtr();
            lenRecv = stream->readBytes(answer.data(), len);   
            if(lenRecv >= len) { success = true;}
        }
        else
        {
            DBGLN(col::RED,"%s:url?(%s).",__func__, url.c_str());
        }
        http.end();
        return success;
    }

    bool httpPost(const std::string& url, const std::string& body, std::string& response) 
    {
        HTTPClient http;
        http.begin(url.c_str());
        http.setConnectTimeout(kHttpConnectTimeoutMs);	// スマホと同じ値(net.h)
        http.setTimeout(kHttpIoTimeoutMs);				// 以前は未設定でArduino既定の5000msだった
        int code = noteHttpStatus(http.POST(body.c_str()));
        if (code > 0) response = http.getString().c_str();
        noteHttpError(code, response);	// 失敗なら理由(接続不可か無返答か)を残す
        http.end();
        return (code == 200 || code == 204);
    }

    // HTTP PUT (M5Stack Core2用)
    bool httpPut(const std::string& url, const std::string& body, std::string& response)
    {
        HTTPClient http;
        // URLの開始
        http.begin(url.c_str());
        http.setConnectTimeout(kHttpConnectTimeoutMs);	// スマホと同じ値(net.h)
        http.setTimeout(kHttpIoTimeoutMs);				// 以前は未設定でArduino既定の5000msだった
        // Content-Typeを指定（APIの仕様に合わせて適宜変更してください）
        http.addHeader("Content-Type", "application/json");

        // PUTメソッドの実行
        int code = noteHttpStatus(http.PUT(body.c_str()));

        if (code > 0) {
            response = http.getString().c_str();
        }
        noteHttpError(code, response);	// 失敗なら理由(接続不可か無返答か)を残す

        http.end();
        // 200 (OK), 201 (Created), 204 (No Content) を成功と判定
        return (code >= 200 && code <= 204);
    }

    // HTTP DELETE (M5Stack Core2用)
    bool httpDelete(const std::string& url, std::string& response)
    {
        HTTPClient http;
        http.begin(url.c_str());
        http.setConnectTimeout(kHttpConnectTimeoutMs);	// スマホと同じ値(net.h)
        http.setTimeout(kHttpIoTimeoutMs);				// 以前は未設定でArduino既定の5000msだった

        // DELETEメソッドの実行
        int code = noteHttpStatus(http.sendRequest("DELETE"));

        if (code > 0) {
            response = http.getString().c_str();
        }

        http.end();
        // 200 (OK) や 204 (No Content) を成功と判定
        return (code == 200 || code == 202 || code == 204);
    }
#endif // USE_KEEP_ALIVE
}
