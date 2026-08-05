#include "commonM5.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
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

//#define     USE_KEEP_ALIVE
#if defined(USE_KEEP_ALIVE)
    ///////////////////////////////////////////////////////////////
    // keep-alive を使う http通信。セッションは切れない。
    #include <HTTPClient.h>
    #include <WiFi.h>

    static HTTPClient http_;
    static bool request_with_body(const std::string& url, const std::string& method, const std::string& body, std::string& response);
    static void prepare_request(const std::string& url);

    void httpInit(void){}
    void httpDeInit(void)
    {
        http_.end();
    };

    // 5. HTTP GET (Device Description取得用)
    std::string httpGet(const std::string& url) {
        if (WiFi.status() != WL_CONNECTED) return "";

        prepare_request(url);
        int httpCode = http_.GET();

        std::string response;
        if (httpCode == HTTP_CODE_OK) {
            response = http_.getString().c_str();
        } else {
            // シリアル出力でエラーログ（Windows版のDBGLN相当）
            Serial.printf("HTTP GET failed, error: %s\n", http_.errorToString(httpCode).c_str());
            Serial.printf("url : %s\n", url.c_str());
        }
        // http_.end() は呼ばず、接続を維持する（Keep-Alive）
        return response;
    }

    // 6. HTTP POST (カメラコマンド実行用)
    bool httpPost(const std::string& url, const std::string& body, std::string& response) {
        return request_with_body(url, "POST", body, response);
    }

    // HTTP PUT (リソース更新用)
    bool httpPut(const std::string& url, const std::string& body, std::string& response) {
        return request_with_body(url, "PUT", body, response);
    }

    // HTTP DELETE (リソース削除用)
    bool httpDelete(const std::string& url, std::string& response) {
        if (WiFi.status() != WL_CONNECTED) return false;

        prepare_request(url);
        int httpCode = http_.sendRequest("DELETE");

        if (httpCode > 0) {
            response = http_.getString().c_str();
            // 200, 202, 204 を成功とする
            return (httpCode == 200 || httpCode == 202 || httpCode == 204);
        }
        return false;
    }

    // 共通のリクエスト準備
    void prepare_request(const std::string& url) 
    {
        // 接続を開始（既存の接続があれば再利用される）
        http_.begin(url.c_str());
        
        // ★重要：Keep-Aliveを有効にする
        http_.setReuse(true); 
        
        // タイムアウト設定（3秒の壁を意識して3000ms程度）
        http_.setConnectTimeout(1500);   // ②不達時に接続で長時間ブロックしない
        http_.setTimeout(3000);

        // JSON用のヘッダーを追加
        http_.addHeader("Content-Type", "application/json");
    }

    // POST/PUT 共通処理
    bool request_with_body(const std::string& url, const std::string& method, const std::string& body, std::string& response) 
    {
        if (WiFi.status() != WL_CONNECTED) return false;

        prepare_request(url);
        int httpCode = http_.sendRequest(method.c_str(), (uint8_t*)body.c_str(), body.length());

        bool success = false;
        if (httpCode > 0) {
            response = http_.getString().c_str();
            // 200～204 を成功とする
            if (httpCode >= 200 && httpCode <= 204) success = true;
        } else {
            Serial.printf("HTTP %s failed, error: %s\n", method.c_str(), http_.errorToString(httpCode).c_str());
        }

        return success;
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
        http.setConnectTimeout(1500);   // ②カメラ不達時にTCP接続(SYN)で数十秒ブロックしない=復帰を速く。健全な同一LANカメラは<100msで接続する。
        http.setTimeout(500);
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
        http.setConnectTimeout(1500);   // ②不達時に接続で長時間ブロックしない
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
        http.setConnectTimeout(1500);   // ②不達時に接続で長時間ブロックしない
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
        http.setConnectTimeout(1500);   // ②不達時に接続で長時間ブロックしない

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
