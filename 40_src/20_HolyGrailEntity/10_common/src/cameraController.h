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

	// カメラへのアクセス
	static errCode startShooting(const class device& device);
	static errCode rdyShutter(const class device& device, const cmdt::shotSet& shotSet);
	static errCode actShutter(const class device& device);
	static errCode getSettings(const class device& device, cmdt::shotRange& settings);
	static errCode rdyMetering(const class device& device);
	static errCode alzMetering(const class device& device, cmdt::HISTOGRAM& hist);

protected:
};

#endif // _CAMERA_CONTROLLER_H_
