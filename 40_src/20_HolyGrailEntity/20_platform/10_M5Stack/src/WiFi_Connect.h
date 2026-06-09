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


};
