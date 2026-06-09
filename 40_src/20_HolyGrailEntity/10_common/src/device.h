#ifndef _DEVICE_H_
#define _DEVICE_H_
#include "common.h"
#include "apiBase.h"

class device
{
public:
	enum class apiClass
	{
		NON = 0,
		CANON_CCAPI,
		SONY_DI
	};

public:
	// デバイスディスカバリの内容
	std::string	uuid;			// uuid。一意にデバイスを決める
	std::string location;		// device ロケーション
	std::string urn;			// urn メーカー名を区別する
	std::string service;		// service ネットワークサービス名称

	// 以下デバイスディスクリプタの内容
	std::string	model;			// カメラモデル名 "Canon EOS R10"
	std::string	friendName;		// 愛称
	std::string	manufacturer;	// 提供元。"canon","sony"
	std::string	serialno;		// シリアルno.
	std::string urlbase;		// url base
	std::string urlAccess;		// access URL

	// api について
	apiClass	apiClass = apiClass::NON;	// これを見て apiBase を設定する
	class apiBase*	apiBase = nullptr;	// カメラAPI

public:
	virtual ~device();
	void clear(void);
};

#endif // _DEVICE_H_