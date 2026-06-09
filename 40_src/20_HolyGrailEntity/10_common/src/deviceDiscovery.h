#ifndef _DEVICE_DISCOVERY_H_
#define _DEVICE_DISCOVERY_H_

#include "common.h"
#include "netThread.h"
#include "device.h"
#include "apiCanonCCAPI.h"

class deviceDiscovery
{

protected:

	// カメラ API インターフェースの定義
	class definitionIntereface
	{
	public:
		std::vector<std::string>	keywords;	// サービスを特定するキーワード
		enum device::apiClass		apiClass;	// 対応する api
	public:
		definitionIntereface(std::vector<std::string> keywords, enum device::apiClass apiClass)
		{
			this->keywords = keywords;
			this->apiClass = apiClass;
		}
	};
	static inline std::vector<definitionIntereface> definitionIntereface =
	{
		// 型名(definitionIntereface)が同名変数に隠れるため初期化子では型名を書かず
		// 波括弧初期化でコンストラクタを呼ぶ(clang対応)。
		{ {"ICPO-CameraControlAPIService","schemas-canon-com"}, device::apiClass::CANON_CCAPI },
		{ {"DigitalImaging","schemas-sony-com"},                device::apiClass::CANON_CCAPI },	// まだ sony の interface は作っていないのでとりあえず canon 。
		// sony のサービス名が一般的すぎるので他の要素で and の確認ができるように改造した方が良い。
	};

public:
	static int search(std::vector<device> & device);

protected:
	static bool analizeUsn(class device& device, const std::string& usnLine);
};

#endif // _DEVICE_DISCOVERY_H_