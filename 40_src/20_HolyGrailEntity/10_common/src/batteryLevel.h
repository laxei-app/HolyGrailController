#ifndef _BATTERY_LEVEL_H_
#define _BATTERY_LEVEL_H_
// エッジ端末のバッテリ残量レベル判定(CoreS3/StickS3 共通)。
//
// 【しきい値の根拠】2026-07-24 に両機をUSB非接続で電池切れまで実測した放電カーブによる。
//   CoreS3 : 4073mV → 3041mV で電源断まで 95分
//   Stick01: 4060mV → 3044mV で電源断まで 89分
//  「その電圧を最後に通過してから電源断までの残り時間」の実測値(平均):
//      3875mV → 63.5分     3725mV → 38.5分     3450mV → 11.0分     3300mV → 3.0分
//  この対応表から、各段のしきい値を「残り時間」で逆算している(下の kMv* を参照)。
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

	// 【段の切り方】「点滅(最後の約10分)を除いた残りを“時間で”3等分」する。
	//  電圧を等間隔に切っても時間は均等にならない(満充電付近はほとんど電圧が下がらず
	//  終盤で急降下する。実測: 3900→3700mV に約30分 / 3400→3040mV は6分)。
	//  そこで実測カーブから「電圧→電源断までの残り分」の表を作り、残り時間で逆算した。
	//
	// 【機種ごとに値を分ける】バッテリ容量も消費電流も違うので、共通の電圧では
	//  どちらかが必ず偏る(実測: 共通値だと CoreS3 33/30/25分 に対し StickS3 18/25/34分)。
	//  機種別にすると両方とも均等になる(下記)。
	struct thresholds
	{
		int mvFull;		// これ以上 = 3/3
		int mvMid;		// これ以上 = 2/3
		int mvLow;		// これ以上 = 1/3 (下回ると点滅)
		int mvOff;		// これを下回り続けたら電源断
		int hystMv;		// 復帰(上がる向き)に必要な上乗せ[mV]
	};

	// M5Stack CoreS3。2026-07-24 実測: 4073mV→3041mV / 95分。
	//  点滅10分を除く85分を3等分(28.4分)して逆算 → 残り66.8分=3894 / 38.4分=3770 / 10分=3463。
	//  検証: 3/3=29分 2/3=29分 1/3=28分 点滅=7分、戻りの往復0回。
	constexpr thresholds kCoreS3 { 3894, 3770, 3463, 3300, 100 };

	// M5StickS3。2026-07-24 実測: 4060mV→3044mV / 89分。
	//  点滅10分を除く79分を3等分(26.4分)して逆算 → 残り62.7分=3856 / 36.4分=3652 / 10分=3366。
	//  検証: 3/3=27分 2/3=27分 1/3=27分 点滅=6分、戻りの往復0回。
	//  ヒステリシスは 100mV だと 3962mV で level4→5 へ1回戻ったため 120mV にした。
	constexpr thresholds kStickS3 { 3856, 3652, 3366, 3300, 120 };

	// 電源断は「連続でこの回数」下回ったら実行する(BATT判定は60秒間隔なので実質1分の確認)。
	//  実測では mvOff を下回ってから実際に電池が尽きるまで両機とも約2分しかない。
	//  3回(=2分)待つと間に合わないので 2回。1回だと瞬間的な電圧降下で誤判定しうるため 2回とする。
	//  mvOff をこれ以上下げると断の判定が電池切れに間に合わない(実測 3150mV で CoreS3 が到達せず)。
	constexpr int kOffConfirmCount = 2;

	// この機種のしきい値。UI(main.cpp)が自機のものを選んで設定する。
	//  既定は CoreS3。StickS3 側は setup() で batt::useThresholds(batt::kStickS3) を呼ぶ。
	inline const thresholds*& current(void) { static const thresholds* p = &kCoreS3; return p; }
	inline void useThresholds(const thresholds& t) { current() = &t; }

	// 電圧[mV]と現在のレベルから次のレベルを求める(ヒステリシス付き)。
	//  cur を level::full 相当で初期化して呼び始めればよい。volt<=0(読めない)は cur を維持。
	inline level next(int volt, level cur)
	{
		if (volt <= 0) { return cur; }
		const thresholds& T = *current();
		const int c = static_cast<int>(cur);
		// 下がる向き: しきい値を下回ったら即座に落とす(安全側)。
		if      (volt <  T.mvOff) { return level::off;   }
		else if (volt <  T.mvLow) { return level::empty; }
		else if (volt <  T.mvMid) { return (c > static_cast<int>(level::low))  ? level::low  : cur; }
		else if (volt <  T.mvFull){ return (c > static_cast<int>(level::mid))  ? level::mid  : cur; }
		// 上がる向き: しきい値 + ヒステリシス を超えるまで戻さない。
		if      (volt >= T.mvFull + T.hystMv) { return level::full; }
		else if (volt >= T.mvMid  + T.hystMv) { return (c < static_cast<int>(level::mid)) ? level::mid : cur; }
		else if (volt >= T.mvLow  + T.hystMv) { return (c < static_cast<int>(level::low)) ? level::low : cur; }
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
