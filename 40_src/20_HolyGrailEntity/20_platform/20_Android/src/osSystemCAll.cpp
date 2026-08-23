// osSystemCall.cpp
// Android で OS 依存のシステムコールを集約する。std::thread を使用。

#include "osSystemCall.h"
#include <thread>

namespace ossc
{
	// スレッドを起動する。
	// return : スレッドを破棄するためのハンドル
	void* threadNet(THREAD_FUNC& func, void* parm, uint32_t /*stackBytes*/, bool /*useStaticPool*/)
	{	// std::thread は既定スタック(数MB)を使うため stackBytes は無視(ESP32専用の調整)。
		// useStaticPool も ESP32 専用。スマホは内部RAMの断片化でスレッド生成が失敗する問題が無い。
		auto thread = new std::thread(func, parm);
		return thread;
	}

	// 生きているスレッドのスタック使用量をログへ出す(ESP32専用の計測)。
	//  スマホは std::thread の既定スタック(数MB)で、削る動機が無いので何もしない。
	size_t internalFree(void) { return 0; }	// スマホには内部RAMの制約が無い
	size_t internalMinFree(void) { return 0; }
	void logLiveThreads(void) {}

	// スレッドの終了を待ってスレッドを破棄する。
	void threadEnd(void* handle)
	{
		if (!handle) { return; }

		auto threadPtr = static_cast<std::thread*>(handle);
		if (threadPtr->joinable())
		{
			threadPtr->join();
		}
		delete threadPtr;
	}
}
