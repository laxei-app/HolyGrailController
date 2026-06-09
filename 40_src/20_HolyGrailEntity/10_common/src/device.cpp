#include "device.h"

device::~device()
{
	if (this->apiBase == nullptr)
	{
		delete this->apiBase;
		this->apiBase = nullptr;
	}
}

// 内容を消去する
void device::clear(void)
{
	//deviceDiscovery の内容
	uuid.clear();			// uuid。一意にデバイスを決める
	location.clear();	// device ロケーション

	// 以下デバイスディスクリプタの内容
	model.clear();			// カメラモデル名 "Canon EOS R10"
	friendName.clear();		// 愛称
	manufacturer.clear();	// 提供元。"canon","sony"
	serialno.clear();		// シリアルno.
	urlbase.clear();		// url base
	urlAccess.clear();		// access URL

	// api を削除
	if (this->apiBase == nullptr)
	{
		delete this->apiBase;
		this->apiBase = nullptr;
	}
	apiClass = apiClass::NON;
}
