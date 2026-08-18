// search.cpp : このファイルには 'main' 関数が含まれています。プログラム実行の開始と終了がそこで行われます。
//

#include <iostream>
#include "deviceDiscovery.h"
#include "net.h"
#include "device.h"

int main()
{
	class net * net = new class net();

	deviceDiscovery discovery(net);				// DeviceDiscovery クラスの生成
	vector<device> devices;
	vector<string> target;
	target.push_back("ICPO-CameraControlAPIService");           // canon
	target.push_back("DigitalImaging");                         // sony
	int num = discovery.search(devices, target);
	if (num == 0)
	{
		cout << "no detect device.";
		return 0;
	}
	cout << "device num (" << devices.size() << std::endl;
	for (auto & device: devices)
	{
		cout << "========================\n" << std::endl;
		cout << "dev:" <<  device.devLocation.c_str() << std::endl;
		cout << "ser:" <<  device.serLocation.c_str() << std::endl;
		cout << "uui:" <<  device.uuid.c_str() << std::endl;
		cout << "mod:" <<  device.model.c_str() << std::endl;
		cout << "fri:" <<  device.assignedName.c_str() << std::endl;
		cout << "man:" <<  device.manufacturer.c_str() << std::endl;
		cout << "ser:" <<  device.serialno.c_str() << std::endl;
		cout << "bas:" <<  device.urlbase.c_str() << std::endl;
		cout << "Acc:" <<  device.urlAccess.c_str() << std::endl;
	}
	return 0;
}

