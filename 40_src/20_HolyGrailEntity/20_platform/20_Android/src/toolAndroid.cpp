// Android 用 tool の時間管理系実装(文字列解析系は共通 tool.cpp)。

#include "common.h"
#include "tool.h"
#include "commonAndroid.h"

#include <ctime>
#include <cstdint>

namespace
{
	// 起動からの経過ミリ秒(単調増加)
	uint32_t monoMs(void)
	{
		timespec ts{};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return static_cast<uint32_t>(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
	}
}

// 経過時間の計測を開始する。handle は getElapse に渡す。
void* tool::startElapse(void)
{
	return reinterpret_cast<void*>(static_cast<uintptr_t>(monoMs()));
}

// startElapse からの経過ミリ秒を取得する。
uint32_t tool::getElapse(void* handle)
{
	return monoMs() - static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
}

// 指定ミリ秒待つ。
void tool::sleep(uint32_t ms)
{
	timespec ts{};
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = static_cast<long>((ms % 1000) * 1000000UL);
	nanosleep(&ts, nullptr);
}

void tool::memoryInfo(void)
{
	__android_log_print(ANDROID_LOG_INFO, HGE_LOG_TAG, "memoryInfo: (not implemented)");
}
