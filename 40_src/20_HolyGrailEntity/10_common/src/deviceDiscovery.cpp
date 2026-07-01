#include "deviceDiscovery.h"
#include "tool.h"
#include "debugOut.h"


// ネットワークに接続されているカメラを検索する
// deviceList:検索したカメラの情報
// ifaces    :検索するサービスの定義(キーワード→apiClass)。呼び出し側(受信バックエンド)が保持。
// 　　　サービスの名称は接続するインターフェースごとに決まっている。
// 　　　canon : ICPO-CameraControlAPIService
// 　　　sony  : DigitalImaging
int deviceDiscovery::search(std::vector<device>& deviceList, const std::vector<definitionIntereface>& ifaces)
{
    std::string query =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "ST: ssdp:all\r\n"
        "MX: 2\r\n\r\n";


    std::vector<std::string> ipList = netThread::getLocalIpList();

    // SSDP の最初の応答の情報を集める
    for (auto& nicIp : ipList)
    {   // NIC 毎に探す
        DBGLN(col::RYEL, "my ip:%s",nicIp.c_str());
        void* handle = netThread::ssdpStart(query, nicIp.c_str());

        std::string deviceInfo;
        auto startTime = tool::startElapse();
        auto itvl = startTime;
        while(tool::getElapse(startTime) < 3000)
        {
            auto result = netThread::ssdpRead(handle, deviceInfo);
            if (result == false)            { continue; }   // 失敗
            if (deviceInfo.length() == 0)   {continue; }    // 取得できていない
            DBGLN(col::GRN, "netThread::ssdpRead(%4ums,%u))",tool::getElapse(itvl), deviceInfo.length());
            itvl = tool::startElapse();
            class device deviceTmp;
            for (const auto& interface : ifaces)
            {   // 対象の service を探す
                if(tool::findKvp(deviceInfo, interface.keywords))
                {   // 対象のserviceが見つかった
                    deviceTmp.apiClass = interface.apiClass;
                    break;
                }
            }
            if (deviceTmp.apiClass == device::apiClass::NON)                   { continue; } // 対象ではない

            // ★ service のバージョンも取得しておいた方がいいのか？
            // 対象のデバイスなので LOCATION を探す
            std::string location = tool::getKvpValue(deviceInfo, "location");
            if (location.length() == 0)                             { continue; } // location が見つからない。
            DBGLN(col::YEL, "location:%s", location.c_str());

            // usn の情報を格納する
            std::string usnLine = tool::getKvpValue(deviceInfo, "usn");
            if (!analizeUsn(deviceTmp, usnLine)) { continue; }       // usn を解析できない
            DBGLN(col::YEL, "UUID:%s", deviceTmp.uuid.c_str());
            deviceTmp.location = location;

            deviceList.push_back(deviceTmp);
        } 
        netThread::ssdpClose(handle);
        DBGLN(col::GRN, "no detect time(%u)", tool::getElapse(itvl));
    }
    return (int)deviceList.size();
}

// usn の内容を取得する。取得する内容は以下。
// ・UUID    : "00000000-0000-0000-0001-F8A26DB2EE0D"
// ・usn     : "schemas-canon-com"
// ・service :"ICPO-CameraControlAPIService"
// return : true 成功、false 失敗
bool deviceDiscovery::analizeUsn(class device& device, const std::string & usnLine)
{
    std::string uuid = tool::getKvpValueColon(usnLine, "uuid");
    if (uuid.length() == 0)               { return false; }   // uuid が無い

    std:: string urn = tool::getKvpValueColon(usnLine, "urn");
    if (urn.length() == 0)                { return false; }   // uuid が無い

    std::string service = tool::getKvpValueColon(usnLine, "device");
    if (service.length() == 0)
    {
        service = tool::getKvpValueColon(usnLine, "service");
    }
    if (service.length() == 0)              { return false; }   // service が無い

    // すべてそろったので反映する
    device.uuid = uuid;
    device.urn = urn;
    device.service = service;

    return true;
}

