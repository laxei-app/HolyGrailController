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
#include "osSystemCall.h"	// 生きているスレッドの高水位を一緒に残す

namespace edgeHeap
{
	// 内部RAMを使う上限を下げ、これを超える malloc を PSRAM へ回す(2026-08-23)。
	//
	// 【なぜ必要か】この機体は PSRAM が 8.3MB 丸々空いている一方で、内部RAM は撮影中に 53KB、
	//  セッション確立の一瞬は 18KB まで細る。タスクスタックは内部RAM にしか置けないので、
	//  そこを他の確保で埋めてしまうと撮影スレッドが作れなくなる(2026-08-23 に実際に発生)。
	//
	// 【既定との違い】framework の既定は 4096(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)。
	//  つまり 4KB 超は元々 PSRAM へ行っている。下げると 1〜4KB の中型の確保も外へ出せる。
	//  小さすぎる値にすると細かい確保が全部低速な PSRAM へ行き遅くなるので、欲張らない。
	//  DMA が要るバッファや Wi-Fi/lwIP は MALLOC_CAP_DMA / 専用ヒープを明示するので影響を受けない。
	inline void useExternalAbove(size_t limitBytes)
	{
		heap_caps_malloc_extmem_enable(limitBytes);
	}

	inline void log(const char* tag)
	{
		char d[160];
		// 内部RAM(タスクスタックに必須)と PSRAM(大きな一時確保の逃げ先)を両方出す。
		std::snprintf(d, sizeof(d), "%s free=%u largest=%u min=%u ps=%u psLargest=%u",
		              (tag && tag[0]) ? tag : "-",
		              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
		              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
		              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
		              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
		              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
		dataManager::logEvent("HEAP", d);
	}

	// 毎ループから呼ぶ。状態が変わったときと、60秒ごと、それに
	// **空きが 4KB 以上動いたとき**に1行残す(2026-08-23 追加)。
	//  確立処理の山は数秒で過ぎるので、60秒周期だと「いつ何で落ちたか」が残らない。
	//  変化で拾えば、前後の NET/CONV の行と突き合わせて犯人を絞れる。
	//  1秒に1行を上限にしてログが溢れないようにする。
	//  状態変化のたびに出すのは、開始(SEARCHING→CAPTURING)と停止(→IDLE)の前後を突き合わせるため。
	inline void pump(int state)
	{
		static int      lastState = -1;
		static uint32_t lastMs    = 0;
		static size_t   lastFree    = 0;
		static uint32_t lastDeltaMs = 0;
		const uint32_t  now       = millis();
		if (state != lastState)
		{
			char tag[24];
			std::snprintf(tag, sizeof(tag), "state=%d", state);
			lastState = state; lastMs = now;
			log(tag);
			return;
		}
		if (now - lastMs >= 60000) { lastMs = now; lastFree = 0; log("periodic"); ossc::logLiveThreads(); return; }
		// 大きく動いたら即時に残す(確立の山を見逃さないため)。
		const size_t nowFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
		if (lastFree == 0) { lastFree = nowFree; return; }
		const size_t diff = (nowFree > lastFree) ? (nowFree - lastFree) : (lastFree - nowFree);
		if (diff >= 4096 && (now - lastDeltaMs) >= 1000)
		{
			const char* tag = (nowFree < lastFree) ? "drop" : "rise";
			lastDeltaMs = now; lastFree = nowFree;
			log(tag);
		}
		else if (diff >= 4096) { lastFree = nowFree; }
	}
}
