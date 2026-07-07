#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wininet.h>
#include <iostream>
#include "commonWin.h"
#include "common.h"
#include "net.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "wininet.lib")

namespace net
{

static void httpInit();
static void httpDeInit();

// 初期化
// return : true:成功、false:失敗
bool init()
{
    WSADATA wsaData;
    auto result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0)
    {
        DBGLN(col::BYEL, "WSAStartup fail(0x%08x)", result);
        return false;
    }
    httpInit();
    return true;
}

// 終了
// return : true:成功、false:失敗
bool deInit()
{
    if (WSACleanup() != 0)
    {
        DBGLN(col::BYEL, "WSACleanup fail(0x%08x)", (int)WSAGetLastError());
        return false;
    }
    httpDeInit();
    return true;
}

// 利用可能な全NICのIPアドレスを取得
// 限定サブネットのバッチ探索はエッジ役(§3.3 tier3)専用。Windows(検証用・非対象)はスタブ(空)。
std::vector<std::string> scanSubnetPort(int /*port*/, int /*timeoutMs*/, int /*maxHosts*/)
{
    return std::vector<std::string>();
}

std::vector<std::string> getLocalIpList()
{
    std::vector<std::string> ips;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
            // 動作中かつループバック以外のアダプタ
            if (pCurr->OperStatus == IfOperStatusUp && pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast != NULL; pUnicast = pUnicast->Next) {
                    sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                    char buf[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &(sa_in->sin_addr), buf, sizeof(buf));
                    ips.push_back(std::string(buf));
                }
            }
        }
    }
    free(pAddresses);
    return ips;
}

// SSDP探索開始 (特定のNICを指定してUDP送信)
void* ssdpStart(const std::string& query, const std::string& localIp) 
{
    SOCKET* sockPtr = new SOCKET(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (*sockPtr == INVALID_SOCKET) return nullptr;

    // タイムアウト設定 (1秒)
    int timeout = 1000;
    setsockopt(*sockPtr, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    // 特定のNICにバインド
    sockaddr_in localAddr = {};
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(0); // 空きポート
    inet_pton(AF_INET, localIp.c_str(), &localAddr.sin_addr);

    if (bind(*sockPtr, (sockaddr*)&localAddr, sizeof(localAddr)) == SOCKET_ERROR) {
        closesocket(*sockPtr);
        delete sockPtr;
        return nullptr;
    }

    // SSDPマルチキャストアドレスへ送信
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);

    sendto(*sockPtr, query.c_str(), (int)query.length(), 0, (sockaddr*)&dest, sizeof(dest));
    return (void*)sockPtr;
}

bool ssdpRead(void* handle, std::string& answer) 
{
    if (!handle) return "";
    SOCKET sock = *(SOCKET*)handle;
    answer.resize(1024*4);
    int r = recvfrom(sock, answer.data(), static_cast<int>(answer.size() - 1), 0, NULL, NULL);
    if (r > 0) {
        answer[r] = '\0';
        return true;
    }
    return false;
}

// 4. 探索終了
void ssdpClose(void* handle)
{
    if (handle) {
        SOCKET* sockPtr = (SOCKET*)handle;
        closesocket(*sockPtr);
        delete sockPtr;
    }
}

// SSDP受動待ち受け: Windows は非対象(検証用)のためスタブ。60秒ごとの能動再探索のみで動く。
void* ssdpListenStart(void) { return nullptr; }
bool  ssdpListenRead(void* handle, std::string& answer) { (void)handle; (void)answer; return false; }
void  ssdpListenClose(void* handle) { (void)handle; }

// http通信の中断要求フラグ
std::atomic<bool> httpBreakRequest = false;

// 中断の要求。http通信を中断させる
void httpBreak(void)
{
    httpBreakRequest = true;
}


#define     USE_KEEP_ALIVE
#if defined(USE_KEEP_ALIVE)
//////////////////////////////////////////////////////////////////////////////////
// keep-alive を有効にした http 通信。 libcurl を使用。
#include <curl/curl.h>

static void reset_handle(const std::string& url);
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
static bool request_with_body(const std::string& url, const std::string& method, const std::string& body, std::string& response);
static int breakCheck(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

static CURL* curl_ = nullptr;

// http の開始
void httpInit()
{
    // libcurlのグローバル初期化（アプリ起動時に1回）
    curl_global_init(CURL_GLOBAL_ALL);
    curl_ = curl_easy_init();

    if (curl_) {
        // Keep-Aliveを有効化
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 120L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 60L);

        // タイムアウト設定（3秒の制約に合わせて厳しめに設定可能）
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 2L);
    }
}

