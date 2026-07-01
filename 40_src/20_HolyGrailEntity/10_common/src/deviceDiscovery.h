#ifndef _DEVICE_DISCOVERY_H_
#define _DEVICE_DISCOVERY_H_

#include "common.h"
#include "netThread.h"
#include "device.h"
#include "apiCanonCCAPI.h"

class deviceDiscovery
{

public:

	// カメラ API インターフェースの定義(サービス識別キーワード → 対応 api)。
	// 分類テーブルは受信バックエンド(detectCanonCCapi 等)が保持し、search に渡す。
	// deviceDiscovery 自身は低レベルの SSDP/USN ヘルパに徹する。
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

public:
	// SSDP(M-SEARCH)で探索する。ifaces に一致した service のデバイスを device に追加。
	//  ifaces : このバックエンドが対応するサービス定義(キーワード→apiClass)。
	static int search(std::vector<device> & device, const std::vector<definitionIntereface>& ifaces);

protected:
	static bool analizeUsn(class device& device, const std::string& usnLine);
};

#endif // _DEVICE_DISCOVERY_H_
