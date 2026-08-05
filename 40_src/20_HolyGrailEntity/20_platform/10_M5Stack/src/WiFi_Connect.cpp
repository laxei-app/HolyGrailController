#include "commonM5.h"
#include <WiFi.h>
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
bool wifiConnect::startAp(const char * ssid, const char * passphrase, int maxConn)
{
    WiFi.mode(WIFI_AP);
    // channel=1、SSID可視、maxConn(スマホ+カメラ2=3以上を確保)。pass 空文字ならオープン。
    bool ok = WiFi.softAP(ssid, (passphrase && passphrase[0]) ? passphrase : nullptr,
                          1 /*channel*/, 0 /*hidden*/, maxConn);
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


