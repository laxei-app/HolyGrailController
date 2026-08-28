#pragma once
// タスクが止まったら、黙って死なせずに証拠を残して再起動する(2026-08-28)。
//
// 【なぜ要るか】実機で端末が完全に固まった。画面も時計も止まり、シリアルに1バイトも
//  出ず、スマホからも見えない。ところが**撮影だけは続いていた**。あとで分かったのは、
//  UI・シリアル・BLE・ETP・ファイル保存はすべて loop()(loopTask)が抱えており、
//  撮影は別タスク(ossNet)だということ。つまり loopTask 1本が止まると、外から見て
//  「死んでいるのに撮っている」という状態になる。
//  このときバックトレースが一切残らず、原因を突き止められなかった。
//
// 【仕掛け】loop() が毎周 beat() を打つ。一定時間打たれなければ、その事実をログへ残し、
//  **わざとウォッチドッグを踏んで panic させる**。この枠組みは既に有効になっている:
//    CONFIG_ESP_TASK_WDT_PANIC=y          発火したら panic(警告printfで済まさない)
//    CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y  panic 時に全タスクのスタックをフラッシュへ
//    CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=64
//  区画(coredump, 0x7F0000, 64KB)も既にある。あとから USB で吸い出せば、**待っていた
//  タスクと相手**が分かる。シリアルに何も出なくても残るのが要点。
//
// 【読み出し】そのファームの ELF が要る(SHA が一致しないと読めない)。ビルドのたびに
//  .pio/build/debug/elf/hgc-<版数>.elf へ控えている(archive_elf.py)。
//    pip install esp-coredump
//    esptool.py --port COMx read_flash 0x7F0000 0x10000 cd.bin
//    python -m esp_coredump --chip esp32s3 info_corefile --core cd.bin --core-format raw <elf>
//
// 【誤発火をさせないこと】loop() は普段1周が数ミリ秒だが、計画の受信や掃除で秒単位に
//  なることがある。閾値は「正常なら絶対に届かない」値にする。撮影を巻き込んで再起動
//  させるのが一番まずいので、疑わしきは待つ側へ倒す。
//
// 置き場所は 18_M5Common。CoreS3 も StickS3 も同じものを使う(機種で分岐しない)。
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "dataManager.h"

namespace edgeAlive
{
	// loop() が固まったと見なすまでの時間。
	//  実測の最長は計画受信まわりの数秒。30秒あれば正常では絶対に届かない。
	constexpr uint32_t kStallMs = 30000;

	inline uint32_t& lastBeat(void) { static uint32_t v = 0; return v; }
	inline bool&     armed(void)    { static bool b = false; return b; }

	// loop() の頭で毎周呼ぶ。
	inline void beat(void) { lastBeat() = millis(); }

	// 見張り本体(前方宣言)。実体は下。
	inline void check(void);

	// setup の最後で1回呼ぶ。見張り役の小さなタスクを1本立てる。
	//  **loop() から見張ってはいけない**(loop が止まったら誰も見なくなる)。
	inline void start(void)
	{
		lastBeat() = millis();
		armed()    = true;
		TaskHandle_t h = nullptr;
		// 2.5KB。判定しかしないので小さくてよい(内部RAMは貴重)。優先度は低め。
		const BaseType_t r = xTaskCreatePinnedToCore(
			[](void*) { for (;;) { check(); vTaskDelay(pdMS_TO_TICKS(2000)); } },
			"alive", 2560, nullptr, 1, &h, 1);
		if (r != pdPASS)
		{	// 立てられなくても本業は続ける。ただし黙って諦めない(次に固まっても分からないため)。
			armed() = false;
			dataManager::logEvent("ALIVE", "watchdog task not created (out of internal RAM)", true);
		}
	}

	// 見張り本体。**loop() ではなく別タスクから呼ぶこと**(loop が止まったら loop からは
	//  呼ばれないため)。撮影スレッドを巻き込まないよう、判定だけして自分では何もしない。
	inline void check(void)
	{
		if (!armed()) { return; }
		const uint32_t ms = millis();
		if (ms - lastBeat() < kStallMs) { return; }
		armed() = false;			// 二度は撃たない
		// まずログへ残す。ファイル層が生きていれば、これだけでも次に読める。
		char d[96];
		std::snprintf(d, sizeof(d), "loop stalled %ums -> forcing watchdog for a core dump",
		              (unsigned)(ms - lastBeat()));
		dataManager::logEvent("ALIVE", d, true);
		// わざとウォッチドッグを踏む。この関数は戻らない(panic → コアダンプ → 再起動)。
		//  自分をウォッチドッグへ登録し、餌をやらずに居座る。
		esp_task_wdt_add(nullptr);
		for (;;) { /* 餌をやらない */ }
	}
}
