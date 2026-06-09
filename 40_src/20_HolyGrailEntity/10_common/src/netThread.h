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

    // SSDP関連
    void* ssdpStart(const std::string& query, const std::string& localIp = "");
    bool ssdpRead(void* handle, std::string& answer );
    void ssdpClose(void* handle);

    // HTTP関連
    void httpBreak(void);
    bool httpGet(const std::string& url, std::string& answer);
    bool httpPost(const std::string& url, const std::string& body, std::string& response);
    bool httpPut(const std::string& url, const std::string& body, std::string& response);
    bool httpDelete(const std::string& url, std::string& response);
};

#endif  // _NET_THREAD_H_