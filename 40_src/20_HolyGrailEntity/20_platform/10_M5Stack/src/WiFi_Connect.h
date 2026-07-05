#include <string>

class wifiConnect
{
public:
    enum class wifiStatus
    {
        cuttingOff,
        connect
    };

public:
    static void setup(void);
    static wifiStatus getStatus(void);
    static bool connect(const char * ssid, const char * passphrase);

    // --- APモード(エッジ自身がアクセスポイント。屋外・ルーター無しでカメラ/スマホを収容) ---
    // SoftAP を起動する。channel=1、maxConn=同時接続上限(ESP32 SoftAPの最大は10)。成功でtrue。
    // スマホ+カメラ複数+2台目エッジ+再接続churn を収容するため呼び出し側は最大(10)を渡す。
    static bool startAp(const char * ssid, const char * passphrase, int maxConn = 10);
    // WiFi を停止(AP/STAとも切断)する。モード切替時に使う。
    static void stop(void);
    // 現在APが起動中か。
    static bool isApActive(void);
    // AP の自局IP文字列(通常 "192.168.4.1")。未起動時は空。
    static std::string apIp(void);
};
