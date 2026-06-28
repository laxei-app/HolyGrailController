// エッジ端末 設定プロビジョニング(仕様 8.2.2)の実装。
//  フロー: スマホがBLE接続 → CTRL に "start" write → エッジが PoP 生成+QR表示(edgeProvShowQr)
//          → スマホがQRを読み PoP を得て鍵=SHA256(PoP) を導出 → {name,ssid,pass} を AES-256-GCM 暗号化
//          → CRED に [IV(12)|暗号文|TAG(16)] を write → エッジが復号 → saveEdgeCreds + WiFi再接続。
//  PoP は QR(視覚)経由でのみ渡す(BLEには載せない)=所有証明。鍵不一致なら GCM 認証で復号失敗。
//  BLE は NimBLE(省RAM)。Bluedroid だと WiFi 併用で DRAM 枯渇しクラッシュするため。
#include "edgeProv.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <vector>
#include <cstring>

// main.cpp 側の連携関数。
extern void edgeProvShowQr(void);                 // PoP生成+QR表示(enterProv)
extern const std::string& edgePop(void);          // 現在のPoP
extern void edgeProvApply(const char* name, const char* ssid, const char* pass);  // 保存+WiFi再接続

namespace
{
	// 設定用 GATT サービス/特性 UUID(本アプリ固有)。
	const char* UUID_SVC  = "a1b2c3d4-0001-4a5b-8c6d-000000000001";
	const char* UUID_CTRL = "a1b2c3d4-0001-4a5b-8c6d-000000000002";  // write: "start"
	const char* UUID_CRED = "a1b2c3d4-0001-4a5b-8c6d-000000000003";  // write: 暗号化認証情報
	const char* UUID_STAT = "a1b2c3d4-0001-4a5b-8c6d-000000000004";  // read/notify: idle/qr/ok/fail

	NimBLECharacteristic* g_stat = nullptr;
	volatile bool g_startReq = false;
	volatile bool g_credReq  = false;
	std::string    g_credBlob;   // BLEタスクが書き込み、loop が読む

	void setStatus(const char* s)
	{
		if (g_stat) { g_stat->setValue((uint8_t*)s, strlen(s)); g_stat->notify(); }
	}

	class CtrlCb : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic* c) override
		{
			std::string v = c->getValue();
			if (v.rfind("start", 0) == 0) { g_startReq = true; }
		}
	};
	class CredCb : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic* c) override
		{
			g_credBlob = c->getValue();   // 生バイト([IV|CT|TAG])
			g_credReq  = true;
		}
	};
	class SrvCb : public NimBLEServerCallbacks
	{
		void onConnect(NimBLEServer*) override    { Serial.println("[PROV] BLE client connected"); }
		void onDisconnect(NimBLEServer*) override { Serial.println("[PROV] BLE client disconnected"); NimBLEDevice::startAdvertising(); }
	};

	// AES-256-GCM 復号。鍵=SHA256(PoP)。in=[IV(12)|CT|TAG(16)]。成功で out に平文。
	bool decryptCreds(const std::string& pop, const std::string& in, std::string& out)
	{
		if (in.size() < 12 + 16) { return false; }
		uint8_t key[32];
		mbedtls_sha256((const uint8_t*)pop.data(), pop.size(), key, 0);  // 0=SHA-256
		const uint8_t* d = (const uint8_t*)in.data();
		size_t n = in.size();
		const uint8_t* iv  = d;
		const uint8_t* ct  = d + 12;
		size_t ctLen = n - 12 - 16;
		const uint8_t* tag = d + 12 + ctLen;
		std::vector<uint8_t> pt(ctLen ? ctLen : 1);
		mbedtls_gcm_context gcm; mbedtls_gcm_init(&gcm);
		int r = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
		if (r == 0) { r = mbedtls_gcm_auth_decrypt(&gcm, ctLen, iv, 12, nullptr, 0, tag, 16, ct, pt.data()); }
		mbedtls_gcm_free(&gcm);
		if (r != 0) { return false; }
		out.assign((const char*)pt.data(), ctLen);
		return true;
	}

	// 最小 JSON 文字列値の取り出し("key":"value")。
	std::string pick(const std::string& s, const char* k)
	{
		std::string key = std::string("\"") + k + "\"";
		size_t p = s.find(key); if (p == std::string::npos) { return ""; }
		p = s.find(':', p);     if (p == std::string::npos) { return ""; }
		size_t q = s.find('"', p); if (q == std::string::npos) { return ""; }
		size_t e = s.find('"', q + 1); if (e == std::string::npos) { return ""; }
		return s.substr(q + 1, e - q - 1);
	}
}

namespace edgeProv
{
	void begin(const std::string& devName)
	{
		NimBLEDevice::init("HGC-Edge");
		NimBLEServer* srv = NimBLEDevice::createServer();
		srv->setCallbacks(new SrvCb());
		NimBLEService* svc = srv->createService(UUID_SVC);

		NimBLECharacteristic* ctrl = svc->createCharacteristic(UUID_CTRL, NIMBLE_PROPERTY::WRITE);
		ctrl->setCallbacks(new CtrlCb());
		NimBLECharacteristic* cred = svc->createCharacteristic(UUID_CRED, NIMBLE_PROPERTY::WRITE);
		cred->setCallbacks(new CredCb());
		g_stat = svc->createCharacteristic(UUID_STAT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
		setStatus("idle");

		svc->start();
		NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
		adv->addServiceUUID(UUID_SVC);
		adv->setScanResponse(true);
		NimBLEDevice::startAdvertising();
		Serial.printf("[PROV] BLE advertising started name=HGC-Edge svc=%s (dev=%s)\n", UUID_SVC, devName.c_str());
	}

	void loop(void)
	{
		if (g_startReq)
		{
			g_startReq = false;
			edgeProvShowQr();
			setStatus("qr");
			Serial.println("[PROV] start -> PoP generated + QR shown");
		}
		if (g_credReq)
		{
			g_credReq = false;
			std::string blob = g_credBlob;
			std::string plain;
			if (decryptCreds(edgePop(), blob, plain))
			{
				std::string name = pick(plain, "name");
				std::string ssid = pick(plain, "ssid");
				std::string pass = pick(plain, "pass");
				Serial.printf("[PROV] creds decrypted: name=%s ssid=%s passLen=%u\n",
				              name.c_str(), ssid.c_str(), (unsigned)pass.size());
				setStatus("ok");
				edgeProvApply(name.c_str(), ssid.c_str(), pass.c_str());
			}
			else
			{
				Serial.println("[PROV] creds decrypt FAILED (wrong PoP / tampered)");
				setStatus("fail");
			}
		}
	}
}
