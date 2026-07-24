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
	// rdyMeteringMs=測光(ライブビュー取得)実測ms、rdyShutterMs=露出設定の実測ms。いずれも -1=計測なし。
	// meterTry/applyTry=それぞれの試行回数(1=一発成功。>1 はリトライした)。
	// prepMs=準備(測光→露出計算→露出設定)の合計ms。リード(kPrepLeadMs)に収まったかの判定に使う。
	// lateMs=このコマのシャッターが「前コマ+撮影周期」からどれだけ遅れたか[ms](0=周期ぴったり。-1=計測なし)。
	//   準備がリードに収まらなかったコマだけ >0 になる=周期を守れたかが1数値で分かる(計測の主指標)。
	struct capturedInfo { int frame; hgc::exposure exp; double luminance; std::string ccm; double metered = -1.0;
	                      int rdyMeteringMs = -1; int rdyShutterMs = -1; int prepMs = -1; int lateMs = -1;
	                      bool rdyOk = true; bool setOk = true;
	                      int meterTry = 0; int applyTry = 0;
	                      uint32_t histSum = 0;	// 測光ヒストグラムの内容チェックサム(0=測光なし)。前コマと同値=古いフレームを掴んだ疑い
	                      uint64_t lvTimeMs = 0;	// 測光フレームのカメラ側取得時刻[ms]。0=不明
	                      int staleSkip = 0;	// 古いフレームと判定して捨てた回数(0=無し)
	                      uint64_t shutterMs = 0;	// actShutter直前の壁時計(UTCエポックms)。0=未記録
	                      double lvP99 = -1.0;	// 【診断トラップ】測光ヒストの明るい側1%点(0..1)。<0=測光なし
	                      double lvPMax = -1.0;	// 【診断トラップ】画素のある最も明るいビン(0..1)。<0=測光なし
	                      std::string meterSs;	// 測光に使ったシャッター(空=撮影露出のまま測光=従来動作)
	                      int meterSettleMs = -1;	// 測光ss変更→LV反映の待ち[ms](-1=切替なし)
	                      bool lvPinned = false; };	// 測光ssを変えても値が動かない=ライブビューが張り付いている

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

	// Phase4(1エッジ複数カメラ同時): このセッションの tm0/tm1 境界を撮影周期内で frac 分だけ後ろへずらす。
	// frac=[0,1)。0=先頭(ずらし無し)、2台なら 0.5。全カメラのHTTPが単一 netThread ワーカーへ同時集中して
	// 衝突する(=シャッターが遅延/失敗する)のを、周期内で時間分散して避けるためのスロット位置。
	// 単独撮影(スマホ/1台)では frac=0 のまま＝挙動不変。
	void setStagger(double frac) { staggerFrac_ = frac; }

	// 撮影準備。plan は events/ccmList を生成済みであること。
	errCode ready(const hgc::cs& plan, device* dev,
	              const hgc::exposureSmoothing& smooth, int utcOffsetMin);
	errCode start(void);	// ワーカースレッドで撮影ループ開始(即 return)
	// 呼び出しスレッド上で撮影ループを実行する(start のインライン版。ループ終了まで戻らない)。
	// エッジ(M5Stack)では runner 用の2本目のタスクスタック(内部RAM14KB)が断片化で確保できない
	// ことがあるため、起動シーケンスのスレッドをそのまま撮影ループに使いセッションあたり1本にする。
	// 停止は stop()(running_ を落とすのみ。join しない)→ 呼び出しスレッド(起動スレッド)の join の順。
	errCode runInline(void);
	errCode stop(void);		// 停止要求して join(runInline 実行中は要求のみ。join は起動スレッド側で行う)
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
	static constexpr int  kMaxConsecutiveFail  = 3;		// 撮影(シャッター)失敗が連続したら再接続を試みる回数
	static constexpr int  kMaxMeterFail        = 5;		// 測光(ライブビュー)が連続失敗したら再establishする回数。
														// シャッターは通るがライブビューだけ死ぬと、従来は露出が固定のまま復帰しなかった(その対策)。
	static constexpr int  kMaxReconnectTries   = 3;		// SSDP再探索の試行回数。これを超えたら諦める
	static constexpr long kReconnectWaitMs     = 2000;	// 再接続試行間の待ち[ms]

	// --- 周期正確化(タイマ方式) ---
	// シャッターは常に「前コマのシャッター([A]) + 撮影周期」で切る(相対アンカー)。
	// 準備(測光→露出計算→露出設定)は、その kPrepLeadMs だけ手前から連続で行う。
	//  ・SSに依存して測光時刻が前後しないので、周期のどこで何が起きるかが固定される。
	//  ・カメラは撮影/記録中(露光+記録)の間、設定変更を 503 "During shooting or recording" で即拒否する。
	//    露光直後に設定するとこれに必ず当たる(実測: 露光終了直後=503 / 0.4秒後=OK)。リード手前まで待てば
	//    カメラは暇なので設定は数十msで通る(実測: R10=52ms, R100=63ms いずれも8/8成功)。
	//  ・準備がリードに収まらなければ、終わり次第すぐシャッターを切り(遅れ許容)、次コマからは正確な周期へ戻る。
	// 制約: 測光開始時に露光が終わっている必要があるため 周期 - kPrepLeadMs > 最大ss。
	//   さらにライブビュー復帰(実測 R100=長秒露光後3.3秒)を待つには 周期 - 最大ss >= リード + 復帰時間 ≒ 5.3秒。
	//   これを満たさない設定(例 周期10秒/夜間ss8秒)も許可する(カメラ単体のインターバル撮影と同じく周期が伸びる)。
	// 測光シャッター導入(2026-07-19)に伴い 2000→5000。準備の中で「測光用シャッターへ変更→
	// ライブビューに反映されるまで待つ(実測 R10 1.3〜2.1秒)→測光→撮影用露出を適用」を行うため。
	static constexpr int    kPrepLeadMs          = 5000;	// 周期の何ms前に準備(測光→計算→設定)を始めるか

	// --- 測光シャッター(仕様: 測光する露出と撮影する露出を分ける) ---
	// ライブビューは「暗くて必要な増感が上限を超える」と張り付き、露出を変えても値が動かなくなる
	// (2026-07-19 実写と突き合わせて実証。R10は8"で完全停止、R100も同様。止まる位置は機種で違う)。
	// 撮影露出のまま測光すると夜間はこの張り付き領域に入り、動かない値を明るさとして読んでしまう。
	// → 測光は「ライブビューが応答する短いシャッター」で行い、環境光はその露出から逆算する。
	//   撮影シャッターが何秒でも(15"でも)測光の難しさは変わらない。
	static constexpr int    kMeterSettleMaxMs    = 2600;	// Tv変更がLVに反映されるまでの待ち上限[ms]
	static constexpr double kMeterTargetX        = 0.35;	// 測光で狙うLV中央値(黒潰れ/張り付きから最も遠い)
	static constexpr double kMeterUsableLoX      = 0.020;	// これ未満は暗すぎて信用しない
	static constexpr double kMeterUsableHiX      = 0.850;	// これ超は明るすぎて信用しない
	static constexpr double kMeterRespondRatio   = 0.50;	// 「Δss段」に対しΔ測光段がこの比未満なら張り付き
	static constexpr int    kMeterInitDropStops  = 5;	// 初回の測光ss=撮影ssから何段短くするか(以後は実測で追従)
	// 測光ssは「明るい中央値(0.35)を狙って長秒化」せず、pin=0(LVが応答する短い側)を保つ(2026-07-24)。
	static constexpr double kMeterPinBackoffStops = 1.0;	// 張り付き検出時、天井をこの段数だけ短く下げる
	static constexpr double kMeterMaxLenStep      = 1.0;	// 暗すぎるとき1コマで伸ばす上限[段](pin突入を防ぐ)
	static constexpr double kMeterCeilRelaxStops  = 0.10;	// 天井を毎コマこれだけ緩め、条件変化(明るくなる等)へ追従
	static constexpr int    kPreConvergeSec      = 30;	// 撮影窓の何秒前から初期収束(測光のみ・シャッター無し)を始めるか
	// ③ ヒスト取得リトライ: 失敗なら kMeterRetryMs 間隔で kMeterMaxMs まで繰り返す。
	static constexpr int    kMeterRetryMs        = 100;	// ヒスト取得リトライ間隔[ms]
	static constexpr int    kMeterMaxMs          = 5000;	// ヒスト取得リトライ上限[ms]
	// 露出設定リトライ: 503(busy)等で弾かれても諦めず繰り返す。設定できないまま撮り続けると
	// アプリの露出モデルとカメラ実機がズレ、白飛び/黒潰れのまま復帰できなくなる(2026-07-16 実機で発生)。
	static constexpr int    kApplyRetryMs        = 100;	// 露出設定リトライ間隔[ms]
	static constexpr int    kApplyMaxMs          = 2000;	// 露出設定リトライ上限[ms]
	// 古いライブビュー判定の許容[ms]。「LV取得時刻の進み ≥ 前回測光からの実経過 − この値」なら新鮮。
	// LVフレームの生成周期+取得パイプラインの揺らぎぶん。固定しきい値(旧: 周期-5秒)は撮影周期前提で、
	// 1〜2秒間隔で測光する初期収束の測光を全て誤って捨て、収束を壊した(2026-07-17夜)。実経過基準は
	// 測光間隔に依存しないため、本撮影(15秒間隔)でも初期収束(1〜2秒間隔)でも正しく判定できる。
	static constexpr int    kLvFreshMarginMs     = 2000;

	// --- 測光値と撮影露出の「土俵合わせ」(2026-07-21 白飛び暴走の根治) ---
	// 測光は測光シャッター(短い露出)で行うため、測光リニア値はその露出で写る明るさである。
	// これを撮影露出の目標(ev0基準)とそのまま比べると、両露出の段差ぶん(実測で最大9段)ずれる。
	// さらに測光ループは中央値を kMeterTargetX に貼り付けるので測光値は撮影露出を動かしても
	// 変化せず、比較結果が永久に同じ向きのまま=1/3段/コマで限界まで暴走した
	// (2026-07-20夕方の実機: 1/60→8秒へ8.91段/27コマ=0.33段/コマ、実写は飽和85〜98%)。
	// → 測光値から露出成分を割り戻して「場面の明るさ」にし、撮影露出で撮った場合の値へ
	//   投影してから比較する。これでループが閉じ、露出を動かすと比較結果も動く。
	static constexpr double kExposureStepStops   = 1.0 / 3.0;	// 露出制御の1ステップ(段)
	// 1コマで詰めてよい上限(段)。撮影中は必ず1ステップ(=1/3段)に留め、撮影計画の境目も含めて
	// 露出設定を飛ばさず滑らかに動かす(2026-07-24: 境目の多段ジャンプを禁止)。
	// 大きなズレを速く詰めるのは撮影前の初期収束(initialConverge, シャッター無し)だけの役割とする。
	static constexpr double kMaxCatchUpStops     = kExposureStepStops;

