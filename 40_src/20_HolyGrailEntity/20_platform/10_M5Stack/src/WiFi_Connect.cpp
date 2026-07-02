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


