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
#include "dataManager.h"

namespace edgeApEvents
{
	inline void onEvent(arduino_event_id_t id, arduino_event_info_t info)
	{
		char d[128];
		if (id == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
		{
			const uint8_t* m = info.wifi_ap_staconnected.mac;
			std::snprintf(d, sizeof(d), "join mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u",
			              m[0], m[1], m[2], m[3], m[4], m[5],
			              (unsigned)info.wifi_ap_staconnected.aid);
			dataManager::logEvent("APSTA", d);
		}
		else if (id == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED)
		{
			const uint8_t* m = info.wifi_ap_stadisconnected.mac;
			std::snprintf(d, sizeof(d), "leave mac=%02X:%02X:%02X:%02X:%02X:%02X aid=%u reason=%u",
			              m[0], m[1], m[2], m[3], m[4], m[5],
			              (unsigned)info.wifi_ap_stadisconnected.aid,
			              (unsigned)info.wifi_ap_stadisconnected.reason);
			dataManager::logEvent("APSTA", d, true);
		}
		else if (id == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)
		{
			std::snprintf(d, sizeof(d), "lease ip=%s",
			              IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
			dataManager::logEvent("APSTA", d);
		}
	}

	// setup から1回呼ぶ(AP起動の前後どちらでもよい)。
	inline void start(void) { WiFi.onEvent(onEvent); }
}
