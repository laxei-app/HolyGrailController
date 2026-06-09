#include "common.h"
#include "cameraController.h"
#include "captureRunner.h"
#include <algorithm>

// 撮影準備
// scedule  : 撮影スケジュール
// device   : 対象のデバイス
// return  : ERR_HGC_OK:成功
errCode captureRunner::ready(std::vector<cmdt::fullcrum>& schedule, device& device)
{
	return ERR_HGC_OK;
}

// 撮影開始
// return  : ERR_HGC_OK:成功
errCode captureRunner::start(void)
{
	return ERR_HGC_OK;
}

// 撮影終了
// return  : ERR_HGC_OK:成功
errCode captureRunner::end(void)
{
	return ERR_HGC_OK;
}
