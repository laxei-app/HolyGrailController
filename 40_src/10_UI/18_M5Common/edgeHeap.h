#pragma once

// 内部RAM(DRAM)の推移をログへ残す(2026-08-23)。
//
// 【なぜ要るか】撮影を停止して開始し直すと、3回目あたりで撮影スレッドの生成が通らなくなる
//  (`start thread create failed` を10秒ごとに繰り返す。2026-08-23 実機で発生)。原因は内部RAMの
//  枯渇か断片化だが、起動時の1行(BOOT の heapKB)しか記録が無いため、どの操作でどれだけ減り、
//  停止でどれだけ戻るのかが分からない。開始/停止のたびに残せば、戻らない量がそのまま漏れになる。
//
// 【何を残すか】タスクスタックは内部RAMの**連続領域**が要るので、空き総量だけでは足りない。
//  ・free    … 内部RAMの空き総量
//  ・largest … 最大の連続空きブロック。xTaskCreate(既定14336B)はここが足りないと失敗する
//  ・min     … 起動してからの空きの最小値(どこまで細ったかの水位)
//
// 置き場所は 18_M5Common。CoreS3 も StickS3 も同じものを使う(機種で分岐しない)。
#include <esp_heap_caps.h>
#include <cstdio>
#include "dataManager.h"

namespace edgeHeap
{
	inline void log(const char* tag)
	{
		char d[96];
		std::snprintf(d, sizeof(d), "%s free=%u largest=%u min=%u",
		              (tag && tag[0]) ? tag : "-",
		              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
		              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
		dataManager::logEvent("HEAP", d);
	}

	// 毎ループから呼ぶ。状態が変わったときと、60秒ごとに1行残す。
	//  状態変化のたびに出すのは、開始(SEARCHING→CAPTURING)と停止(→IDLE)の前後を突き合わせるため。
	inline void pump(int state)
	{
		static int      lastState = -1;
		static uint32_t lastMs    = 0;
		const uint32_t  now       = millis();
		if (state != lastState)
		{
			char tag[24];
			std::snprintf(tag, sizeof(tag), "state=%d", state);
			lastState = state; lastMs = now;
			log(tag);
			return;
		}
		if (now - lastMs >= 60000) { lastMs = now; log("periodic"); }
	}
}
