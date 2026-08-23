#ifndef _BATTERY_PARAMS_H_
#define _BATTERY_PARAMS_H_
// M5Stack Core2 のバッテリ値。18_M5Common/batteryLevel.h から参照される。
//
// 【暄定値。実測して入れ直すこと】
//  下の値は CoreS3 の実測値をそのまま置いている。リチウムイオンなので**電圧の絶対値は近い**が、
//  Core2 は電池容量(390mAh)も消費電流も CoreS3(500mAh)と違うため、**段の平等さと残り時間は合わない**。
//  正しい値は「USB非接続で電池切れまで放置し、BATT ログの放電カーブから逆算する」で決める
//  (CoreS3 と StickS3 もその手順で決めた。推測で確定させないこと)。
//
//  とくに mvOff は「下回ってから尽きるまでの余裕」が機種で違う。Core2 は容量が小さい分だけ
//  末期が早い可能性があるので、実測までは offConfirm を CoreS3 と同じ 2 回(=2分)のままにしている。

namespace batt
{
	struct params
	{
		int mvFull;		// これ以上 = 3/3
		int mvMid;		// これ以上 = 2/3
		int mvLow;		// これ以上 = 1/3 (下回ると点滅)
		int mvOff;		// これを下回り続けたら電源断
		int hystMv;		// 復帰(上がる向き)に必要な上乗せ[mV]
		int offConfirm;	// 電源断は連続でこの回数下回ったら実行(BATT判定は60秒間隔)
	};

	// 暄定(CoreS3 の実測値を流用)。実測後に差し替える。
	constexpr params kParams { 3894, 3770, 3600, 3300, 100, 2 };
}

#endif // _BATTERY_PARAMS_H_
