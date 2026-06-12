// osClock のエッジ端末(M5Stack)実装。
// TZをOSから取得できないため、スマホから受信したUTCオフセットを NVS に永続化して使う。
#include "osClock.h"
#include <Preferences.h>

namespace osclock
{
	namespace
	{
		constexpr const char* NS  = "hgc";		// NVS 名前空間
		constexpr const char* KEY = "tzoff";	// UTCオフセット[分]

		int  g_off    = 0;
		bool g_loaded = false;

		void ensureLoaded(void)
		{
			if (g_loaded) { return; }
			Preferences pref;
			if (pref.begin(NS, true))	// 読み取り専用で開く
			{
				g_off = pref.getInt(KEY, 0);
				pref.end();
			}
			g_loaded = true;
		}
	}

	int utcOffsetMin(void)
	{
		ensureLoaded();
		return g_off;
	}

	void setUtcOffsetMin(int offMin)
	{
		ensureLoaded();
		if (offMin == g_off) { return; }	// 変化なしなら書き込まない(NVS摩耗を避ける)
		g_off = offMin;
		Preferences pref;
		if (pref.begin(NS, false))			// 書き込み可で開く
		{
			pref.putInt(KEY, offMin);
			pref.end();
		}
	}
}
