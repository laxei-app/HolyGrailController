// BLE負荷テスト(中央役)。Wi-Fi/BLE 共存の実測用。製品コードは使わない独立スケッチ。
//
// 対象のエッジ端末(HGC-<名前>)へ BLE で接続し、設定用 GATT の STAT 特性を連続で read する。
// これで「接続が張られて ATT が流れ続けている」状態を作る。撮影側の周期がこの間に
// どれだけ乱れるかを、撮影レポートの late/busy/prep と突き合わせて評価する。
//
// 画面/シリアルに 1秒あたりの read 回数と累計を出す。切断したら自動で張り直す。
#include <Arduino.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>

// 接続先。エッジは "HGC-<端末名>" で広告している(edgeProv.cpp)。
static const char* TARGET_NAME = "HGC-Stick01";

// 設定用 GATT(edgeProv.cpp と同じ UUID)。STAT は read/notify なので read を投げ続けられる。
static const char* UUID_SVC  = "a1b2c3d4-0001-4a5b-8c6d-000000000001";
static const char* UUID_STAT = "a1b2c3d4-0001-4a5b-8c6d-000000000004";

static NimBLEClient*            g_cl   = nullptr;
static NimBLERemoteCharacteristic* g_stat = nullptr;
static uint32_t g_reads = 0, g_fails = 0, g_lastShow = 0, g_lastReads = 0;
static bool     g_connected = false;
// A/B を切り替えるためのスイッチ。既定は OFF(=広告のみの状態を作る)。
// シリアルに 'c' で負荷開始、'x' で停止。撮影を止めずに条件を切り替えられる。
static bool     g_enabled   = false;

static void show(const char* line1, const char* line2)
{
	M5.Display.fillScreen(TFT_BLACK);
	M5.Display.setCursor(0, 10);
	M5.Display.setTextSize(2);
	M5.Display.println(line1);
	M5.Display.println(line2);
}

// 対象を探して接続し、STAT 特性を掴む。
static bool connectTarget(void)
{
	NimBLEScan* scan = NimBLEDevice::getScan();
	scan->setActiveScan(true);
	NimBLEScanResults res = scan->getResults(5000, false);
	const NimBLEAdvertisedDevice* found = nullptr;
	for (int i = 0; i < res.getCount(); ++i)
	{
		const NimBLEAdvertisedDevice* d = res.getDevice(i);
		if (d->getName() == TARGET_NAME) { found = d; break; }
	}
	scan->clearResults();
	if (!found) { Serial.printf("[BLELOAD] %s not found\n", TARGET_NAME); return false; }

	if (g_cl == nullptr) { g_cl = NimBLEDevice::createClient(); }
	if (!g_cl->connect(found)) { Serial.println("[BLELOAD] connect failed"); return false; }
	g_cl->exchangeMTU();	// できるだけ大きく(1回あたりの転送量を増やして負荷を上げる)
	// 接続間隔を詰める。既定(約180ms)は省電力寄りで、無線を占有する時間が短い=甘い条件になる。
	// ログ転送のような実使用の最悪ケースに近づけるため 15ms を要求する(単位1.25ms)。
	//  引数: minInterval, maxInterval, latency, supervisionTimeout(単位10ms)
	g_cl->updateConnParams(12, 12, 0, 400);

	NimBLERemoteService* svc = g_cl->getService(UUID_SVC);
	if (svc == nullptr) { Serial.println("[BLELOAD] service not found"); g_cl->disconnect(); return false; }
	g_stat = svc->getCharacteristic(UUID_STAT);
	if (g_stat == nullptr) { Serial.println("[BLELOAD] char not found"); g_cl->disconnect(); return false; }

	Serial.printf("[BLELOAD] connected to %s (mtu=%u)\n", TARGET_NAME, (unsigned)g_cl->getMTU());
	return true;
}

void setup(void)
{
	auto cfg = M5.config();
	M5.begin(cfg);
	M5.Display.setTextSize(2);
	Serial.begin(115200);
	delay(300);
	Serial.printf("[BLELOAD] start. target=%s\n", TARGET_NAME);
	NimBLEDevice::init("HGC-BLELOAD");
	NimBLEDevice::setMTU(517);
	show("BLE LOAD", "scanning...");
}

void loop(void)
{
	M5.update();
	while (Serial.available() > 0)
	{
		const int c = Serial.read();
		if (c == 'c') { g_enabled = true;  Serial.println("[BLELOAD] enabled");  }
		if (c == 'x') { g_enabled = false; Serial.println("[BLELOAD] disabled"); }
	}
	if (!g_enabled)
	{
		if (g_cl != nullptr && g_cl->isConnected()) { g_cl->disconnect(); g_connected = false; }
		show("BLE LOAD", "OFF");
		delay(500);
		return;
	}
	if (!g_connected || g_cl == nullptr || !g_cl->isConnected())
	{
		g_connected = connectTarget();
		if (!g_connected) { show("BLE LOAD", "no target"); delay(1000); return; }
		show("BLE LOAD", "connected");
	}

	// 連続 read。1回ごとに待たない = 接続イベントを詰められるだけ詰める。
	NimBLEAttValue v = g_stat->readValue();
	if (v.size() > 0) { ++g_reads; } else { ++g_fails; }

	const uint32_t now = millis();
	if (now - g_lastShow >= 1000)
	{
		const uint32_t rps = g_reads - g_lastReads;
		g_lastReads = g_reads; g_lastShow = now;
		char l1[32], l2[32];
		std::snprintf(l1, sizeof(l1), "read/s %lu", (unsigned long)rps);
		std::snprintf(l2, sizeof(l2), "tot %lu ng %lu", (unsigned long)g_reads, (unsigned long)g_fails);
		show(l1, l2);
		Serial.printf("[BLELOAD] %lu read/s  total=%lu ng=%lu\n",
		              (unsigned long)rps, (unsigned long)g_reads, (unsigned long)g_fails);
	}
}
