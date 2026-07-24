#ifndef _BATTERY_GUARD_H_
#define _BATTERY_GUARD_H_
// バッテリ残量の監視と、限界での自動シャットダウン(CoreS3/StickS3 共通)。
//
// 60秒ごとの BATT ログ出力と同じ周期で更新する。レベル判定は batteryLevel.h。
//
// 【電源を切るまでの手順】(ユーザー指示)
//  1. 限界(kMvOff)を kOffConfirmCount 回連続で下回る
//  2. ログに残す(電源断の記録。osfile::append は都度追記なので確実に残る)
//  3. 全セッションを NOCAMERA(✖)にする → スマホのポーリング(約10秒)が1回拾える
//  4. スマホのポーリング1周期ぶん待つ(kNotifyWaitMs)
//  5. 電源を切る
//  確実に通知できなくてよい、という前提なので待ちは1周期ぶんだけ。

#include "batteryLevel.h"
#include "dataManager.h"
#include "holyGrailEntity.h"
#include <cstdio>

namespace batt
{
	// スマホの edgePoll は約10秒間隔(MainActivity.kt)。1周期+余裕を見て待つ。
	constexpr uint32_t kNotifyWaitMs = 12000;

	// 監視の状態。UI側が表示に使う。
	struct guard
	{
		level    lv        = level::full;	// 現在の表示レベル
		int      offStreak = 0;				// 限界電圧を連続で下回った回数
		bool     shutdownRequested = false;	// 電源断シーケンスに入った
		uint32_t shutdownAtMs = 0;			// 電源を切る時刻(millis基準)

		// BATTログと同じ周期(60秒)で呼ぶ。volt<=0(読めない)は前回値を維持する。
		//  戻り値: true = このタイミングで電源断シーケンスを開始した(呼び出し側でログ等を出す)
		bool update(int volt, uint32_t nowMs)
		{
			lv = next(volt, lv);
			if (volt > 0 && volt < current()->mvOff) { ++offStreak; } else { offStreak = 0; }
			if (!shutdownRequested && offStreak >= kOffConfirmCount)
			{
				shutdownRequested = true;
				shutdownAtMs = nowMs + kNotifyWaitMs;
				return true;
			}
			return false;
		}

		// 電源を切ってよい時刻に達したか(loop から毎回呼ぶ)。
		bool readyToPowerOff(uint32_t nowMs) const
		{
			return shutdownRequested && (int32_t)(nowMs - shutdownAtMs) >= 0;
		}
	};

	// 電源断シーケンスの開始処理(ログ + スマホへ✖を見せる)。両機で同じ手順にする。
	inline void beginShutdown(int volt, int pct)
	{
		char d[128];
		std::snprintf(d, sizeof(d),
		              "battery low: volt=%dmV pct=%d → 撮影を止めて電源を切ります(復電後は再開されます)",
		              volt, pct);
		dataManager::logEvent("PWROFF", d, true);	// ERR扱いで目立たせる
		hge_markAllNoCameraForShutdown();			// スマホのポーリングが✖を拾えるようにする
	}
}

#endif // _BATTERY_GUARD_H_
