#include "common.h"
#include "commonM5.h"
#include "WiFi_Connect.h"
#include "deviceDiscovery.h"
#include "device.h"
#include "net.h"
#include "debugOut.h"


// put function declarations here:
int test(void);
void setup() 
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setTextSize(2);
    dbg::init();

    wifiConnect::setup();
    DBGLN(col::YEL, "C++ Version Macro: %u", __cplusplus);

    // 199711L	C++98 / C++03
    // 201103L	C++11
    // 201402L	C++14
    // 201703L	C++17
    // 202002L	C++20
}

void loop() 
{

    if(wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
    {   // wifi 切れたら接続しなおす
        cons::clr();
        cons::printf("Try wifi connecting.\n");
        if(!wifiConnect::connect("Buffalo-G-D850","rnhcftfbk75tf"))
        {
            cons::printf("wifi could not connected.\n");
            delay(1000);
            return;
        }
        cons::printf("wifi conneced.");
        delay(1000);

    }

    if (cons::kbhit()) 
    {   // ボタン押されたらカメラを検索
        cons::clr();
        test();
    }

}

