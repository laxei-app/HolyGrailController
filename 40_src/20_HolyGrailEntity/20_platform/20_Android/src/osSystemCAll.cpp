// osSystemCall.cpp
// Android で OS 依存のシステムコールを集約する。std::thread を使用。

#include "osSystemCall.h"
#include <thread>

namespace ossc
{
	// スレッドを起動する。
	// return : スレッドを破棄するためのハンドル
	void* threadNet(THREAD_FUNC& func, void* parm, uint32_t /*stackBytes*/)
	{	// std::thread は既定スタック(数MB)を使うため stackBytes は無視(ESP32専用の調整)。
		auto thread = new std::thread(func, parm);
		return thread;
	}

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
