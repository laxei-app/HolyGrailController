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


};

#endif // _API_BASE_H_
