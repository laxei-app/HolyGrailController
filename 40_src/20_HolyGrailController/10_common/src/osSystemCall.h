#ifndef  _THREAD_ENTRY_H
#define _THREAD_ENTRY_H 
// OS のシステムコールを使う部分を集約する

#include "common.h"

// os system call
namespace ossc
{

	typedef const std::function<errCode(void*)> THREAD_FUNC;

	// 具体的なスレッドの入り口をここにまとめる
	void* threadNet(THREAD_FUNC& func, void* parm);
	void  threadEnd(void * handle);	

	// 通知
	//void* handleNotify(std::string id);	// 通知のハンドルを取得する。
	//errCode sendNotify(void* handle);
	//errCode waitNotify(void* handle, uint32_t timeout);

}

#endif