// http の終了
void httpDeInit()
{
    if (curl_) curl_easy_cleanup(curl_);
    curl_global_cleanup();
}

// 5. HTTP GET (Device Description取得用)
bool httpGet(const std::string& url, std::string& response) 
{
    if (!curl_) return false;
    response.clear();

    reset_handle(url);
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);    // progress 表示を有効にする
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, breakCheck);// コールバック関数を登録

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        DBGLN(col::CYN, "curl_easy_perform(%d)\n", res);
        DBGLN(col::CYN, "url : %s\n", url.c_str());
    }

    return true;
}

// 6. HTTP POST (カメラコマンド実行用)
bool httpPost(const std::string& url, const std::string& body, std::string& response) 
{
    return request_with_body(url, "POST", body, response);
}

// HTTP PUT (リソース更新用)
bool httpPut(const std::string& url, const std::string& body, std::string& response) 
{
    return request_with_body(url, "PUT", body, response);
}

// HTTP DELETE (リソース削除用)
bool httpDelete(const std::string& url, std::string& response) 
{
    if (!curl_) return false;
    response.clear();

    reset_handle(url);
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);    // progress 表示を有効にする
    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, breakCheck);// コールバック関数を登録

    CURLcode res = curl_easy_perform(curl_);
    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        // WinInet版の判定に準拠 (200, 202, 204)
        return (code == 200 || code == 202 || code == 204);
    }
    return false;
}

// ハンドルの共通初期化
void reset_handle(const std::string& url) 
{
    curl_easy_reset(curl_); // 前回の設定をクリア
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_callback);
    // Keep-Alive設定はresetで消えるため再設定
    curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
}

// 受信コールバック
size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) 
{
    size_t total = size * nmemb;
    static_cast<std::string*>(userp)->append(static_cast<char*>(contents), total);
    return total;
}

// POST/PUT 共通処理
bool request_with_body(const std::string& url, const std::string& method, const std::string& body, std::string& response) 
{
    if (!curl_) return false;
    response.clear();

    reset_handle(url);
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)body.length());
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    // ヘッダー設定 (WinInet版と同じ application/json)
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
//    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);    // progress 表示を有効にする
//    curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, breakCheck);// コールバック関数を登録

    CURLcode res = curl_easy_perform(curl_);
    bool success = false;

    if (res == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &code);
        // WinInet版の判定に準拠 (200～204)
        if (code >= 200 && code <= 204) success = true;
    }
    else {
        DBGLN(col::CYN, "curl_easy_perform(%d) [%s]\n", res, method.c_str());
    }

    curl_slist_free_all(headers);
    return success;
}

// 中断のチェックをする
// clientp  : ユーザー定義ポインタ
// dltotal  : ダウンロード予定の全サイズ
// dlnow    : 現在ダウンロード済みのサイズ
// ultotal  : アップロード予定の全サイズ
// ulnow    : 現在アップロード済みのサイズ
// return   : 0:継続、1:中断
static int breakCheck(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    if (httpBreakRequest.load())
    {
        httpBreakRequest.store(false); // フラグをリセット
        return 1;       // 中断要求がある場合は1を返して通信を中断
    }
    return 0;           // 通信継続
}


#else
/////////////////////////////////////////////////////////////////////////////////////////
// windows 標準機能を使った http 通信。

void httpInit(){}
void httpDeInit() {}

// 5. HTTP GET (Device Description取得用)
bool httpGet(const std::string& url, std::string& response)  
{
    response.resize(0);
    HINTERNET hInternet = ::InternetOpenA("HolyGrail", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hInternet) { return false; }
    HINTERNET hUrl = ::InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
    if (!hUrl)
    {
        DBGLN(col::CYN, "InternetOpenUrlA(%u)\n", GetLastError());
        DBGLN(col::CYN, "nurl : %s\n", url);
    }
    else
    {
        char buf[4096];
        DWORD read;
        while (::InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0) 
        {
            response.append(buf, read);
        }
        ::InternetCloseHandle(hUrl);
    }
    ::InternetCloseHandle(hInternet);
    return true;
}

