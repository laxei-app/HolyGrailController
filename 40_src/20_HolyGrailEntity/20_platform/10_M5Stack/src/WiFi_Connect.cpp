#include "commonM5.h"
#include <WiFi.h>
#include <esp_wifi.h>	// esp_wifi_set_inactive_time(無通信で追い出すまでの時間)
#include <esp_netif.h>	// DHCP の配布範囲を読み戻して確かめる
#include "dhcpserver/dhcpserver.h"	// dhcps_lease_t
#include "WiFi_Connect.h"

// wifi の初期化をおこなう
void wifiConnect::setup()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
}

// wifi の接続状況を取得する
wifiConnect::wifiStatus wifiConnect::getStatus(void)
{
    if(WiFi.status() != WL_CONNECTED) { return wifiStatus::cuttingOff;}
    return wifiStatus::connect;
}

// wifi に接続する
bool wifiConnect::connect(const char * ssid, const char * passphrase)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, passphrase);

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        timeout++;
    }
    if(WiFi.status() != WL_CONNECTED) { return false;}

    // Wi-Fi の省電力を切る(2026-08-05 実験)。
    // ESP32 の既定は省電力ありで、無線はビーコン間隔ごとに寝る。しばらく通信が無いと
    // 深く寝るため、復帰にかかる時間がそのまま TCP の接続完了を遅らせる。
    //
    // 【この設定を疑う根拠】長時間カメラと通信していない状態からの初回だけ、露出設定が
    //  errno=119(EINPROGRESS=接続が1.5秒以内に完了しない)で落ちる。放置5回は全て失敗、
    //  連続実行4回は全て成功。同じ共通コード・同じ「1リクエスト1接続」で動くスマホは
    //  同じ1時間放置でも失敗しない(2026-08-05 対照実験)ため、カメラ側ではなくこちら側の要因。
    // 常時受信になるので消費電力は増えるが、エッジは基本的に給電前提で動かす。
    WiFi.setSleep(false);
    return true;
}

// --- APモード ---
// エッジ自身をアクセスポイントにする(純AP。上流には繋がない=AP+STA共存は行わない)。
// カメラ(EOS R10 をインフラ参加)とスマホがこのAPに参加し、CCAPI/ETP を同一サブネットで行う。
// SoftAP 既定サブネット=192.168.4.0/24、自局=192.168.4.1、内蔵DHCPがクライアントへ配布。
bool wifiConnect::startAp(const char * ssid, const char * passphrase, int maxConn,
                          uint32_t dhcpStart)
{
    WiFi.mode(WIFI_AP);
    // 【DHCPの配布範囲(2026-08-26)】呼び出し側が「前に配った先」を避けた開始位置をくれたら、
    //  SoftAP を立てる前に入れておく。softAPConfig の4番目が配布開始IPで、範囲は
    //  start〜start+10 の11個に固定される(NetworkInterface::config)。
    //  ここを既定のままにすると、再起動のたびに 192.168.4.2 から配り直して、
    //  電源が落ちていなかったカメラとIPがぶつかる。
    if (dhcpStart != 0)
    {
        const bool cok = WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                                           IPAddress(255, 255, 255, 0), IPAddress(dhcpStart));
        Serial.printf("[AP] dhcp pool start=%s -> %s\n",
                      IPAddress(dhcpStart).toString().c_str(), cok ? "ok" : "FAILED");
    }
    // channel=1、SSID可視、maxConn(スマホ+カメラ2=3以上を確保)。pass 空文字ならオープン。
    bool ok = WiFi.softAP(ssid, (passphrase && passphrase[0]) ? passphrase : nullptr,
                          1 /*channel*/, 0 /*hidden*/, maxConn);
    if (ok)
    {   // 無通信で追い出すまでの時間を伸ばす(既定300秒 -> 3600秒。2026-08-23)。
        //  ESP32 の SoftAP は接続中の端末から一定時間データが来ないと強制切断する。
        //  カメラ複数台では撮影していない台が無通信になり、実機で reason=4(ASSOC_EXPIRE) を確認した。
        //  主対策は在否監視の軽い接触。これはそれが滞ったときの保険。
        //  伸ばしすぎると居なくなった端末が接続一覧に残り接続枠(最大4)を占めるので1時間にする。
        //  この設定はフラッシュに保存されないので AP 起動のたびに呼ぶ。
        esp_wifi_set_inactive_time(WIFI_IF_AP, 3600);
        // 配布範囲は**読み戻して**残す(2026-08-26)。softAP() が設定を上書きしないか、
        //  推測ではなく実値で確かめられるようにする。IP重複の調査で効く。
        {
            esp_netif_t* ni = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            dhcps_lease_t ls{};
            if (ni != nullptr &&
                esp_netif_dhcps_option(ni, ESP_NETIF_OP_GET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                       &ls, sizeof(ls)) == ESP_OK)
            {
                Serial.printf("[AP] dhcp range: %s -- %s (enable=%d)\n",
                              IPAddress(ls.start_ip.addr).toString().c_str(),
                              IPAddress(ls.end_ip.addr).toString().c_str(), (int)ls.enable);
            }
        }
        // 接続枠の実効値を残す(2026-08-25)。softAP へは maxConn を渡しているが、
        //  フレームワークのビルド設定(CONFIG_WIFI_AP_MAX_STATIONS)で下げられることがある。
        //  パノラマ撮影で何台まで AP に入れるかはこの値が決めるので、推測せず実値を見る。
        {
            wifi_config_t cfg{};
            if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK)
            {
                Serial.printf("[AP] max_connection: requested=%d accepted=%u\n",
                              maxConn, (unsigned)cfg.ap.max_connection);
                // 【PMF(管理フレーム保護)は使わない(2026-08-25)】
                //  実機で AP がカメラを reason=209(SA_QUERY_TIMEOUT) で切っていた。
                //  PMF を張ろうとして応答が返らず AP 側だけが接続を落とす形で、
                //  **カメラ側は繋がっているつもりのまま**になる(ユーザー報告と一致)。
                //  キヤノン機は WPA2-PSK(PMF無し)で足りるので、こちらから明示的に切る。
                Serial.printf("[AP] pmf: capable=%d required=%d authmode=%d\n",
                              (int)cfg.ap.pmf_cfg.capable, (int)cfg.ap.pmf_cfg.required,
                              (int)cfg.ap.authmode);
                if (cfg.ap.pmf_cfg.capable || cfg.ap.pmf_cfg.required)
                {
                    cfg.ap.pmf_cfg.capable  = false;
                    cfg.ap.pmf_cfg.required = false;
                    const esp_err_t pe = esp_wifi_set_config(WIFI_IF_AP, &cfg);
                    Serial.printf("[AP] pmf disabled (err=%d)\n", (int)pe);
                }
            }
        }
    }
    return ok;
}

void wifiConnect::stop(void)
{
    WiFi.softAPdisconnect(true);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

bool wifiConnect::isApActive(void)
{
    return (WiFi.getMode() & WIFI_MODE_AP) != 0;
}

std::string wifiConnect::apIp(void)
{
    if (!isApActive()) { return ""; }
    return WiFi.softAPIP().toString().c_str();
}


