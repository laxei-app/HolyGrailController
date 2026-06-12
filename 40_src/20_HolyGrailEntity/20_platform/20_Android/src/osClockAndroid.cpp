// osClock の Android 実装。端末のタイムゾーンを OS(libc)から取得する。
#include "osClock.h"
#include <ctime>

namespace osclock
{
	int utcOffsetMin(void)
	{
		time_t t = std::time(nullptr);
		std::tm lt{};
		localtime_r(&t, &lt);
		// bionic は tm_gmtoff(UTCからの秒・東が正) を埋める。
		return static_cast<int>(lt.tm_gmtoff / 60);
	}

	// 端末TZが権威なので保存は不要(受信値は無視)。
	void setUtcOffsetMin(int /*offMin*/) {}
}
