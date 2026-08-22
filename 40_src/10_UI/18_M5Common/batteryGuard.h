#ifndef _BATTERY_GUARD_H_
#define _BATTERY_GUARD_H_
// バッテリ残量の監視と、限界での自動シャットダウン(エッジ端末のUI共通部)。
// **このファイルに機種名は出てこない。** 機種ごとに違う値は batteryParams.h(各機種フォルダ)。
//
// 「限界に達したか」の判定(offJudge)は**機種ごとに別実装**とする:
//   10_M5Stack/batteryGuard.cpp    … CoreS3。連続 offConfirm 回下回りで断(素直な電圧降下向け)
//   15_M5StickS3/batteryGuard.cpp  … StickS3。ラッチ式+即断フロア(撮影負荷のサグ/バウンス対応)
// どちらをリンクするかは各ターゲットの build_src_filter(src_dir)が決めるので、
// 呼び出し側(main.cpp)にも本ヘッダにも機種による分岐は無い(edgeRtc と同じ方式)。
//
// 60秒ごとの BATT ログ出力と同じ周期で更新する。レベル判定は batteryLevel.h。
//
// 【電源を切るまでの手順】(ユーザー指示)
//  1. 限界に達したと offJudge が判定する(判定条件は機種別)
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

	// 「限界に達した=電源断を開始せよ」の判定。**機種別実装**(各機種フォルダの batteryGuard.cpp)。
	//  60秒周期で毎回呼ばれる。判定に使う内部状態(連続回数/ラッチ等)は実装側が持つ。
	//  volt<=0(読めない)は状態を変えず false を返すこと。
	bool offJudge(int volt);

	// 監視の状態。UI側が表示に使う。
	struct guard
	{
		level    lv        = level::full;	// 現在の表示レベル
		bool     shutdownRequested = false;	// 電源断シーケンスに入った
		uint32_t shutdownAtMs = 0;			// 電源を切る時刻(millis基準)

		// BATTログと同じ周期(60秒)で呼ぶ。volt<=0(読めない)は前回値を維持する。
		//  戻り値: true = このタイミングで電源断シーケンスを開始した(呼び出し側でログ等を出す)
		bool update(int volt, uint32_t nowMs)
		{
			lv = next(volt, lv);
			if (!shutdownRequested && offJudge(volt))
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

	// 電源断シーケンスの開始処理(ログ + スマホへ✖を見せる)。機種によらず同じ手順。
	inline void beginShutdown(int volt, int pct)
	{
		char d[128];
		std::snprintf(d, sizeof(d),
		              "battery low: volt=%dmV pct=%d -> stopping capture and powering off (resumes after recharge)",
		              volt, pct);
		dataManager::logEvent("PWROFF", d, true);	// ERR扱いで目立たせる
		hge_markAllNoCameraForShutdown();			// スマホのポーリングが✖を拾えるようにする
	}
}

#endif // _BATTERY_GUARD_H_
