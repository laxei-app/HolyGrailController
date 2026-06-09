// osSystemCall.cpp
// Android で OS 依存のシステムコールを集約する。std::thread を使用。

#include "osSystemCall.h"
#include <thread>

namespace ossc
{
	// スレッドを起動する。
	// return : スレッドを破棄するためのハンドル
	void* threadNet(THREAD_FUNC& func, void* parm)
	{
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
