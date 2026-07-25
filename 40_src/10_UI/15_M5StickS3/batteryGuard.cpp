// M5StickS3 の電源断判定(batt::offJudge)。
//
// **この機体は「連続 N 回下回り」では断が遅れる。** 電池が小さく撮影の電流スパイクで
// 電圧が深く沈む/戻る(サグ・バウンス)ため、2026-07-25 の撮影中実放電では mvOff 付近を
// 3320→3284→3314→3222 と上下して連続条件がリセットされ、3080mV まで落ちてからの断になった。
// 劣化・低温・複数台撮影でさらに悪化し、ハード保護(ブラウンアウト)に先に切られる恐れがある。
//
// そこで判定を2段構えにする:
//  1) ラッチ式カウント: mvOff を下回ったら数え、**mvOff+hystMv まで戻らない限り数を保持**する
//     (瞬間的なバウンスではリセットされない。USB給電などで本当に回復したときだけリセット)。
//     累計 offConfirm 回で断。
//  2) 即断フロア: mvFloor を1回でも下回ったら確認を待たずに断
//     (深いサグ=残量が尽きる寸前。60秒後の次判定を待つ余裕は無い)。

#include "batteryGuard.h"

namespace batt
{
	bool offJudge(int volt)
	{
		static int lowCount = 0;	// mvOff を下回った累計(ラッチ式)
		if (volt <= 0) { return false; }	// 読めない → 状態を変えない
		if (volt < kParams.mvFloor) { return true; }	// 即断フロア
		if      (volt <  kParams.mvOff)                   { ++lowCount; }
		else if (volt >= kParams.mvOff + kParams.hystMv)  { lowCount = 0; }	// 本当に回復したときだけリセット
		/* mvOff〜mvOff+hystMv の帯はバウンスとみなし数を保持 */
		return lowCount >= kParams.offConfirm;
	}
}
