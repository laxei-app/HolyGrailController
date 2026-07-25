// M5Stack CoreS3 の電源断判定(batt::offJudge)。
//
// CoreS3 は電池が大きく末期の電圧降下が素直(2026-07-25 撮影中実測: 3300 を割ってから
// 3199mV で整然と断)。従来どおり「連続 offConfirm 回下回り」で十分に働く。
//  ・1回だけの瞬間的な落ち込み(誤検出)は連続条件で弾く
//  ・閾値以上へ戻ったら数え直し(この機体はバウンスがほぼ無いのでリセットでよい)

#include "batteryGuard.h"

namespace batt
{
	bool offJudge(int volt)
	{
		static int streak = 0;	// 限界電圧を連続で下回った回数
		if (volt <= 0) { return false; }	// 読めない → 状態を変えない
		if (volt < kParams.mvOff) { ++streak; } else { streak = 0; }
		return streak >= kParams.offConfirm;
	}
}
