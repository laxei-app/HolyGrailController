#include "device.h"

// ~device は = default(device.h)。apiBase は shared_ptr なので自動解放される。

// 内容を消去する
void device::clear(void)
{
	//deviceDiscovery の内容
	uuid.clear();			// uuid。一意にデバイスを決める
	location.clear();	// device ロケーション

	// 以下デバイスディスクリプタの内容
	model.clear();			// 型番 "EOS R10"
	assignedName.clear();		// ユーザーがカメラ本体で付けた名前(愛称)
	manufacturer.clear();	// メーカー1語 "Canon"
	serialno.clear();		// シリアルno.
	urlbase.clear();		// url base
	urlAccess.clear();		// access URL

	// api への参照を解放(最後の参照なら apiBase 実体も解放される)
	apiBase = nullptr;
	origin = nullptr;
}
