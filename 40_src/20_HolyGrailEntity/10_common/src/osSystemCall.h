#ifndef  _THREAD_ENTRY_H
#define _THREAD_ENTRY_H 
// OS のシステムコールを使う部分を集約する

#include "common.h"

// os system call
namespace ossc
{

	typedef const std::function<errCode(void*)> THREAD_FUNC;

	// 具体的なスレッドの入り口をここにまとめる。
	// stackBytes: タスクのスタックサイズ(M5Stack/ESP32のみ有効)。撮影runnerはjson反復収束で16KB必要だが、
	//   netThreadワーカーやSSDP待ち受けのようなHTTP/UDP I/Oだけの軽量スレッドは小さくして内部DRAMを空ける
	//   (2カメラ同時で撮影runnerタスク×2の生成に内部DRAMが不足して失敗する問題への対策)。
	// 既定14336: Arduino 3.x(ESP-IDF5)では内部DRAMの最大連続ブロックが約16KBまで細切れ化するため、
	// 16384 だと 2台目の撮影runnerタスクの生成が「わずかに足りず」失敗する(largest=16372 < 16384)。
	// 14KB に下げてチャンクへ確実に収める(実スタック高水位は RUNNER-STACK ログで安全確認済)。
	void* threadNet(THREAD_FUNC& func, void* parm, uint32_t stackBytes = 14336);
	void  threadEnd(void * handle);	

	// 通知
	//void* handleNotify(std::string id);	// 通知のハンドルを取得する。
	//errCode sendNotify(void* handle);
	//errCode waitNotify(void* handle, uint32_t timeout);

}

#endif