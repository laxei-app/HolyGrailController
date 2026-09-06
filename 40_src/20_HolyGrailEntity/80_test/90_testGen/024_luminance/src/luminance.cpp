#include "common.h"
#include "debugOut.h"
#include "cameraController.h"
#include <cmath>
#include "testGen.h"
#include "netThread.h"

int shutterExecute(int line, void* parm);
int getCanSetting(int lin, void* param);
int Metering(int line, void* parm);

int test(void) 
{
	int key;

    DBGLN(col::CYN,"netThread::init().");
	netThread::init();
	do
	{
		cons::clr();				// クリア
		cons::printf("enter any key to start detect. => ");
		key = cons::getch();
		if (key == 'c')					{ break; }
		cons::printf("%c\n", key);
		cons::printf("now being search.\n");

		// ネットワークに接続されているカメラを検出する
		std::vector<device> devices;
		auto num = cameraController::detectTarget(devices);
		if (num == 0)
		{	// 1つも見つからなかった
			cons::printf("no detect device.\n");
			key = cons::getch();
			continue;
		}

		// 検出したカメラを表示する
		cons::printf("detect %u device(s).\n", (int)devices.size());
		cons::clr();				// クリア
		int row0, col0;
		cons::getRowCol(row0, col0);
		for (auto const& device : devices)
		{
			cons::setRowCol(row0, col0);
			cons::printf("%s\ns/n:%s\n", device.model.c_str(), device.serialno.c_str());
			int row1, col1;
			cons::getRowCol(row1, col1);
			std::vector<tstgn::menuItem> menu;
			menu.push_back({ "act shutter     ", (void*)&device, shutterExecute });
			menu.push_back({ "detect inf      ", (void*)&device, getCanSetting });
			menu.push_back({ "Metering        ", (void*)&device, Metering });
			while (key != 'c')
			{
				cons::setRowCol(row1, col1);
				key = tstgn::menu(menu);
				if (key == 'c') { break; }
			}
			tool::memoryInfo();
		}

	} while (key != 'c');

	netThread::deInit();
	return 0;
}


// シャッターを切るテストの実体
int shutterExecute(int line, void* parm)
{
	(void)line;

	std::vector<cmdt::shotSet> shotSetting;
	//                       ss             F     ISO
	shotSetting.push_back({ 1.0f / 1000.0f, 1.4f, 1600.0f });
	shotSetting.push_back({ 30.0f,         16.0f, 100.0f });
	shotSetting.push_back({ 10.0f,          5.6f, 200.0f });
	shotSetting.push_back({ 0.5f,           8.0f, 400.0f });
	int select = 0;

	// 実行関数
	device* device = static_cast<class device*>(parm);
	struct param_t
	{
		const class device& device;
		std::vector<cmdt::shotSet>& shotSetting;

	} param = { *device, shotSetting };

	auto shot = [](int lin, void* param)->int
	{
		auto prm = static_cast<struct param_t*>(param);
		return cameraController::actShutter(prm->device);
	};

	// 設定関数
	auto sel = [](int lin, void* param)->int
	{
		auto prm = static_cast<struct param_t*>(param);
		return cameraController::rdyShutter(prm->device, prm->shotSetting[lin]);
	};

	std::vector<tstgn::menuItem> menuItems;
	for (auto& setting : shotSetting)
	{
		std::string buff;
		buff.resize(32);
		snprintf(buff.data(), buff.size(),
			"ss:%-6s F%-4.1f iso%u", std::to_string(setting.ss).c_str(),
			setting.fNum,
			static_cast<uint32_t>(setting.iso));

		menuItems.push_back({ buff, &param, shot, sel });
	}

	return tstgn::menu(menuItems);
}

// 設定可能値を取得する
int getCanSetting(int lin, void* param)
{
	device& device = *static_cast<class device*>(param);

	// 設定値を取得する
	cmdt::shotRange settings;
	auto err = cameraController::getSettings(device, settings);
	if (err != ERR_HGC_OK)
	{
		DBGLN(col::RYEL, "getSettings(%u)", err);
		tool::sleep(200);
		return 'a';
	}

	// 表示
	DBGLN(col::CYN, "can set fnum(%u)", settings.fNum.size());
	for (const auto& fnum : settings.fNum)
	{
		auto num = static_cast<uint32_t>(std::trunc(fnum));
		auto frac = static_cast<uint32_t>(std::round(std::fmod(fnum,1.0f) * 10.0));
		DBGLN(col::CYN, "f/%u.%u,", num, frac);
	}
	DBGLN(col::CYN, " ");

	DBGLN(col::CYN, "can set ss(%u)", settings.ss.size());
	for (const auto& ss : settings.ss)
	{
		if (ss > 0.3) { DBGLN(col::CYN, "%5.2f,", ss); }
		else
		{
			DBGLN(col::CYN, "1/%u,", (uint32_t)(std::round(1.0 / ss)));
		}

	}
	DBGLN(col::CYN, " ");

	DBGLN(col::CYN, "can set ISO(%u)", settings.iso.size());
	for (const auto& iso : settings.iso)
	{
		DBGLN(col::CYN, "%u,", static_cast<uint32_t>(std::round(iso)));
	}
	DBGLN(col::CYN, " ");

	return 'a';
}

