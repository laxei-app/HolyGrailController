#ifndef _NET_THREAD_H_
#define _NET_THREAD_H_
#include "common.h"
// net の機能をノンブロッキングで実現する。

namespace netThread
{
    typedef std::function<void(errCode)> CALL_BACK;

    void init(void);
    void deInit(void);

    // 利用可能なローカルIPアドレスのリストを返す
    std::vector<std::string> getLocalIpList();

    // APモード時、自SoftAP接続クライアントのIP一覧(SSDP不使用のカメラ発見用)。
    // 軽量なローカル問い合わせなので単一ワーカーキューは通さず直接呼ぶ。非AP/非対象は空。
    std::vector<std::string> apClientIps();

    // SSDP関連
    void* ssdpStart(const std::string& query, const std::string& localIp = "");
    bool ssdpRead(void* handle, std::string& answer );
    void ssdpClose(void* handle);

    // HTTP関連
    void httpBreak(void);
    bool httpGet(const std::string& url, std::string& answer);
    // 範囲指定GET(HTTP Range)。撮影画像のEXIFを読むのに先頭だけ取る。
    bool httpGetRange(const std::string& url, long from, long to, std::string& answer);
    bool httpPost(const std::string& url, const std::string& body, std::string& response);
    bool httpPut(const std::string& url, const std::string& body, std::string& response);
    bool httpDelete(const std::string& url, std::string& response);

    // 直近に失敗した HTTP POST/PUT の記録を返す(診断用)。
    //  status : 受け取った HTTP ステータス。0 = 応答なし(接続失敗/送信失敗)
    //  body   : 応答本文の先頭(最大120文字)。CCAPI はここに理由を返す(例: "During shooting or recording")
    // 失敗が「カメラが断った」のか「そもそも届かなかった」のかをログで区別するために使う。
    void lastHttpFailure(int& status, std::string& body);
};

#endif  // _NET_THREAD_H_