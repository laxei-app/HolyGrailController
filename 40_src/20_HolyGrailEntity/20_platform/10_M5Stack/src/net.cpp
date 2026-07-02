#include "commonM5.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <cstring>
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
    if (!ips.empty())
    {
        std::string joined; for (auto& s : ips) { joined += s + " "; }
        DBGLN(col::CYN, "apClientIps: %d client(s): %s", (int)ips.size(), joined.c_str());
    }
    return ips;
}

// SSDP探索開始 (特定のNICを指定してUDP送信)
void* ssdpStart(const std::string& query, const std::string& localIp) 
{
    WiFiUDP* udp = new WiFiUDP();

    // UDP開始
    udp->beginMulticast(IPAddress(239, 255, 255, 250), 1900);
    
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
        http.setTimeout(500);
        int code = http.GET();
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
        int code = http.POST(body.c_str());
        if (code > 0) response = http.getString().c_str();
        http.end();
        return (code == 200 || code == 204);
    }

    // HTTP PUT (M5Stack Core2用)
    bool httpPut(const std::string& url, const std::string& body, std::string& response)
    {
        HTTPClient http;
        // URLの開始
        http.begin(url.c_str());
        // Content-Typeを指定（APIの仕様に合わせて適宜変更してください）
        http.addHeader("Content-Type", "application/json");

        // PUTメソッドの実行
        int code = http.PUT(body.c_str());

        if (code > 0) {
            response = http.getString().c_str();
        }

        http.end();
        // 200 (OK), 201 (Created), 204 (No Content) を成功と判定
        return (code >= 200 && code <= 204);
    }

    // HTTP DELETE (M5Stack Core2用)
    bool httpDelete(const std::string& url, std::string& response)
    {
        HTTPClient http;
        http.begin(url.c_str());

        // DELETEメソッドの実行
        int code = http.sendRequest("DELETE");

        if (code > 0) {
            response = http.getString().c_str();
        }

        http.end();
        // 200 (OK) や 204 (No Content) を成功と判定
        return (code == 200 || code == 202 || code == 204);
    }
#endif // USE_KEEP_ALIVE
}
