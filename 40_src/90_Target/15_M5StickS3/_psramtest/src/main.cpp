// StickS3 Octal PSRAM 検証(最小)。Arduino-ESP32 3.x(pioarduino)で OPI PSRAM が
// 安定して読み書きできるかだけを見る。表示はバックライト(GPIO38)の点滅パターン。
#include <Arduino.h>
#include <esp_heap_caps.h>

static const int PIN_BL = 38;   // LCDバックライト。点滅で結果表示に流用。

static void blink(int n, int on_ms, int off_ms) {
	for (int i = 0; i < n; ++i) {
		digitalWrite(PIN_BL, HIGH); delay(on_ms);
		digitalWrite(PIN_BL, LOW);  delay(off_ms);
	}
}

void setup() {
	pinMode(PIN_BL, OUTPUT);
	digitalWrite(PIN_BL, LOW);
	Serial.begin(115200);
	delay(4000);                    // USB-CDC 列挙待ち(ログを拾えるように)

	Serial.println();
	size_t psTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
	size_t psFree  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
	Serial.printf("[PSRAMTEST] SPIRAM total=%u free=%u\n", (unsigned)psTotal, (unsigned)psFree);
	Serial.printf("[PSRAMTEST] ESP.getPsramSize=%u\n", (unsigned)ESP.getPsramSize());
	Serial.flush();

	if (psTotal == 0) {
		Serial.println("[PSRAMTEST] NO PSRAM DETECTED"); Serial.flush();
		for (;;) { blink(10, 80, 80); delay(600); }     // 速い点滅の連続 = 未検出
	}

	const size_t N = 2 * 1024 * 1024;   // 2MB
	Serial.println("[PSRAMTEST] heap_caps_malloc(2MB, SPIRAM)..."); Serial.flush();
	uint8_t* p = (uint8_t*)heap_caps_malloc(N, MALLOC_CAP_SPIRAM);
	Serial.printf("[PSRAMTEST] ptr=%p\n", (void*)p); Serial.flush();
	if (!p) {
		Serial.println("[PSRAMTEST] ALLOC FAIL"); Serial.flush();
		for (;;) { blink(6, 300, 300); delay(800); }    // 中速点滅 = 確保失敗
	}

	Serial.println("[PSRAMTEST] writing..."); Serial.flush();
	for (size_t i = 0; i < N; i += 4096) { p[i] = (uint8_t)(i >> 12); }
	Serial.println("[PSRAMTEST] reading back..."); Serial.flush();
	bool ok = true;
	for (size_t i = 0; i < N; i += 4096) { if (p[i] != (uint8_t)(i >> 12)) { ok = false; break; } }
	Serial.printf("[PSRAMTEST] verify %s\n", ok ? "OK" : "FAIL"); Serial.flush();
	free(p);

	if (ok) {
		Serial.println("[PSRAMTEST] RESULT: PSRAM OK"); Serial.flush();
		blink(3, 500, 500);            // 3回ゆっくり点滅…
		digitalWrite(PIN_BL, HIGH);    // …のあと点灯しっぱなし = 成功
	} else {
		Serial.println("[PSRAMTEST] RESULT: VERIFY FAIL"); Serial.flush();
		for (;;) { blink(6, 300, 300); delay(800); }
	}
}

void loop() { delay(1000); }
