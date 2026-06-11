#ifndef _CAPTURE_RUNNER_H_
#define _CAPTURE_RUNNER_H_
// 仕様書(10) 4章 撮影実行。
// 撮影計画(cs)のスケジュールに従い、別スレッドで撮影ループを回す。
//  - 夜間撮影: 固定露出
//  - 朝日/夕日/日中: 測光→露出決定の自動露出(初期補正・移動平均・ヒステリシス)
//  - リニア移行: 夜間の固定露出へ 1/3 段ずつ収束
// カメラ I/O は cameraController(CCAPI) を使用する。

#include "common.h"
#include "cameraController.h"
#include "device.h"
#include "cs.h"
#include "exposureMath.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

class captureRunner
{
public:
	// 進捗・撮影完了の通知情報
	struct progressInfo { int frame; int total; int remainSec; int elapsedSec; };
	struct capturedInfo { int frame; hgc::exposure exp; double luminance; std::string ccm; };

	using stateCb    = std::function<void(int)>;					// hgeState 値
	using progressCb = std::function<void(const progressInfo&)>;
	using capturedCb = std::function<void(const capturedInfo&)>;
	using errorCb    = std::function<void(errCode, const std::string&)>;

	captureRunner() = default;
	~captureRunner();

	void setCallbacks(stateCb s, progressCb p, capturedCb c, errorCb e);

	// 撮影準備。plan は events/ccmList を生成済みであること。
	errCode ready(const hgc::cs& plan, device* dev,
	              const hgc::exposureSmoothing& smooth, int utcOffsetMin);
	errCode start(void);	// ワーカースレッドで撮影ループ開始(即 return)
	errCode stop(void);		// 停止要求して join
	bool    isRunning(void) const { return running_; }

	// hgeState 値(モジュール構造仕様書 47 準拠)
	enum state { ST_IDLE = 0, ST_SEARCHING = 1, ST_READY = 2,
	             ST_CAPTURING = 3, ST_STOPPING = 4, ST_ERROR = 5 };

private:
	errCode loop(void);								// 撮影ループ本体(別スレッド)
	const hgc::ccmWindow* activeWindow(long long nowSec) const;
	hgc::exposure nightGoalAfter(long long nowSec) const;	// 次の夜間固定露出
	void interruptibleSleep(long ms);

	hgc::cs plan_{};
	device* dev_ = nullptr;
	hgc::exposureSmoothing smooth_{};
	int off_ = 0;

	std::atomic<bool> running_{ false };
	void* thread_ = nullptr;

	// カメラの設定可能値テーブル(開始時に取得して構築。仕様 4.2)
	expo::expoTables tables_;

	stateCb    onState_;
	progressCb onProgress_;
	capturedCb onCaptured_;
	errorCb    onError_;
};

#endif // _CAPTURE_RUNNER_H_
