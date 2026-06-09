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
    WiFi.begin(ssid, passphrase); 

    int timeout = 0;
    while (WiFi.status() != WL_CONNECTED && timeout < 20) {
        delay(500);
        timeout++;
    }
    if(WiFi.status() != WL_CONNECTED) { return false;}
    return true;
}


