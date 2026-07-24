#ifndef _BATTERY_LEVEL_H_
#define _BATTERY_LEVEL_H_
// エッジ端末のバッテリ残量レベル判定(CoreS3/StickS3 共通)。
//
// 【しきい値の根拠】2026-07-24 に両機をUSB非接続で電池切れまで実測した放電カーブによる。
//   CoreS3 : 4073mV → 3041mV で電源断まで 95分
//   Stick01: 4060mV → 3044mV で電源断まで 89分
//  「その電圧を最後に通過してから電源断までの残り時間」の実測値:
//      3600mV → CoreS3 21分 / Stick01 31分
//      3500mV → CoreS3 13分 / Stick01 20分
//      3470mV → CoreS3 10分 / Stick01 16分   ← 点滅(level2)の開始はここ
//      3400mV → CoreS3  6分 / Stick01  9分
//  要望「点滅してから残り10分ほど」に対し、実測ログを流したシミュレーションで
//  3470mV が CoreS3=10分 / Stick01=16分。CoreS3 が要望どおりで Stick01 は余裕を持つ側になる
//  (足りないより余る方が安全なのでこの値を採る)。
//
// 【pct ではなく電圧で判定する理由】
//  同じ残り10分の地点で pct は CoreS3=38% / Stick01=8% と機体差が大きく、基準にできない。
//  電圧は両機でよく一致する(上表)。よって電圧を主、pct は参考に留める。
//
// 【ヒステリシス】
//  電圧は負荷変動で ±50mV 程度は容易に上下する(実測でも 3366→3406→3358mV と往復した)。
//  そのままでは境目でアイコンがちらつくので、
//   ・下がる向き: しきい値を下回ったら即座に下のレベルへ(安全側=早めに警告)
//   ・上がる向き: しきい値 + kHystMv を上回るまで戻さない
//  とする。充電(USB接続)で電圧が上がる場合もこの規則で滑らかに戻る。
//
// 【level1(電源断)について】
//  3300mV は実測で残り3分。ここまで来たら安全に自ら電源を切る。
//  ただし電圧の一時的な落ち込みで誤って切らないよう、判定側で「連続して」下回ることを要求する
//  (kOffConfirmCount)。

#include <cstdint>

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

	// しきい値[mV](下がる向きで「この値を下回ったら」下のレベルへ)。
	// 実測の残り時間: 3900→CoreS3 68分 / 3700→29分 / 3450→9分 / 3300→3分。
	constexpr int kMvFull  = 3900;	// これ以上 = 3/3
	constexpr int kMvMid   = 3700;	// これ以上 = 2/3
	constexpr int kMvLow   = 3470;	// これ以上 = 1/3  (下回ると点滅=残り約10分)
	constexpr int kMvOff   = 3300;	// これを下回り続けたら電源断(実測 残り約3分)

	// 復帰(上がる向き)に必要な上乗せ[mV]。実測ログを流して決めた:
	//  60mV では Stick01 が 3858→3962→3830mV と揺れて level5⇄level4 を1往復した。
	//  100mV にすると両機とも戻りの往復が 0 回になる。
	constexpr int kHystMv  = 100;
	// 電源断は「連続でこの回数」下回ったら実行する(BATT判定は60秒間隔なので実質1分の確認)。
	//  実測では kMvOff を下回ってから実際に電池が尽きるまで CoreS3/Stick01 とも約2分しかない。
	//  3回(=2分)待つと間に合わないので 2回。1回だと瞬間的な電圧降下で誤判定しうるため 2回とする。
	constexpr int kOffConfirmCount = 2;

	// 電圧[mV]と現在のレベルから次のレベルを求める(ヒステリシス付き)。
	//  cur を level::full 相当で初期化して呼び始めればよい。volt<=0(読めない)は cur を維持。
	inline level next(int volt, level cur)
	{
		if (volt <= 0) { return cur; }
		const int c = static_cast<int>(cur);
		// 下がる向き: しきい値を下回ったら即座に落とす(安全側)。
		if      (volt <  kMvOff) { return level::off;   }
		else if (volt <  kMvLow) { return level::empty; }
		else if (volt <  kMvMid) { return (c > static_cast<int>(level::low))  ? level::low  : cur; }
		else if (volt <  kMvFull){ return (c > static_cast<int>(level::mid))  ? level::mid  : cur; }
		// 上がる向き: しきい値 + ヒステリシス を超えるまで戻さない。
		if      (volt >= kMvFull + kHystMv) { return level::full; }
		else if (volt >= kMvMid  + kHystMv) { return (c < static_cast<int>(level::mid)) ? level::mid : cur; }
		else if (volt >= kMvLow  + kHystMv) { return (c < static_cast<int>(level::low)) ? level::low : cur; }
		return cur;
	}

	// 満充電側の 3/3 表示に使う「塗る段数」(0〜3)。level1(off)は 0。
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
