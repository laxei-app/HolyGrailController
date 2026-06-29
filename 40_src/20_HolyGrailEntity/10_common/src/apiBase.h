#ifndef _API_BASE_H_
#define _API_BASE_H_
#include "common.h"
#include "device.h"

class apiBase
{
public:
	// 撮影の設定可能な値

public:
	apiBase(void) {};
	virtual ~apiBase(void) {};
	virtual errCode init(class device& device) = 0;
	virtual errCode startShooting(void) { return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode rdyShutter(const cmdt::shotSet& shotSet) { return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode actShutter(void)						{ return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode getSettings(cmdt::shotRange& settings)	{ return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode rdyMetering(void)						{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode alzMetering(cmdt::HISTOGRAM& hist)		{ return ERR_HGC_NOT_SUPPORTED; };
	// 撮影開始時にカメラを当アプリ都合(マニュアル露出)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	virtual errCode setupShootingModeManual(void)			{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode restoreShootingMode(void)				{ return ERR_HGC_NOT_SUPPORTED; };
	// 接続維持用の無害なGET。撮影窓まで待機中などに定期送出し、無通信でカメラの
	// Wi-Fi/CCAPIセッションがタイムアウト切断するのを防ぐ。return ERR_HGC_OK で到達。
	virtual errCode keepAlive(void)							{ return ERR_HGC_NOT_SUPPORTED; };


};

#endif // _API_BASE_H_
