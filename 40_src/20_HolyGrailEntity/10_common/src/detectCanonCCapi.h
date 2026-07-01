#ifndef _DETECT_CANON_CCAPI_H_
#define _DETECT_CANON_CCAPI_H_

#include "detectSsdpBase.h"

// Canon CCAPI カメラの受信バックエンド。
// ・SSDP のサービス識別語(ICPO-CameraControlAPIService / schemas-canon-com)
// ・apiBase 生成 = apiCanonCCAPI
// ・手動接続URL規則 = http://<host>:8080/ccapi(initManual で記述子取得をスキップ)
class detectCanonCCapi : public detectSsdpBase
{
public:
	// IP 直指定で CCAPI カメラを構築する(http://<host>:8080/ccapi)。
	bool makeManualDevice(const std::string& host, class device& out) override;

protected:
	const std::vector<deviceDiscovery::definitionIntereface>& interfaces() const override;
	class apiBase* makeApi() override;	// new apiCanonCCAPI
};

#endif // _DETECT_CANON_CCAPI_H_
