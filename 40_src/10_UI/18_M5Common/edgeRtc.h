#ifndef _EDGE_RTC_H_
#define _EDGE_RTC_H_
// エッジ端末の RTC アクセス(UTC保持運用)のインターフェース。
//
// **このファイルに機種名は出てこない。** 実装は機種ごとに別ファイルで用意する:
//   10_M5Stack/edgeRtc.cpp    … CoreS3。内蔵RTC(M5.Rtc)が必ず在る前提で素直に使う
//   15_M5StickS3/edgeRtc.cpp  … StickS3。内蔵は無い。外付け(Port A の PCF8563/BM8563)を使い、
//                                無ければ RTC 無しとして動く(スマホからの時刻のみで動作)
// どちらをリンクするかは各ターゲットの build_src_filter が決めるので、
// 呼び出し側(etpEdge.cpp)には機種による分岐が一切要らない。

#include <M5Unified.h>

namespace edgeRtc
{
	// 起動時に一度だけ呼ぶ。機種ごとの初期化(外付け探索など)を行う。
	void begin(void);

	// RTC が使えるか。false のときは時刻の保持ができない
	// (スマホからの time コマンドを待つ運用になる)。
	bool available(void);

	// RTC から現在時刻(UTC)を読む。return: 成功(電池切れ・読み取り失敗は false)。
	bool get(m5::rtc_datetime_t& out);

	// RTC へ現在時刻(UTC)を書く。RTC が無い/失敗したときは何もしない。
	void set(const m5::rtc_datetime_t& dt);
}

#endif // _EDGE_RTC_H_
