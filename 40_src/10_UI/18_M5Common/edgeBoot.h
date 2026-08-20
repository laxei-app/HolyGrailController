#pragma once

// 起動したことと、その理由をログに1行残す(2026-08-21)。
//
// 【なぜ要るか】8月20日の記録を見ると、15:47〜21:14 のあいだ**約9分ごとに再起動**を
//  繰り返していた(バッテリ記録が up=5s → up=305s → 消える、の繰り返し)。その日は
//  一度も撮影できていない。ところがログには「落ちた」ことも「なぜ落ちたか」も残って
//  いないため、電源断なのか、ウォッチドッグなのか、電圧低下なのかを後から区別できない。
//  起動のたびに理由を残しておけば、次に起きたときログを見るだけで分かる。
//
// 【何を残すか】
//  ・reason … ESP32 が持っている前回のリセット要因。これが本命。
//              POWERON=電源投入 / EXT=外部リセット / SW=ソフトからの再起動(書き込み後など)
//              PANIC=例外で落ちた / INT_WDT・TASK_WDT=ウォッチドッグ / BROWNOUT=電圧低下
//  ・heap  … 起動直後の空きメモリ。断片化や枯渇で落ちているなら、ここが痩せていく
//  ・行の時刻そのもの … 起動時点で時計が何時だと思っているかが残る。時計の出どころが
//              おかしいとき(記録が別の日のファイルへ紛れ込む等)の手掛かりになる。
//
// 置き場所は 18_M5Common。CoreS3 も StickS3 も同じものを使う(機種で分岐しない)。
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include "dataManager.h"

namespace edgeBoot
{
	inline const char* resetReasonName(void)
	{
		switch (esp_reset_reason())
		{
		case ESP_RST_POWERON:   return "POWERON";	// 電源投入
		case ESP_RST_EXT:       return "EXT";		// 外部リセット端子
		case ESP_RST_SW:        return "SW";		// ソフトからの再起動(書き込み後/モード切替)
		case ESP_RST_PANIC:     return "PANIC";		// 例外で落ちた
		case ESP_RST_INT_WDT:   return "INT_WDT";	// 割り込みウォッチドッグ
		case ESP_RST_TASK_WDT:  return "TASK_WDT";	// タスクウォッチドッグ(処理が返らない)
		case ESP_RST_WDT:       return "WDT";		// その他のウォッチドッグ
		case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
		case ESP_RST_BROWNOUT:  return "BROWNOUT";	// 電圧低下(電池/配線を疑う)
		case ESP_RST_SDIO:      return "SDIO";
		default:                return "UNKNOWN";
		}
	}

	// 起動時に1回だけ呼ぶ。時計とログの保存先が使える状態になってから呼ぶこと
	//  (時刻が入らないと、いつの起動か分からなくなる)。
	inline void logMarker(const char* netMode, const char* version)
	{
		char d[128];
		std::snprintf(d, sizeof(d), "reason=%s mode=%s ver=%s heapKB=%u minKB=%u",
		              resetReasonName(),
		              (netMode && netMode[0]) ? netMode : "?",
		              (version && version[0]) ? version : "?",
		              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
		              (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
		dataManager::logEvent("BOOT", d);
	}
}
