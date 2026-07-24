#ifndef _BATTERY_LEVEL_H_
#define _BATTERY_LEVEL_H_
// バッテリ残量レベルの判定ロジック(エッジ端末のUI共通部)。
//
// **このファイルに機種名は出てこない。** 機種ごとに違うもの(しきい値・ヒステリシス・
// 電源断の確認回数)は、各機種のフォルダに置く batteryParams.h の batt::kParams で与える。
// どの batteryParams.h を拾うかはビルドのインクルードパスが決めるので、
// 機種が増えてもこのファイルは変更しない。
//
// 【段の切り方の考え方】
//  「点滅(最後の約10分)を除いた残りを“時間で”3等分」する。
//  電圧を等間隔に切っても時間は均等にならない(満充電付近はほとんど電圧が下がらず
//  終盤で急降下するため)。各機種の実測放電カーブから「電圧→電源断までの残り分」を
//  作り、残り時間で逆算した値を batteryParams.h に置いている。
//
// 【pct ではなく電圧で判定する理由】
//  同じ残り時間の地点でも pct は機体差が大きく基準にできない(実測で 38% と 8%)。
//  電圧の方が素直に効くので電圧を主、pct は参考に留める。
//
// 【ヒステリシス】
//  電圧は負荷変動で容易に上下するので、そのままでは境目でアイコンがちらつく。
//   ・下がる向き: しきい値を下回ったら即座に下のレベルへ(安全側=早めに警告)
//   ・上がる向き: しきい値 + hystMv を上回るまで戻さない
//  充電(USB接続)で電圧が戻る場合もこの規則で滑らかに上がる。

#include <cstdint>
#include "batteryParams.h"	// 機種ごとの実測値(各機種フォルダに置く)

namespace batt
{
	// 表示レベル。level5=満充電(3/3) … level2=0/3(点滅) / level1=電源断。
	enum class level : uint8_t
	{
		off   = 1,	// level1: 電源を切る
		empty = 2,	// level2: 0/3 点滅
		low   = 3,	// level3: 1/3
		mid   = 4,	// level4: 2/3
		full  = 5,	// level5: 3/3
	};

	// 電圧[mV]と現在のレベルから次のレベルを求める(ヒステリシス付き)。
	//  cur を level::full 相当で初期化して呼び始めればよい。volt<=0(読めない)は cur を維持。
	inline level next(int volt, level cur)
	{
		if (volt <= 0) { return cur; }
		const int c = static_cast<int>(cur);
		// 下がる向き: しきい値を下回ったら即座に落とす(安全側)。
		if      (volt <  kParams.mvOff) { return level::off;   }
		else if (volt <  kParams.mvLow) { return level::empty; }
		else if (volt <  kParams.mvMid) { return (c > static_cast<int>(level::low)) ? level::low : cur; }
		else if (volt <  kParams.mvFull){ return (c > static_cast<int>(level::mid)) ? level::mid : cur; }
		// 上がる向き: しきい値 + ヒステリシス を超えるまで戻さない。
		if      (volt >= kParams.mvFull + kParams.hystMv) { return level::full; }
		else if (volt >= kParams.mvMid  + kParams.hystMv) { return (c < static_cast<int>(level::mid)) ? level::mid : cur; }
		else if (volt >= kParams.mvLow  + kParams.hystMv) { return (c < static_cast<int>(level::low)) ? level::low : cur; }
		return cur;
	}

	// アイコンで塗る段数(0〜3)。level1(off)は 0。
	inline int bars(level l)
	{
		switch (l)
		{
			case level::full:  return 3;
			case level::mid:   return 2;
			case level::low:   return 1;
			default:           return 0;
		}
	}
}

#endif // _BATTERY_LEVEL_H_
