// osSystemCall.cpp
// windows の OS 依存部分をここに集約する

#include "osSystemCall.h"
#include <thread>
#include <mutex>

namespace ossc
{
	// スレッドを起動する。
	// return : スレッドに通知するための例えばタスクIDとかを返す
	void* threadNet(THREAD_FUNC& func, void* parm)
	{
		auto thread = new std::thread(func, parm);
		return thread;
	}

	// スレッドの終了を待ってからスレッドを破棄する
	void  threadEnd(void* handle)
	{
		if (!handle) { return; }

		auto threadPtr = (std::thread*)handle;
		if (threadPtr->joinable())
		{
			threadPtr->join();
		}
		delete threadPtr;
	}

	

}
