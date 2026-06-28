// エッジ端末 設定プロビジョニング(仕様 8.2.2)。
// BLE GATT サーバを立て、スマホから「設定開始」要求→PoP生成+QR表示→
// PoP由来鍵(SHA256)でAES-GCM暗号化された接続情報(端末名/SSID/password)を受信→復号→保存。
// BLE コールバックは BLE タスクで走るので、重い処理(復号/NVS/WiFi再接続)は loop() 側で行う。
#ifndef EDGE_PROV_H
#define EDGE_PROV_H

#include <string>

namespace edgeProv
{
	// BLE GATT サーバを初期化して広告を開始する(setup から1回)。
	void begin(const std::string& devName);
	// 保留中の要求(設定開始/認証情報受信)をメインスレッドで処理する(loop から毎回)。
	void loop(void);
}

#endif // EDGE_PROV_H
