#pragma once

// SoftAP への参加/離脱をログへ残す(2026-08-23)。
//
// 【なぜ要るか】カメラ3台を Edge00 の AP へ繋ごうとすると、2台までは繋がるのに3台目が
//  切れる、という事象が出た。エッジ側には「今つながっている台数」しか手掛かりが無く
//  (neighborHostIps は DHCP の貸出一覧なので、参加しても貸出前なら 0 に見える)、
//  離脱が「カメラの都合」なのか「APが蹴った」のかを区別できない。
//  Wi-Fi のイベントには**離脱理由コード**が付いてくるので、それを残せば1回で切り分けられる。
//
// 【読み方】reason は IEEE802.11 の Reason Code + ESP独自拡張。よく出るもの:
//   1=UNSPECIFIED / 2=AUTH_EXPIRE / 3=AUTH_LEAVE / 4=ASSOC_EXPIRE(無通信で追い出し)
//   5=ASSOC_TOOMANY(**受け入れ上限**) / 7=NOT_ASSOCED / 8=ASSOC_LEAVE(相手から切断)
//   15=4WAY_HANDSHAKE_TIMEOUT(**パスワード不一致や鍵交換の失敗**) / 200番台=ESP独自
//
// 置き場所は 18_M5Common。CoreS3 も StickS3 も同じものを使う(機種で分岐しない)。
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "dataManager.h"
#include "edgeApLeases.h"	// 配った IP を覚えて次回の配布範囲を外す(IP重複の防止)

namespace edgeApEvents
{
	// 【イベントタスクではファイルを触らないこと(2026-08-28)】
	//  ここは Arduino のイベントループのタスク(arduino_events)で、Wi-Fi のイベントを
	//  順番に配る場所である。ここでファイルを書くと、その間ほかのイベントが全部止まる。
	//  実機のログに、このタスクの追記と loop() 側の追記が**1行に混ざった**跡が残っている:
	//    14:22:12 camera book received: 1 camera(s)
	//    14:22:18 camera book recei5:50:1C:FC aid=1
	//  この直後に端末が停止した(loop() が止まり、画面・時計・シリアル・BLE・ETP が全滅。
	//  撮影は別タスクなので動き続けた)。因果は確定していないが、イベントタスクで
	//  ファイルを触るのは元より誤りなので、ここでは RAM に積むだけにして、
	//  ログ出しは pump()(通常のループ)へ回す。
	//
	//  なお edgeApLeases も同じ決まりで作られている(「イベントタスクでは RAM だけ触り、
	//  書き込みは pump に任せる」)。ここだけが破っていた。

	struct pending { char kind; uint8_t mac[6]; uint16_t aid; uint16_t reason; uint32_t ip; };

	inline std::vector<pending>& queue(void) { static std::vector<pending> v; return v; }

	// 溜めすぎない。イベントが嵐のように来ても RAM を食い潰さないための上限。
	constexpr size_t kMaxPending = 24;

	inline void push(const pending& p)
	{
		if (queue().size() >= kMaxPending) { return; }	// あふれたら捨てる(ログが欠けるだけ)
		queue().push_back(p);
	}

	inline void onEvent(arduino_event_id_t id, arduino_event_info_t info)
	{
		pending p{};
		if (id == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
		{
			p.kind = 'j';
			std::memcpy(p.mac, info.wifi_ap_staconnected.mac, 6);
			p.aid = (uint16_t)info.wifi_ap_staconnected.aid;
			push(p);
		}
		else if (id == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED)
		{
			p.kind = 'l';
			std::memcpy(p.mac, info.wifi_ap_stadisconnected.mac, 6);
			p.aid    = (uint16_t)info.wifi_ap_stadisconnected.aid;
			p.reason = (uint16_t)info.wifi_ap_stadisconnected.reason;
			push(p);
			edgeApLeases::onLeft(p.mac);	// RAM に積むだけ。IP へ直すのは pump
		}
		else if (id == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
		{
			// このイベントは配った IP と**相手の MAC**の両方をくれる(esp-idf 5 / arduino 3.x)。
			//  MAC があるので「同じ端末が別の IP をもらった=古い IP は空いた」と判断できる。
			p.kind = 'a';
			std::memcpy(p.mac, info.wifi_ap_staipassigned.mac, 6);
			p.ip = info.wifi_ap_staipassigned.ip.addr;
			push(p);
			edgeApLeases::onAssigned(p.mac, p.ip);	// RAM のみ(このファイルの決まりどおり)
		}
	}

	// 溜まったイベントをログへ出す。**通常のループ(pump)から呼ぶこと。**
	inline void pump(void)
	{
		if (queue().empty()) { return; }
		std::vector<pending> take;
		take.swap(queue());
		char d[128];
		for (const pending& p : take)
		{
			if (p.kind == 'j')
			{
				std::snprintf(d, sizeof(d), "join mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u",
				              p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5], (unsigned)p.aid);
				dataManager::logEvent("APSTA", d);
			}
			else if (p.kind == 'l')
			{
				std::snprintf(d, sizeof(d), "leave mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u reason=%u",
				              p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
				              (unsigned)p.aid, (unsigned)p.reason);
				dataManager::logEvent("APSTA", d, true);
			}
			else if (p.kind == 'a')
			{
				std::snprintf(d, sizeof(d), "lease ip=%s mac=%02X:%02X:%02X:%02X:%02X:%02X",
				              IPAddress(p.ip).toString().c_str(),
				              p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);
				dataManager::logEvent("APSTA", d);
			}
		}
	}

	// setup から1回呼ぶ(AP起動の前後どちらでもよい)。
	inline void start(void) { WiFi.onEvent(onEvent); }
}
