// ETP を BLE(GATT)で受ける経路。設計の意図はヘッダを参照。
#include "etpBle.h"
#include <Arduino.h>	// delay()
#include <NimBLEDevice.h>
#include <vector>
#include <string>
#include "etpEdge.h"
#include "etp.h"
#include "debugOut.h"

namespace
{
	// ETP 用 GATT(本アプリ固有。プロビジョニング用 a1b2c3d4-0001-... とは別のサービス)。
	const char* UUID_SVC = "a1b2c3d4-0002-4a5b-8c6d-000000000001";
	const char* UUID_RX  = "a1b2c3d4-0002-4a5b-8c6d-000000000002";  // write: スマホ→エッジ(ETPフレーム)
	const char* UUID_TX  = "a1b2c3d4-0002-4a5b-8c6d-000000000003";  // notify: エッジ→スマホ(応答フレーム)

	NimBLECharacteristic* g_tx = nullptr;
	bool                  g_connected = false;

	// BLE タスクが書き、メインループが読む。ESP32 は単一コアではないので排他が要る。
	portMUX_TYPE          g_mux = portMUX_INITIALIZER_UNLOCKED;
	std::vector<uint8_t>  g_rx;			// 受信バッファ(フレーミング用)
	constexpr size_t      RX_MAX = 16384;	// 想定外の流入でメモリを食い潰さないための上限

	// 1回の notify で送れる量。MTU-3(ATTヘッダ)。接続時に更新する。
	size_t                g_chunk = 20;

	class RxCb : public NimBLECharacteristicCallbacks
	{
		void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) override
		{
			// ここは BLE タスク。積むだけにして、処理は loop() へ回す。
			const std::string v = c->getValue();
			if (v.empty()) { return; }
			portENTER_CRITICAL(&g_mux);
			if (g_rx.size() + v.size() <= RX_MAX)
			{
				g_rx.insert(g_rx.end(), v.begin(), v.end());
			}
			else
			{
				g_rx.clear();	// 溢れた = 同期が壊れている。捨てて作り直す
			}
			portEXIT_CRITICAL(&g_mux);
		}
	};

	// 応答フレームを notify で送る。MTU を超える分は分割する。
	//  ETP は自分でフレーム長を持っているので、受け側は届いた順に積んで decode すればよい。
	//  分割用の独自ヘッダは付けない。
	void sendReply(const std::vector<uint8_t>& out)
	{
		if (g_tx == nullptr || !g_connected) { return; }
		size_t off = 0;
		while (off < out.size())
		{
			const size_t n = ((out.size() - off) < g_chunk) ? (out.size() - off) : g_chunk;
			g_tx->setValue(out.data() + off, n);
			// notify はキューが詰まると失敗する。少し待って詰め直す(落とすと応答が壊れるため)。
			for (int i = 0; i < 20 && !g_tx->notify(); ++i) { delay(5); }
			off += n;
		}
	}
}

namespace etpBle
{
	void attach(NimBLEServer* srv)
	{
		if (srv == nullptr) { return; }
		// サーバのコールバックは 1 つしか登録できない(setCallbacks は置き換え)。
		// プロビジョニング側(edgeProv)が持っているので、そこから onConnected/onDisconnected/
		// onMtu を呼んでもらう形にする。ここでは登録しない。
		NimBLEService* svc = srv->createService(UUID_SVC);
		NimBLECharacteristic* rx = svc->createCharacteristic(
			UUID_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
		rx->setCallbacks(new RxCb());
		g_tx = svc->createCharacteristic(UUID_TX, NIMBLE_PROPERTY::NOTIFY);
		svc->start();
		NimBLEDevice::getAdvertising()->addServiceUUID(UUID_SVC);
		DBGLN(col::GRN, "etpBle: service up (%s)", UUID_SVC);
	}

	void loop(void)
	{
		// BLE タスクが積んだぶんを取り出す(排他は最小区間だけ)。
		std::vector<uint8_t> buf;
		portENTER_CRITICAL(&g_mux);
		if (!g_rx.empty()) { buf.swap(g_rx); }
		portEXIT_CRITICAL(&g_mux);
		if (buf.empty()) { return; }

		size_t pos = 0;
		while (pos < buf.size())
		{
			etp::packet pk;
			const int c = etp::decode(buf.data() + pos, buf.size() - pos, pk);
			if (c > 0)
			{
				DBGLN(col::YEL, "etpBle: rx cmd=%u m=%u len=%u",
				      (unsigned)pk.cmd, (unsigned)pk.method, (unsigned)pk.data.size());
				sendReply(etpEdge::handleFrame(pk));	// TCP と同じ処理を通る
				pos += static_cast<size_t>(c);
			}
			else if (c == 0) { break; }		// データ不足 → 続きを待つ
			else             { pos += 1; }	// 不正 → 1バイト進めて再同期
		}
		// 使い残し(フレーム途中)は次回へ戻す。新たに届いた分より前に置く。
		if (pos < buf.size())
		{
			portENTER_CRITICAL(&g_mux);
			g_rx.insert(g_rx.begin(), buf.begin() + static_cast<long>(pos), buf.end());
			portEXIT_CRITICAL(&g_mux);
		}
	}

	bool connected(void) { return g_connected; }

	// --- サーバのイベント。edgeProv のサーバコールバックから中継してもらう ---
	void onConnected(void)
	{
		g_connected = true;
		DBGLN(col::GRN, "etpBle: connected");
	}
	void onDisconnected(void)
	{
		g_connected = false;
		portENTER_CRITICAL(&g_mux);
		g_rx.clear();		// 途中のフレームは次の接続へ持ち越さない
		portEXIT_CRITICAL(&g_mux);
		DBGLN(col::CYN, "etpBle: disconnected");
	}
	void onMtu(uint16_t mtu)
	{
		g_chunk = (mtu > 3) ? static_cast<size_t>(mtu - 3) : 20;
		DBGLN(col::CYN, "etpBle: mtu=%u chunk=%u", (unsigned)mtu, (unsigned)g_chunk);
	}
}
