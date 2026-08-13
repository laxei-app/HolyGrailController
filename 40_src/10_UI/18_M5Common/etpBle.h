#ifndef _ETP_BLE_H_
#define _ETP_BLE_H_
// エッジ端末の ETP を BLE(GATT)でも受けられるようにする経路。
//
// 【なぜ要るか】屋外でルーターが無い運用ではエッジ自身が AP になり、カメラがそこへ繋がる。
//  スマホもその AP へ入らないと話せないため、エッジが複数台あると SSID を切り替えて回る
//  ことになる。BLE ならスマホは Wi-Fi を離れずに全台と話せる。
//
// 【何を足すだけで済むか】ETP はもともとトランスポート非依存で、etp::decode() は
//  「消費バイト数」を返すストリーム用フレーミングになっている。だから BLE でも
//  同じフレームをそのまま流せばよく、**プロトコルは一切変えていない**。
//  受けたフレームは etpEdge::handleFrame() へ渡す = TCP と完全に同じ処理を通る。
//
// 【スマホ側だけが選ぶ】どちらで話すかはスマホが決める。エッジは常に両方で待ち受ける。
//  エッジ側にモードを持たせないので、「Wi-Fi専用にしたら届かなくなって戻せない」で
//  現地に行く羽目になる経路が無い。
//
// 【注意】GATT のコールバックは BLE タスク上で走る。撮影の開始やファイル読み書きを
//  そこで行うと危険なので、受信はバッファへ積むだけにして、処理は loop()(メインループ)で行う。
#include <cstddef>
#include <cstdint>

class NimBLEServer;

namespace etpBle
{
	// 既存の NimBLE サーバへ ETP 用サービスを追加する(edgeProv がサーバを作った直後に呼ぶ)。
	void attach(NimBLEServer* srv);

	// 受信フレームの処理と応答の送信(メインループから周期的に呼ぶ)。
	void loop(void);

	// いまスマホと BLE でつながっているか(表示・診断用)。
	bool connected(void);

	// --- サーバのイベント中継 ---
	// NimBLE はサーバのコールバックを 1 つしか持てない。プロビジョニング(edgeProv)が
	// 登録済みなので、そこから以下を呼んでもらう。
	void onConnected(void);
	void onDisconnected(void);
	void onMtu(uint16_t mtu);
}

#endif // _ETP_BLE_H_
