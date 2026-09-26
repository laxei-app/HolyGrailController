#ifndef _DEVICE_DISCOVERY_H_
#define _DEVICE_DISCOVERY_H_

#include "common.h"
#include "netThread.h"
#include "device.h"

class deviceDiscovery
{

public:

	// このバックエンドが自分のカメラと見なす SSDP のサービス識別語。
	// 分類テーブルは受信バックエンド(detectCanonCCapi 等)が保持し、search に渡す。
	// deviceDiscovery 自身は低レベルの SSDP/USN ヘルパに徹し、API の種類を知らない(2026-09-06)。
	class definitionIntereface
	{
	public:
		std::vector<std::string>	keywords;	// サービスを特定するキーワード
	public:
		explicit definitionIntereface(std::vector<std::string> keywords) : keywords(std::move(keywords)) {}
	};

public:
	// SSDP(M-SEARCH)で探索する。ifaces に一致した service のデバイスを device に追加。
	//  ifaces : このバックエンドが対応するサービス識別語。合ったものだけを device に足す。
	static int search(std::vector<device> & device, const std::vector<definitionIntereface>& ifaces);

protected:
	static bool analizeUsn(class device& device, const std::string& usnLine);
};

#endif // _DEVICE_DISCOVERY_H_
