// M5Stack CoreS3 の RTC 実装。
//
// **CoreS3 は内蔵RTC(BM8563)を必ず持つ**ので、「RTC が在るか」の判定はしない。
// 読み書きできないならハードウェアの故障であり、分岐で吸収すべきものではない。
//  (電池切れで時刻を保持できていない状態だけは getVoltLow() で判別できるので、
//   それは「読めなかった」= get() が false として扱う。)

#include "edgeRtc.h"
#include "debugOut.h"

namespace edgeRtc
{
	void begin(void)
	{
		// 内蔵RTCは M5.begin() で初期化済み。ここで行うことは無い。
		DBGLN(col::GRN, "edgeRtc: internal RTC (built-in)");
	}

	bool available(void)
	{
		return true;	// 内蔵RTCは必ず在る(無ければハード故障)
	}

	bool get(m5::rtc_datetime_t& out)
	{
		if (M5.Rtc.getVoltLow()) { return false; }	// 電池切れ等で時刻を保持できていない
		out = M5.Rtc.getDateTime();
		return true;
	}

	void set(const m5::rtc_datetime_t& dt)
	{
		M5.Rtc.setDateTime(dt);
	}
}
