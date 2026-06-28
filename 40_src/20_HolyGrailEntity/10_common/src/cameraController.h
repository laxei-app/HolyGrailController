#ifndef _CAMERA_CONTROLLER_H_
#define _CAMERA_CONTROLLER_H_

#include "common.h"
#include "deviceDiscovery.h"
#include "apiCanonCCAPI.h"
#include "device.h"

class cameraController
{
public:
public:
	// カメラの検出
	static size_t detectTarget(std::vector<class device>& device);

	// IP直指定でカメラに接続する(SSDPを使わない。エミュレータ等での手動接続用)。
	//  host : カメラのIPアドレス(例 "192.168.1.4")。CCAPIは http://<host>:8080/ccapi を使用。
	//  return : 接続できたデバイス数(0 or 1)
	static size_t connectManual(std::vector<class device>& device, const std::string& host);

	// カメラへのアクセス
	static errCode startShooting(const class device& device);
	static errCode rdyShutter(const class device& device, const cmdt::shotSet& shotSet);
	static errCode actShutter(const class device& device);
	static errCode getSettings(const class device& device, cmdt::shotRange& settings);
	static errCode rdyMetering(const class device& device);
	static errCode alzMetering(const class device& device, cmdt::HISTOGRAM& hist);
	// 撮影開始時にカメラをM(ダイアル無視)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	static errCode setupShootingModeManual(const class device& device);
	static errCode restoreShootingMode(const class device& device);

protected:
};

#endif // _CAMERA_CONTROLLER_H_
