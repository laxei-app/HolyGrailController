#ifndef _NET_H_
#define _NET_H_

#include <string>
#include <vector>

namespace net 
{

    bool init();
    bool deInit();

    // --- 追加: 利用可能なローカルIPアドレスのリストを返す ---
    std::vector<std::string> getLocalIpList();

    // SSDP関連(能動: M-SEARCH送信→応答受信)
    void* ssdpStart(const std::string& query, const std::string& localIp = "");
    bool ssdpRead(void* handle, std::string& answer);
    void ssdpClose(void* handle);

    // SSDP受動待ち受け(NOTIFY受信)。0.0.0.0:1900 に bind し 239.255.255.250 へ参加する。
    // M-SEARCH は送らず、カメラが自発広告する NOTIFY を受ける(スマホ接続後などにSSDP広告を
    // 止めるカメラの復帰は拾えないが、電源復帰・再起動時の再出現を60秒待たず即検知するため)。
    // 専用ソケット+専用スレッドから使い、単一 netThread ワーカーは迂回する。
    //  ※Android は Java 側 WifiManager.MulticastLock 保持 + 権限 CHANGE_WIFI_MULTICAST_STATE が必要
    //    (無いと Wi-Fi ドライバがマルチキャスト受信を黙って破棄する)。Windows はスタブ(非対象)。
    void* ssdpListenStart(void);
    bool  ssdpListenRead(void* handle, std::string& answer);
    void  ssdpListenClose(void* handle);

    // HTTP関連
    void httpBreak(void);
    bool httpGet(const std::string& url, std::string& answer);
    bool httpPost(const std::string& url, const std::string& body, std::string& response);
    bool httpPut(const std::string& url, const std::string& body, std::string& response);
    bool httpDelete(const std::string& url, std::string& response);

 
};

#endif  // _NET_H_