private:
	errCode loop(void);								// 撮影ループ本体(別スレッド)
	bool    establishSession(void);					// startShooting+M設定+設定値テーブル構築(開始/再接続で使用)
	errCode rdyMeterTimed(void);					// rdyMetering を実測付きで呼ぶ(所要msは meterMs_ に退避)
	// ③ ヒスト取得(rdyMetering+alzMetering)を最大 kMeterMaxMs まで kMeterRetryMs 間隔でリトライ。成功で hist を埋め true。
	//    露光中は取得できないので、露光終了までの待ちは呼び出し側(ループ)の責務。tries=試行回数を返す。
	bool    meterWithRetry(cmdt::HISTOGRAM& hist, int& tries);
	// 測光シャッターへ切り替える。戻り値=実際に測光に使う露出(ss だけ差し替えたもの)。
	//  ・切替が失敗/不要なら shotExp をそのまま返す(=従来動作へフォールバック)。
	//  ・反映待ちの実測は meterSettleMs_ へ、使った ss は meterSsUsed_ へ退避(ログ用)。
	hgc::exposure enterMeteringShutter(const hgc::exposure& shotExp);
	// 今回の測光結果から「次コマの測光ss」を更新し、張り付きを判定する。
	void          updateMeterShutter(const hgc::exposure& meterExp, double linear);
	// 測光リニア値から露出成分を割り戻し、露出に依存しない「場面の明るさ」にする(土俵合わせ)。
	//  測光露出が無効なら割り戻さずそのまま返す(従来動作へのフォールバック)。
	double        sceneRefFromMetered(double linear, const hgc::exposure& meterExp) const;
	// 「場面の明るさ」を、その露出で撮ったときのリニア輝度へ戻す(sceneRefFromMetered の逆)。
	double        linearAtExposure(double sceneRef, const hgc::exposure& e) const;
	// 目標との差(段)から、このコマで踏む 1/3 段ステップ数を決める(1〜kMaxCatchUpStops相当)。
	int           stepsToClose(double needStops) const;
	// 実際にカメラへ適用できている露出を返す。lastXxxApplied_ は「その軸の設定が成功したときだけ」
	// 更新されるので、一部の軸だけ失敗した場合も含めて実機の状態を正しく表す。
	hgc::exposure appliedExposure(void) const;
	// HTTPを伴うカメラ操作の失敗メッセージに、直近のHTTPステータスと応答本文を添える。
	//  "actShutter http=503 During shooting or recording" / "actShutter http=応答なし"
	std::string   withHttpDetail(const char* what) const;
	errCode applyExposureChanged(const hgc::exposure& exp);	// 変更のあった ss/iso/fn だけを適用
	// 露出設定を最大 kApplyMaxMs まで kApplyRetryMs 間隔でリトライ。tries=試行回数を返す。
	errCode applyWithRetry(const hgc::exposure& exp, int& tries);
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
	double staggerFrac_ = 0.0;	// Phase4: 撮影周期内のスロット位置[0,1)。同時撮影のずらし用(0=単独/先頭)

	std::atomic<bool> running_{ false };
	std::atomic<bool> wake_{ false };	// 取得フェーズの待ちを前倒しするポーク(pokeAcquire で立てる)
	void* thread_ = nullptr;

	// カメラの設定可能値テーブル(開始時に取得して構築。仕様 4.2)
	expo::expoTables tables_;

	// ② ev0シグモイド設定(コマ毎に太陽高度から中心bmを算出して更新)。露出計算/初期収束で共用。
	expo::ev0Sigmoid ev0cfg_{};

	int  meterMs_  = -1;	// 直近 rdyMetering(ライブビュー取得)の実測ms。コマ毎にリセットしログへ出す(計測用)
	bool meterOk_  = true;	// 直近 rdyMetering の成否(rdyMeterTimed が設定)。シャッター後にまとめてログ
	int  meterTry_ = 0;	// 直近の測光試行回数(1=一発成功。>1=リトライした)。コマ毎にリセットしログへ出す
	uint64_t lvTimeMs_ = 0;	// 直近の測光フレームのカメラ側取得時刻[ms](0=不明)
	uint64_t lvPrev_   = 0;	// 前回「採用した」測光フレームの取得時刻[ms](カメラ時計)。古いフレーム判定の基準
	void*    lvPrevAt_ = nullptr;	// 前回採用した時点の単調時計。実経過時間との比較に使う(kLvFreshMarginMs参照)
	int      staleSkip_ = 0;	// このコマで「古い」と判定して捨てた回数(0=無し)。ログで頻度を追う
	uint32_t histSum_ = 0;	// 直近の測光ヒストグラムの内容チェックサム(0=測光なし)。
							// 前コマと一致=カメラが古いフレームを返した疑い(測光値が1コマ古い)の検出用
	// 【診断トラップ(一時)】ライブビュー・ヒストグラムの明るい側の位置(0..1, sRGB符号化)。
	//  夜明けにライブビューが「明るい画素を捉えているか」を見るため。測光統計は変えず、既存のヒストから算出するだけ。
	//  p99=明るい側1%点, pMax=画素のある最も明るいビン。<0=測光なし。
	double   lvP99_  = -1.0;
	double   lvPMax_ = -1.0;
	// --- 測光シャッターの状態 ---
	std::string meterSs_;					// 次に使う測光シャッター(空=未決定→撮影ssから kMeterInitDropStops 段短く)
	std::string meterSsUsed_;				// 今コマ実際に使った測光ss(ログ用。空=撮影ssのまま測光した)
	int         meterSettleMs_ = -1;		// 今コマの「Tv変更→LV反映」実測[ms](-1=切替なし)
	double      meterPrevStops_ = 0.0;		// 前コマの測光ssの明るさ[段]
	double      meterPrevLin_   = -1.0;		// 前コマの測光リニア値(<0=無し)
	bool        lvPinned_       = false;	// ライブビューが張り付いている(測光値を信用しない)
	bool        lvPinnedLog_    = false;	// 今コマのログ用
	double      meterCeilStops_ = 1e9;		// 測光ssの長さ上限[段]。張り付き検出で下がる(=pin=0を保つ天井)。1e9=天井なし
	// 変更分のみ適用(タイマ方式tm0)の直近適用値。establishSession でクリアし次回フル適用させる。
	std::string lastFnApplied_, lastSsApplied_, lastIsoApplied_;
	// 露出設定に失敗したまま次のシャッターを落とすと、カメラは測光シャッターのままなので
	// そのコマだけ露出が飛ぶ(2026-07-20 IMG_1092=1/100, IMG_1100=1/320 が実例)。
	// 失敗を覚えておき、次のシャッター直前にもう一度だけ適用を試す。
	bool          applyFailed_ = false;	// 直前の露出適用が失敗した
	hgc::exposure wantExp_{};			// そのとき適用したかった露出(再試行用)

	stateCb     onState_;
	progressCb  onProgress_;
	capturedCb  onCaptured_;
	errorCb     onError_;
	reconnectCb onReconnect_;
};

#endif // _CAPTURE_RUNNER_H_
