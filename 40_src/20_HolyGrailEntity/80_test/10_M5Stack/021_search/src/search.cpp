#include "common.h"
#include "commonM5.h"
#include "WiFi_Connect.h"
#include "deviceDiscovery.h"
#include "device.h"
#include "net.h"
#include "debugOut.h"


static class net net;
// put function declarations here:
int deviceDiscoveryTest(void);
void setup() 
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setTextSize(2);
    dbg::init();

    wifiConnect::setup();

}

void loop() 
{
    M5.update(); // ボタン状態の更新（Core2のタッチもこれで判定）

    if(wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
    {   // wifi 切れたら接続しなおす
        M5.Display.clear();
        M5.Display.setCursor(0, 0);
        M5.Display.printf("Try wifi connecting.");
        if(!wifiConnect::connect("Buffalo-G-D850","rnhcftfbk75tf"))
        {
            M5.Display.setCursor(1, 0);
            M5.Display.printf("wifi could not connected.");
            delay(1000);
            return;
        }
        M5.Display.clear();
        M5.Display.setCursor(0, 0);
        M5.Display.printf("wifi conneced.");
        delay(1000);

    }

    if (M5.BtnA.wasPressed()) 
    {   // ボタン押されたらカメラを検索
        M5.Display.clear();
        M5.Display.setCursor(0, 0);
        deviceDiscoveryTest();
    }

}

// put function definitions here:
int deviceDiscoveryTest(void) 
{
	deviceDiscovery discovery(net);				// DeviceDiscovery クラスの生成
	vector<device> devices;
	vector<string> target;
	target.push_back("ICPO-CameraControlAPIService");           // canon
	target.push_back("DigitalImaging");                         // sony

	int num = discovery.search(devices, target);
	if (num == 0)
	{
		M5.Display.printf("no detect device.");
		return 0;
	}
	printf("device num (%d)\n",devices.size());
	for (auto & device: devices)
	{
		M5.Display.printf("========================\n");
		M5.Display.printf("dev:%s\n", device.devLocation.c_str());
		M5.Display.printf("ser:%s\n", device.serLocation.c_str());
		M5.Display.printf("uui:%s\n", device.uuid.c_str());
		M5.Display.printf("mod:%s\n", device.model.c_str());
		M5.Display.printf("fri:%s\n", device.assignedName.c_str());
		M5.Display.printf("man:%s\n", device.manufacturer.c_str());
		M5.Display.printf("ser:%s\n", device.serialno.c_str());
		M5.Display.printf("bas:%s\n", device.urlbase.c_str());
		M5.Display.printf("Acc:%s\n",device.urlAccess.c_str());

	}
	return 0;
}