// 6. HTTP POST (カメラコマンド実行用)
bool httpPost(const std::string& url, const std::string& body, std::string& response) 
{
    URL_COMPONENTSA urlComp = { sizeof(URL_COMPONENTSA) };
    char hostName[256], urlPath[1024];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = sizeof(hostName);
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = sizeof(urlPath);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComp)) return false;

    HINTERNET hInternet = InternetOpenA("HolyGrail", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    HINTERNET hConnect = InternetConnectA(hInternet, hostName, urlComp.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    bool success = false;

    if (hConnect) {
        const char* types[] = { "application/json", "*/*", NULL };
        HINTERNET hReq = HttpOpenRequestA(hConnect, "POST", urlPath, NULL, NULL, types, INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (hReq) {
            std::string head = "Content-Type: application/json\r\n";
            if (HttpSendRequestA(hReq, head.c_str(), (DWORD)head.length(), (LPVOID)body.c_str(), (DWORD)body.length())) {
                DWORD code = 0, sz = sizeof(code);
                HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz, NULL);
                if (code == 200 || code == 204) success = true;

                char buf[1024]; DWORD read;
                while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0) response.append(buf, read);
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return success;
}

// HTTP PUT (リソース更新用)
bool httpPut(const std::string& url, const std::string& body, std::string& response)
{
    URL_COMPONENTSA urlComp = { sizeof(URL_COMPONENTSA) };
    char hostName[256], urlPath[1024];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = sizeof(hostName);
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = sizeof(urlPath);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComp)) return false;

    HINTERNET hInternet = InternetOpenA("HolyGrail", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    HINTERNET hConnect = InternetConnectA(hInternet, hostName, urlComp.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    bool success = false;

    if (hConnect) {
        const char* types[] = { "application/json", "*/*", NULL };
        // メソッドを "PUT" に指定
        HINTERNET hReq = HttpOpenRequestA(hConnect, "PUT", urlPath, NULL, NULL, types, INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (hReq) {
            std::string head = "Content-Type: application/json\r\n";
            if (HttpSendRequestA(hReq, head.c_str(), (DWORD)head.length(), (LPVOID)body.c_str(), (DWORD)body.length())) {
                DWORD code = 0, sz = sizeof(code);
                HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz, NULL);
                // 200 (OK), 201 (Created), 204 (No Content) を成功とみなす
                if (code >= 200 && code <= 204) success = true;

                char buf[1024]; DWORD read;
                while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0) response.append(buf, read);
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return success;
}

// HTTP DELETE (リソース削除用)
bool httpDelete(const std::string& url, std::string& response)
{
    URL_COMPONENTSA urlComp = { sizeof(URL_COMPONENTSA) };
    char hostName[256], urlPath[1024];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = sizeof(hostName);
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = sizeof(urlPath);

    if (!InternetCrackUrlA(url.c_str(), 0, 0, &urlComp)) return false;

    HINTERNET hInternet = InternetOpenA("HolyGrail", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    HINTERNET hConnect = InternetConnectA(hInternet, hostName, urlComp.nPort, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    bool success = false;

    if (hConnect) {
        // メソッドを "DELETE" に指定
        HINTERNET hReq = HttpOpenRequestA(hConnect, "DELETE", urlPath, NULL, NULL, NULL, INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (hReq) {
            // DELETEは通常ボディを送らないため、第3・4引数はNULL/0
            if (HttpSendRequestA(hReq, NULL, 0, NULL, 0)) {
                DWORD code = 0, sz = sizeof(code);
                HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &sz, NULL);
                // 200 (OK) や 204 (No Content) を成功とする
                if (code == 200 || code == 202 || code == 204) success = true;

                char buf[1024]; DWORD read;
                while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0) response.append(buf, read);
            }
            InternetCloseHandle(hReq);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return success;
}
#endif // USE_KEEP_ALIVE
}