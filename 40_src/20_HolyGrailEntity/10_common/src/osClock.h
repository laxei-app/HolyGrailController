#ifndef _OS_CLOCK_H_
#define _OS_CLOCK_H_
// プラットフォーム非依存の「現在のUTCオフセット[分]」抽象。
// 天文計算(astroSched)・固定計画の生成・ログのタイムスタンプで使う現地時刻の基準。
// タイムゾーンの取得元はプラットフォームごとに異なるためここで吸収する:
//   - Android : 端末のタイムゾーン(OSから取得)。
//   - エッジ端末(M5Stack) : RTC等からTZは得られないため、スマホから ETP の time コマンドで
//                            受信したオフセットを永続化(NVS)して以後も使う。
// 実装は各プラットフォーム(20_platform/*/src/osClock*.cpp)に置く。

namespace osclock
{
	// 現在のUTCオフセット[分](東が正。日本= +540)。取得できなければ 0。
	int utcOffsetMin(void);

	// UTCオフセット[分]を設定・永続化する。
	// エッジ端末が time コマンド受信時に呼ぶと、次回起動後もこの値を使う。
	// 端末TZをOSから取得できるプラットフォーム(Android等)では無視してよい。
	void setUtcOffsetMin(int offMin);
}

#endif // _OS_CLOCK_H_
