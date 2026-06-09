#include "DeviceDiscovery.h"

/// ネットワークに接続されているカメラを検索する
/// device:検索したカメラの情報
/// target:検索するサービスの名称
/// 　　　サービスの名称は接続するインターフェースごとに決まっている。
/// 　　　今後拡張していき増えるかも
/// 　　　canon : ICPO-CameraControlAPIService
/// 　　　sony  : DigitalImaging
int DeviceDiscovery::search(vector<device>& device, vector<string> target)
{
    std::string query =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "ST: ssdp:all\r\n"
        "MX: 2\r\n\r\n";

    void* handle = net->ssdpStart(query, ip.c_str());

    std::string deviceInfo;
    do
    {
        deviceInfo = net->ssdpRead(handle);
        if (deviceInfo.length() == 0) { break; }
        printf("device ===================\r\n%s", deviceInfo.c_str());
    } while (deviceInfo.length() != 0);
    net->ssdpClose(handle);
    return 0;
}

