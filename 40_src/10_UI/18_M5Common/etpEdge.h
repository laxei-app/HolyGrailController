#ifndef _ETP_EDGE_H_
#define _ETP_EDGE_H_
// エッジ端末(M5Stack)側の ETP サーバ(データ構造仕様書43 §6)。
//  - UDP(50505): スマホのブロードキャスト検索(search)に edgeInfo を応答する。
//  - TCP(50506): time/capturePlan/action/stop/progress を受けて holyGrailEntity を駆動する。
//  - time 受信時に RTC とシステム時計を同期する。起動時は RTC からシステム時計を復元する。
// WiFi 接続後に setup() を呼び、loop() を周期的に呼ぶ(非ブロッキングでポーリングする)。

#include <string>
#include <vector>

#include "etp.h"

namespace etpEdge
{
	// 1フレームを処理して応答フレームを返す(トランスポート非依存)。BLE 経路(etpBle)が使う。
	// 中身は TCP と完全に同じ処理を通る。
	//  conn = BLE の接続番号。持ち主かどうかを**接続ごと**に見分けるのに使う
	//  (2台つないでいるとき、片方の名乗りでもう片方の要求が通ってしまわないように)。
	std::vector<uint8_t> handleFrame(const etp::packet& pk, uint16_t conn);

	// 検索応答(C_SEARCH)と同じ edgeInfo の JSON。BLE でも同じものを返す。
	std::string infoJson(void);

	// このスマホの札で登録・設定してよいか。持ち主が居ないうちは誰でも可。
	//  起動直後の猶予(下記)の間も可: **電源を入れ直せる人＝端末の画面を見られる人**なので、
	//  「画面を見られるか」という境界は崩れない。持ち主のスマホを失くしたときの唯一の逃げ道。
	bool ownerAllows(const std::string& phoneId);
	// 起動直後の猶予[ミリ秒]。この間は持ち主でなくても登録できる。
	constexpr uint32_t CLAIM_GRACE_MS = 60000;

	// 持ち主のスマホを登録する(プロビジョニングから呼ぶ)。
	//  QR の PoP で導いた鍵で復号できた中身に入っていた識別子だけを受け取る。
	//  = 端末の画面を見られる人しか持ち主になれない。
	void setOwner(const std::string& phoneId);

	// RTC からシステム時計を復元し、UDP/TCP サーバを開始する。
	// edgeName: 検索応答で返すエッジ端末の名称。
	void setup(const std::string& edgeName);

	// UDP/TCP をポーリングして要求を処理する(loop から周期的に呼ぶ)。
	void loop(void);
}

#endif // _ETP_EDGE_H_
