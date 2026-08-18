#pragma once
#include "common.h"

using namespace std;

class DeviceDiscovery
{
public:
	class device
	{
		std::string location;		// ロケーション
		uint16_t	port;			// 接続先 port
		std::string	ipa;			// ip アドレス

		// 以下デバイスディスクリプタの内容
		std::string	model;			// カメラモデル名 "Canon EOS R10"
		std::string	assignedName;		// 愛称
		std::string	manufacturer;	// 提供元。"canon","sony"
		std::string	serialno;		// シリアルno.
	};

public:
	DeviceDiscovery() {};
	virtual ~DeviceDiscovery() {}
	int search(vector<device> & device, vector<string> target);
};

