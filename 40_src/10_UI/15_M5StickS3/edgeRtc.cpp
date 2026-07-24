// M5StickS3 の RTC 実装。
//
// **StickS3 は内蔵RTCを持たない。** 外付けRTCユニット(Port A / Ex_I2C の
// PCF8563・BM8563 = I2C 0x51)が挿さっていればそれを使い、無ければ RTC 無しとして動く
// (その場合はスマホから受けた時刻のみで動作する = 自前で計算して保持する)。
//
// M5Unified の M5.Rtc.begin(&Ex_I2C) は PCF8563 を生成するとき i2c を渡さず内部バス既定の
// ままになるため、外付けを拾えない。そこで外付けは自前のインスタンスで持つ。
// 撮影中に外れた場合も、読み書きの失敗を検出して以後は無効として扱う。

#include "edgeRtc.h"
#include "debugOut.h"
// 外付けRTC(Port A の PCF8563/BM8563)を自前で扱うため。
#include <utility/rtc/PCF8563_Class.hpp>

namespace
{
	m5::PCF8563_Class g_extRtc;			// 外付けRTC(Ex_I2C)。使えるときだけ有効化する
	bool              g_extRtcOk = false;
}

namespace edgeRtc
{
	void begin(void)
	{
		M5.Ex_I2C.begin();
		// begin(&Ex_I2C) で内部の I2C 参照を外部バスへ差し替えたうえで初期化する。
		g_extRtcOk = g_extRtc.begin(&M5.Ex_I2C);
		DBGLN(g_extRtcOk ? col::GRN : col::YEL, "edgeRtc: external RTC %s",
		      g_extRtcOk ? "detected (Ex_I2C 0x51)" : "not found (use phone time only)");
	}

	bool available(void)
	{
		return g_extRtcOk;
	}

	bool get(m5::rtc_datetime_t& out)
	{
		if (!g_extRtcOk) { return false; }
		if (g_extRtc.getVoltLow())
		{
			DBGLN(col::YEL, "edgeRtc: external RTC volt-low (not kept) -> wait time cmd");
			return false;
		}
		if (!g_extRtc.getDateTime(&out.date, &out.time))
		{
			g_extRtcOk = false;
			DBGLN(col::YEL, "edgeRtc: external RTC lost (read failed)");
			return false;
		}
		return true;
	}

	void set(const m5::rtc_datetime_t& dt)
	{
		if (!g_extRtcOk) { return; }
		if (!g_extRtc.setDateTime(&dt.date, &dt.time))
		{
			g_extRtcOk = false;
			DBGLN(col::YEL, "edgeRtc: external RTC lost (write failed)");
		}
	}
}
