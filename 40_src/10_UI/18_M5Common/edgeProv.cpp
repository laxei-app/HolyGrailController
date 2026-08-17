// エッジ端末 設定プロビジョニング(仕様 8.2.2)の実装。
//  フロー: スマホがBLE接続 → CTRL に "start" write → エッジが PoP 生成+QR表示(edgeProvShowQr)
//          → スマホがQRを読み PoP を得て鍵=SHA256(PoP) を導出 → {name,ssid,pass} を AES-256-GCM 暗号化
//          → CRED に [IV(12)|暗号文|TAG(16)] を write → エッジが復号 → saveEdgeCreds + WiFi再接続。
//  PoP は QR(視覚)経由でのみ渡す(BLEには載せない)=所有証明。鍵不一致なら GCM 認証で復号失敗。
//  BLE は NimBLE(省RAM)。Bluedroid だと WiFi 併用で DRAM 枯渇しクラッシュするため。
#include "edgeProv.h"
#include "etpBle.h"		// ETP を BLE でも受ける経路(同じ NimBLE サーバへ相乗り)
#include "holyGrailEntity.h"	// 撮影中かどうかの判定(hge_getState)
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <vector>
#include <cstring>

// main.cpp 側の連携関数。
extern void edgeProvShowQr(void);                 // PoP生成+QR表示(enterProv)
extern const std::string& edgePop(void);          // 現在のPoP
// 保存+ネットワーク反映。mode="sta"(ssid/passで参加) / "ap"(エッジ自身がAP)。空は"sta"扱い。
extern void edgeProvApply(const char* name, const char* ssid, const char* pass, const char* mode);

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

	// NimBLE 2.x のコールバック署名(NimBLEConnInfo& / onDisconnect の理由int)に合わせている。
	// 以前は 1.4 と両立させるため EDGE_NIMBLE2 で分岐していたが、両機とも 2.x になったので削除した。
	class CtrlCb : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override
		{
			std::string v = c->getValue();
			if (v.rfind("start", 0) == 0) { g_startReq = true; }
		}
	};
	class CredCb : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override
		{
			g_credBlob = c->getValue();   // 生バイト([IV|CT|TAG])
			g_credReq  = true;
		}
	};
	// サーバのコールバックは NimBLE に 1 つしか登録できない。プロビジョニングと ETP の
	// 両方がこのサーバを使うので、ここで受けて etpBle へ中継する。
	class SrvCb : public NimBLEServerCallbacks
	{
		void onConnect(NimBLEServer* srv, NimBLEConnInfo&) override
		{
			Serial.printf("[PROV] BLE client connected (conn=%d)\n", (int)(srv ? srv->getConnectedCount() : 0));
			etpBle::onConnected();
			// 【つながっていても広告を続ける(2026-08-17)】NimBLE は接続すると広告を止める。
			//  ETP を BLE で運ぶ設定にしているとスマホがつなぎっぱなしにするので、エッジが
			//  広告しなくなり **設定変更のQRを出す経路が使えなくなる**(スマホのプロビジョニング
			//  画面は "HGC-<端末名>" の広告を名前一致で探すため「見つかりません」になる)。
			//  屋外のAPモード運用中に SSID/パスワードを変えられなくなるのは詰みなので、
			//  接続中も広告を出し続ける。同時接続は CONFIG_BT_NIMBLE_MAX_CONNECTIONS=3 まで。
			NimBLEDevice::startAdvertising();
		}
		void onDisconnect(NimBLEServer* srv, NimBLEConnInfo&, int) override
		{
			const int left = srv ? (int)srv->getConnectedCount() : 0;
			Serial.printf("[PROV] BLE client disconnected (conn=%d)\n", left);
			// ETP の受信状態を捨てるのは**最後の1本が切れたとき**だけ。プロビジョニング用の
			//  2本目が切れただけで捨てると、生きている ETP の途中フレームを壊す。
			if (left <= 0) { etpBle::onDisconnected(); }
			NimBLEDevice::startAdvertising();
		}
		void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override { etpBle::onMtu(mtu); }
	};

	// いま撮影の最中か(武装〜撮影〜停止処理まで含む)。
	// 撮影中に Wi-Fi モード(AP/STA)を切り替えるとカメラとの回線が切れて撮影が壊れるので、
	// その間は設定の適用を断る(2026-08-14 指示)。スマホ側でも同じ判定でブロックする。
	bool capturingNow(void)
	{
		const int st = hge_getState();
		return (st != HGE_ST_IDLE) && (st != HGE_ST_ERROR);
	}

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
		// 【広告名に端末名を入れる(2026-08-08 UI依頼)】従来は全機が "HGC-Edge" を広告しており、
		//  スマホは最初に見つけた1台へ無条件で接続していた。エッジを複数台起動していると
		//  どれに設定が飛ぶか分からず、登録済み端末の設定を更新できなかった。
		//  端末名が決まっていれば "HGC-<端末名>" を広告し、スマホ側は名前一致で選ぶ。
		//  出荷時(名前未設定)は従来どおり "HGC-Edge" を広告して新規登録を妨げない。
		const std::string advName = devName.empty() ? std::string("HGC-Edge") : ("HGC-" + devName);
		NimBLEDevice::init(advName);
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
		etpBle::attach(srv);	// ETP 用サービスを同じサーバへ追加する(BLE は常に使える)
		NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
		adv->addServiceUUID(UUID_SVC);
		adv->enableScanResponse(true);
		// 【名前を広告に載せる(2026-08-08 修正)】NimBLE 2.x は NimBLEDevice::init(name) で
		//  GAPの端末名を設定しても、広告パケット/スキャン応答には自動で入れない。
		//  そのためスマホ側の名前一致が永久に成立せず、QR表示要求が12秒でタイムアウト
		//  していた(Edje00 のAPモード設定が始められなかった)。明示的に載せる。
		//  enableScanResponse(true) の後に呼ぶこと(setName は scanResp が立っていれば
		//  スキャン応答側へ入れる。広告本体は128bitサービスUUIDで18バイト使うため手狭)。
		adv->setName(advName);
		NimBLEDevice::startAdvertising();
		Serial.printf("[PROV] BLE advertising started name=%s svc=%s (dev=%s)\n", advName.c_str(), UUID_SVC, devName.c_str());
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
				std::string mode = pick(plain, "mode");	// "sta"(既定) / "ap"。スマホからのモード切替。
				Serial.printf("[PROV] creds decrypted: name=%s ssid=%s passLen=%u mode=%s\n",
				              name.c_str(), ssid.c_str(), (unsigned)pass.size(), mode.c_str());
				// 撮影中はネットワーク設定を適用しない。AP/STA を切り替えるとカメラとの回線が
				// 切れて撮影が壊れるため(2026-08-14 指示)。スマホ側でもブロックするが、
				// エッジ単体でも守れるようにここでも見る。
				if (capturingNow())
				{
					Serial.println("[PROV] rejected: capturing");
					setStatus("busy");
				}
				else
				{
					setStatus("ok");
					edgeProvApply(name.c_str(), ssid.c_str(), pass.c_str(), mode.c_str());
				}
			}
			else
			{
				Serial.println("[PROV] creds decrypt FAILED (wrong PoP / tampered)");
				setStatus("fail");
			}
		}
	}
}
