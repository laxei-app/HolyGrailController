#ifndef _CAPTURE_RUNNER_H_
#define _CAPTURE_RUNNER_H_
// 仕様書(10) 4章 撮影実行。
// 撮影計画(cs)のスケジュールに従い、別スレッドで撮影ループを回す。
//  - 夜間撮影: 固定露出
//  - 朝日/夕日/日中: 測光→露出決定の自動露出(初期補正・移動平均・ヒステリシス)
//  - 夜間前移行: 自動露出→夜間の固定露出へ 1/3 段ずつ収束
//  - 夜間後移行: 夜間露出を上限にクランプし、次の自動露出の初期値/限界/優先度で測光自動露出(目標=postNightEv)
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
	// luminance=露出の明るさ[段](Sv-Av-Tv)。metered=測光したリニア輝度(自動補正時のみ。<0=測光なし)。
	// rdyMeteringMs=ライブビュー取得(rdyMetering)実測ms、rdyShutterMs=露出設定適用(rdyShutter)実測ms。
	// いずれも計測点では変数に退避するだけで、ログ出力はこのコマ確定後(シャッター後)に行う(-1=計測なし)。
	// tm0Ms=タイマ方式のtm0処理時間(境界での 露出適用+測光 の合計ms)。offset に収まるかの判定用(ユーザー要望)。
	struct capturedInfo { int frame; hgc::exposure exp; double luminance; std::string ccm; double metered = -1.0;
	                      int rdyMeteringMs = -1; int rdyShutterMs = -1; int tm0Ms = -1; };

	using stateCb    = std::function<void(int)>;					// hgeState 値
	using progressCb = std::function<void(const progressInfo&)>;
	using capturedCb = std::function<void(const capturedInfo&)>;
	using errorCb    = std::function<void(errCode, const std::string&)>;
	// 再接続要求。撮影中にカメラ通信が連続失敗したときに呼ばれる。SSDP等で計画のカメラを
	// 再探索し、成功したら *dev_ が指すデバイス(セッション専用device)を最新へ更新して true を返す。
	using reconnectCb = std::function<bool()>;

	captureRunner() = default;
	~captureRunner();

	void setCallbacks(stateCb s, progressCb p, capturedCb c, errorCb e);
	void setReconnect(reconnectCb r) { onReconnect_ = std::move(r); }

	// 撮影準備。plan は events/ccmList を生成済みであること。
	errCode ready(const hgc::cs& plan, device* dev,
	              const hgc::exposureSmoothing& smooth, int utcOffsetMin);
	errCode start(void);	// ワーカースレッドで撮影ループ開始(即 return)
	errCode stop(void);		// 停止要求して join
	bool    isRunning(void) const { return running_; }

	// 取得フェーズ(カメラ未取得=apiBase==nullptr の間)の60秒待ちを打ち切り、即再探索させる。
	// SSDP受動待ち受けがカメラの出現(NOTIFY)を検知したときに外部から呼ぶ(3b)。取得フェーズ以外では無害。
	void    pokeAcquire(void);

	// hgeState 値(モジュール構造仕様書 47 準拠。holyGrailEntity.h の hgeState と番号同期)
	enum state { ST_IDLE = 0, ST_SEARCHING = 1, ST_READY = 2,
	             ST_CAPTURING = 3, ST_STOPPING = 4, ST_ERROR = 5,
	             ST_DISCONNECTED = 6,	// 撮影中に接続断(NOCAMERAの旧同義)
	             ST_WAITING = 7,		// 撮影窓前・カメラOKで待機中(点灯)
	             ST_NOCAMERA = 8 };		// カメラ未検出(✖点灯)。武装/撮影中いずれでも

	// --- 接続維持・再接続のパラメータ ---
	static constexpr int  kKeepAliveSec        = 60;	// 撮影窓まで待機中、無害なGETを送る周期[秒]
	static constexpr int  kWaitMaxFail         = 2;		// 待機中keepAliveがこの回数連続失敗で先回り再接続(健全性チェック)
	static constexpr int  kMaxConsecutiveFail  = 3;		// 撮影失敗が連続したら再接続を試みる回数
	static constexpr int  kMaxReconnectTries   = 3;		// SSDP再探索の試行回数。これを超えたら諦める
	static constexpr long kReconnectWaitMs     = 2000;	// 再接続試行間の待ち[ms]

	// --- 周期正確化(タイマ方式) ---
	static constexpr double kShutterOffsetSec  = 2.0;	// tm0(露出適用+測光)→ tm1(シャッター)の差[秒]。=周期と最大ssの差(固定)
	static constexpr int    kPreConvergeSec    = 30;	// 撮影窓の何秒前から初期収束(測光のみ・シャッター無し)を始めるか

private:
	errCode loop(void);								// 撮影ループ本体(別スレッド)
	bool    establishSession(void);					// startShooting+M設定+設定値テーブル構築(開始/再接続で使用)
	errCode rdyMeterTimed(void);					// rdyMetering を実測付きで呼ぶ(所要msは meterMs_ に退避)
	errCode applyExposureChanged(const hgc::exposure& exp);	// 変更のあった ss/iso/fn だけを適用(タイマ方式tm0)
	void    sleepUntilElapse(void* mono, long targetMs);	// monotonic基準(mono)で targetMs 経過まで待つ(中断可)
	const hgc::ccmWindow* activeWindow(long long nowSec) const;
	hgc::exposure nightGoalAfter(long long nowSec) const;	// 次の夜間固定露出
	// 最初の補正(仕様 4.4)を反復収束で行い、撮影開始直後の初期露出を決める。
	hgc::exposure initialConverge(expo::exposureCtl& ctl, const hgc::exposure& initial, double evT);
	void interruptibleSleep(long ms);

	hgc::cs plan_{};
	device* dev_ = nullptr;
	hgc::exposureSmoothing smooth_{};
	int off_ = 0;

	std::atomic<bool> running_{ false };
	std::atomic<bool> wake_{ false };	// 取得フェーズの待ちを前倒しするポーク(pokeAcquire で立てる)
	void* thread_ = nullptr;

	// カメラの設定可能値テーブル(開始時に取得して構築。仕様 4.2)
	expo::expoTables tables_;

	int meterMs_ = -1;	// 直近 rdyMetering(ライブビュー取得)の実測ms。コマ毎にリセットしログへ出す(計測用)
	// 変更分のみ適用(タイマ方式tm0)の直近適用値。establishSession でクリアし次回フル適用させる。
	std::string lastFnApplied_, lastSsApplied_, lastIsoApplied_;

	stateCb     onState_;
	progressCb  onProgress_;
	capturedCb  onCaptured_;
	errorCb     onError_;
	reconnectCb onReconnect_;
};

#endif // _CAPTURE_RUNNER_H_
