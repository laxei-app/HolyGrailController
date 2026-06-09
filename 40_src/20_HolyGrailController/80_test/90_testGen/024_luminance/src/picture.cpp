#include "common.h"
#include "debugOut.h"
#include "cameraController.h"
#include "testGen.h"

static int rdyMetering(int line, void* parm);
static int alzMetering(int line, void* parm);


int rdyMetering(int line, void* parm);
int alzMetering(int line, void* parm);

// 撮った画像の取得
int Metering(int line, void * parm)
{

	std::vector<tstgn::menuItem> menuItems;
	menuItems.push_back({ "ready   Metering", parm, rdyMetering });
	menuItems.push_back({ "analyze Metering", parm, alzMetering });

	return tstgn::menu(menuItems);

}

// 測光準備
int rdyMetering(int line, void* parm)
{
	const device& device = *(static_cast<class device*>(parm));
	cameraController::startShooting(device);
    tool::sleep(200);
	errCode err =  cameraController::rdyMetering(device);
    if(err == ERR_HGC_OK){ return 'a'; }
    DBGLN(col::YEL,"errCode(%u)", err);
    return 'a';
}

// 測光結果を取得
int alzMetering(int line, void* parm)
{
	const device& device = *(static_cast<class device*>(parm));
	cmdt::HISTOGRAM hist;
	errCode err = cameraController::alzMetering(device, hist);
    if(err != ERR_HGC_OK)
    {
        DBGLN(col::YEL,"errCode(%u)", err);
        return 'a'; 
    }
	std::vector<std::string> elemName = {"Luminance", "red","green","blue"};

	for (uint16_t elem = 0; elem < cmdt::hist_elem; elem++)
	{
		DBGLN(col::YEL, "%s", elemName[elem].c_str());
		for (uint16_t bin = 0; bin < cmdt::hist_bin; bin++)
		{
			if (((bin % 16) == 0)&&(bin)) { dbg::trace("\n"); }
			dbg::trace("%5u,", hist.raw[elem][bin]);
		}
        dbg::trace("\n");
	}
	return 'a';
}
