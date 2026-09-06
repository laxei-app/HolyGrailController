#ifndef _DEVICE_H_
#define _DEVICE_H_
#include "common.h"
#include "apiBase.h"
#include <memory>

class device
{
public:
	// デバイスディスカバリの内容
	std::string	uuid;			// uuid。一意にデバイスを決める
	std::string location;		// device ロケーション
	std::string urn;			// urn メーカー名を区別する
	std::string service;		// service ネットワークサービス名称

	// 以下デバイスディスクリプタの内容
	std::string	model;			// カメラモデル名 "Canon EOS R10"
	std::string	assignedName;		// ユーザーがカメラ本体で付けた名前(愛称)。同機種の見分けに使う
	std::string	manufacturer;	// 提供元。"canon","sony"
	std::string	serialno;		// シリアルno.
	std::string urlbase;		// url base
	std::string urlAccess;		// access URL

	// カメラAPI。共有所有(shared_ptr): device を値コピー(S->dev=*hit 等)すると参照を共有し、
	// 最後の参照が消えた時に解放される。旧実装は二重解放回避のため ~device で意図的に解放せず
	// リークさせていた(反転ガード)が、shared_ptr で正しく所有権を管理しリークを根治した。
	std::shared_ptr<class apiBase>	apiBase;
	// この device を見つけた探索元(2026-09-06)。挨拶のように「見つけた側にしか分からない手順」を
	//  頼むときの宛先。探索元は cameraController::backends() が寿命を持つので生ポインタでよい。
	class detectBase* origin = nullptr;

public:
	virtual ~device() = default;
	void clear(void);
};

#endif // _DEVICE_H_