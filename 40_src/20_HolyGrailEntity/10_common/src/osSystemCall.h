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
	// 既定12288(2026-08-23): 撮影スレッドの高水位を実測して決めた。
	//  4セッション計測で used=9964、9964、9964、10036(ばらつき72バイト)。
	//  12288 なら余裕 2252(18%)。断片化ではなく**実使用量**から決めている
	//  (確保先は .bss の静的プールなので、連続ブロックの大きさには左右されない)。
	//  通っていない経路(再接続/認証/R10のライブビュー測光)で深くなる可能性があるため、
	//  セッション終了ごとに STACK ログ(leftMin)を残して監視し続けること。
	// useStaticPool: true=撮影セッション用の静的スタック枠を使う(M5実装のみ意味を持つ)。
	//  当初は「スタックサイズが既定値と一致したらプール」としていたが、常駐スレッド
	//  (在否監視/カメラ検索)も既定値で作られるため枠を奇取し、3本目の撮影が始まらなかった
	//  (2026-08-23 実機)。サイズではなく**用途を明示**すること。
	void* threadNet(THREAD_FUNC& func, void* parm, uint32_t stackBytes = 12288, bool useStaticPool = false);
	// 内部RAM(タスクスタックに必須な領域)の空き[byte]。共通層から計測点を置くための窓口。
	//  スマホはこの制約が無いので 0 を返す(ログを出さない判定に使う)。
	size_t internalFree(void);
	// 起動からの内部RAMの最低値[byte]。単調に下がるだけなので、区間の前後で比べれば
	// 「その区間で新しい底を作ったか」が分かる(サンプリングの瞬間に依存しない)。
	size_t internalMinFree(void);
	void  logLiveThreads(void);	// 生きているスレッドの確保量と高水位をログへ(内部RAMを削るための計測)
	void  threadEnd(void * handle);	

	// 通知
	//void* handleNotify(std::string id);	// 通知のハンドルを取得する。
	//errCode sendNotify(void* handle);
	//errCode waitNotify(void* handle, uint32_t timeout);

}

#endif