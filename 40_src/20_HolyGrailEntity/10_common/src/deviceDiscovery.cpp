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
    // MX:1 = カメラの応答遅延を0〜1秒に抑え、短い受信窓でも取りこぼしにくくする。
    std::string query =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "ST: ssdp:all\r\n"
        "MX: 1\r\n\r\n";

    // WiFiのマルチキャストは取りこぼしやすく、M-SEARCHを1回だけ送ると応答0台になりやすい(実測)。
    // socketを開き直して複数回送り、信頼性を確保する。これでも見つからなければ本当に居ない=NOCAMERA扱い
    // とし、旧「直近IP直結フォールバック」(別カメラへ誤接続する事故の原因)は holyGrailEntity 側で廃止した。
    const int      kAttempts = 3;      // M-SEARCH 送信回数
    const uint32_t kWindowMs = 1100;   // 1回あたりの応答待ち(MX:1なので1秒強で足りる)

    std::vector<std::string> ipList = netThread::getLocalIpList();

    // SSDP の応答の情報を集める
    for (auto& nicIp : ipList)
    {   // NIC 毎に探す
        DBGLN(col::RYEL, "my ip:%s",nicIp.c_str());
        for (int attempt = 0; attempt < kAttempts; ++attempt)
        {   // M-SEARCH を複数回送る(取りこぼし対策)
            void* handle = netThread::ssdpStart(query, nicIp.c_str());   // socket確保 + M-SEARCH送信(1回)

            std::string deviceInfo;
            auto startTime = tool::startElapse();
            auto itvl = startTime;
            while(tool::getElapse(startTime) < kWindowMs)
            {
                auto result = netThread::ssdpRead(handle, deviceInfo);
                // 【必ず眠ること(2026-08-28 実機でリセット)】ssdpRead は届いていなければ
                //  すぐ false で戻る(parsePacket が 0)。眠らずに回すと、この 1.1 秒が
                //  まるごと空回りになり、CPU を明け渡さないので IDLE タスクが走れず
                //  task_wdt でリセットする。カメラが1台も居ないとき(APモードで
                //  カメラが別のAPに居る等)は応答が皆無なので、必ずこの経路になる。
                //  M-SEARCH は MX:1 なので応答は1秒かけて散らばって届く。5ミリ秒刻みで
                //  十分に拾える(1窓あたり220回。空回りは数万回だった)。
                if (result == false)            { tool::sleep(5); continue; }   // 失敗
                if (deviceInfo.length() == 0)   { tool::sleep(5); continue; }   // 取得できていない
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

                // 対象のデバイスなので LOCATION を探す
                std::string location = tool::getKvpValue(deviceInfo, "location");
                if (location.length() == 0)                             { continue; } // location が見つからない。
                DBGLN(col::YEL, "location:%s", location.c_str());

                // usn の情報を格納する
                std::string usnLine = tool::getKvpValue(deviceInfo, "usn");
                if (!analizeUsn(deviceTmp, usnLine)) { continue; }       // usn を解析できない
                DBGLN(col::YEL, "UUID:%s", deviceTmp.uuid.c_str());
                deviceTmp.location = location;

                // 複数回送信で同一機体(uuid)が重複して返るため、既出uuidは弾く。
                bool dup = false;
                for (auto& ex : deviceList) { if (ex.uuid == deviceTmp.uuid) { dup = true; break; } }
                if (!dup) { deviceList.push_back(deviceTmp); }
            }
            netThread::ssdpClose(handle);
            DBGLN(col::GRN, "no detect time(%u)", tool::getElapse(itvl));
        }
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

