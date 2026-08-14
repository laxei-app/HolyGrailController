#ifndef _CAPTURE_RUNNER_H_
#define _CAPTURE_RUNNER_H_
// 仕様書(10) 4章 撮影実行。
// 撮影計画(cs)のスケジュールに従い、別スレッドで撮影ループを回す。
//  - 夜間撮影: 固定露出
//  - 朝日/夕日/日中: 測光→露出決定の自動露出(初期補正・移動平均・ヒステリシス)
// 【測光は「どう測るか」を知らない】場面の明るさは cameraController::meterScene / meterHere が返す。
//  CCAPI(Canon)は直前に撮れた最新画像のサムネイルから算出するが、そこはカメラ実装の中に
//  完全に閉じており、別方式のカメラを足してもこのファイルは変わらない。
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
	// rdyMeteringMs=測光の実測ms、rdyShutterMs=露出設定の実測ms。いずれも -1=計測なし。
	// meterTry/applyTry=それぞれの試行回数(1=一発成功。>1 はリトライした)。
	// prepMs=準備(測光→露出計算→露出設定)の合計ms。リード(kPrepLeadMs)に収まったかの判定に使う。
	// lateMs=このコマのシャッターが「前コマ+撮影周期」からどれだけ遅れたか[ms](0=周期ぴったり。-1=計測なし)。
	//   準備がリードに収まらなかったコマだけ >0 になる=周期を守れたかが1数値で分かる(計測の主指標)。
	// 撮影開始前の初期収束が、うまくいったのか失敗したのか(セッション単位)。撮影レポートへ載せる。
	// 収束が済んでいないまま撮り始めると露出が最初から外れるので、後から判断できるようにする。
	struct convergeInfo
	{
		int steps   = 0;	// 測光値を採用できた回数。0=一度も測れていない
		int applyNg = 0;	// 露出をカメラへ適用できず、やり直した回数
		int meterNg = 0;	// 測光できず、やり直した回数
		// 0=収束した(目標±1/3段以内 か 露出限界に到達) / 1=収束しきらず最良推定で開始
		// 2=一度も測れず基準値のまま開始(最悪。露出が最初から大きく外れる)
		int outcome = 2;
	};

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
	                      // 測光統計量の比較用(2026-08-08 診断。制御には使わない)。夜明けの追従遅れの
	                      // 原因が中央値にあるため、候補(平均/p75/p90/飽和率)を並べてログに残す。
	                      double lvMeanLin = -1.0;	// ヒストグラム全体のリニア平均
	                      double lvP75 = -1.0, lvP90 = -1.0;	// 75%点/90%点(sRGB位置)
	                      double lvSat = -1.0;	// 最上位ビンの画素割合(0..1)
	                      std::string meterSs;	// 測光に使ったシャッター(空=撮影露出のまま測光=従来動作)
	                      int meterSettleMs = -1;	// 測光のために待った時間[ms](-1=待っていない)
	                      bool lvPinned = false;	// 測光ssを変えても値が動かない=ライブビューが張り付いている
	                      // 測れたが中央値が使える帯の外(=露出制御の根拠にできない)。値はログに残し
	                      // 露出は据え置いた、という状態(2026-08-07)。ログ use=0。true=正常に採用。
	                      bool meterUsable = true;
	                      // 測光所要の内訳(2026-07-28)。遅延がカメラの記録待ちか通信かを切り分ける。
	                      int meterWaitMs = -1;	// 新しい画像の登録通知を待った時間[ms]
	                      int meterFetchMs = -1;	// サムネイル取得(HTTP GET)の時間[ms]
	                      int meterDecodeMs = -1;	// JPEG復号+ヒスト計算の時間[ms]
	                      int meterFetchTries = 0;	// サムネイル取得の試行回数
	                      // 露光終了から「測光できる」までカメラが記録で塞がっていた時間[ms]。
	                      // サムネイル測光では「露光終了 → 新しい画像の登録通知が来るまで」がそれに当たる。
	                      //  >=0 : 実測値(0=待ちなし)  kBusyNotMeasured : 計測できなかった
	                      int busyMs = -1;
	                      // このコマで準備に与えられたリード[ms](=周期 - 準備開始時刻)。測光が
	                      // 間に合っているかを判断する基準。0=不明(ウォームアップ等)。
	                      int leadMs = 0;
	                      // 旧LV方式の診断(現方式では常に -1)。ログの列を保つために残す。
	                      double asIsLinear = -1.0;
	                      // このセッションで1枚目の露出をカメラへ適用するのに要した回数
	                      // (1=一発で乗った / >1=待って乗った / 0=不明)。放置後の初回にだけ
	                      // 起きる適用失敗を、後から「起きたが吸収した」と確認するための記録。
	                      int firstApplyTries = 0;
	                      convergeInfo converge{};	// 撮影開始前の初期収束の結果(セッション単位)
	                      // 何で測ったか(レポートの件数用)。apiBase::via_* の値。
	                      int meterVia = 0; };

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
	// シャッターが 503(記録中)で断られたときの粘り方(2026-07-30)。カメラが応答している間は
	// そのコマの締め切りまで再試行する。締め切り = 周期 - 露光 - 余裕(上限あり)。
	static constexpr int  kShutterRetryMs     = 300;	// 再試行の間隔
	static constexpr int  kShutterMarginMs    = 1000;	// 次コマへ食い込まないための余裕
	static constexpr int  kShutterBusyMaxMs   = 8000;	// 粘る上限(長周期でも延々待たない)
	static constexpr int  kMaxConsecutiveFail  = 3;		// 撮影(シャッター)失敗が連続したら再接続を試みる回数
	// --- 「応答するのに撮れていない」カメラの検出(2026-08-13) ---
	// シャッターは 200 で受け付けられ、情報系もライブビューも生きているのに、撮った画像が
	// 一枚も記録されなくなることがある(EOS R10 の不具合として実測。電源を入れ直すまで戻らない)。
	// この状態は接続の生死では判定できない。判定できるのは「撮った画像が現れるか」だけである。
	//  ・1回目は静かにセッションを張り直す(通知の取りこぼし等の一過性はこれで直る)。
	//  ・張り直しても現れなければ「オンラインでない」(ST_NOCAMERA)として提示する。
	//  ・提示後もシャッターは撃ち続ける。復帰(電源入れ直し等)を拾えるのはそれだけだから。
	//    ただし1コマ内での 503 粘りはやめる(固まっているカメラへの追い打ちを避ける)。
	static constexpr int  kMaxNoRecordFrames   = 3;		// 撮影結果が現れないコマがこれだけ続いたら手を打つ
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

	// --- 周期を守れたかの判定 ---
	// [A](前コマのシャッター起点)からの実測間隔が「周期 + kLateOkMs」を超えたコマを
	// 「遅れたコマ」と数え、その回数と遅れ時間を撮影レポートへ載せる(ユーザー指示 2026-08-13)。
	// 100ms はスリープの刻み(sleepUntilElapse は最後の1回で端数まで寝る)とHTTP POSTの
	// ばらつきを吸収する幅。これを超える遅れは準備が周期に間に合わなかったことを意味する。
	static constexpr int kLateOkMs = 100;

	// --- busy(露光終了 → カメラが測光に応じられるまで) ---
	// 撮影周期をどこまで SS へ近づけられるかはこの時間が決めるので、機種ごとに実測して
	// 撮影レポートへ残す。サムネイル測光では「露光が閉じてから新しい画像の登録通知が来るまで」
	// (= meterResult.waitMs)がそのまま記録に掛かっていた時間なので、それを使う。
	// 専用のプローブは廃止した(2026-08-13)。測光そのものが毎コマ測ってくれるうえ、
	// プローブはライブビューが流れている前提で、ループ中はもう掴んでいないため。
	static constexpr int kBusyNotMeasured = -1;		// 計測できなかった(測光しないコマ等)

	// --- 測光 ---
	// 「どう測るか」(何を読むか・カメラをどう使うか・何回粘るか)はカメラ依存の実装詳細として
	// apiBase 実装側(apiCanonCCAPI)に完全に閉じている。このクラスが使うのは
	// cameraController::meterScene/meterHere が返す「場面の明るさ」(sceneRef)だけである。
	static constexpr int    kPreConvergeSec      = 30;	// 撮影窓の何秒前から初期収束(測光のみ・シャッター無し)を始めるか
	// 露出設定リトライ: 503(busy)等で弾かれても諦めず繰り返す。設定できないまま撮り続けると
	// アプリの露出モデルとカメラ実機がズレ、白飛び/黒潰れのまま復帰できなくなる(2026-07-16 実機で発生)。
	static constexpr int    kApplyRetryMs        = 100;	// 露出設定リトライ間隔[ms]
	static constexpr int    kApplyMaxMs          = 2000;	// 露出設定リトライ上限[ms]
	// 1枚目だけは上限を大きく取る(2026-08-05)。撮影中の適用は次コマに間に合わせる必要があるので
	// 2秒で打ち切るが、1枚目は撮影窓の前(または開始直後)なので待てる。ここで諦めて撮り始めると
	// カメラは初期収束で最後に使った測光露出のままになり、その露出でライブビューを測って飽和し、
	// 以後20コマ測光ss切替(毎コマ2.6秒の反映待ち)に落ちる。実測で確認した経路なので待って直す。
	// エッジのHTTP PUTは無返答時に既定5秒ブロックするため、1回の試行に5秒かかりうる。
	// 2秒では1回も再試行できていなかった(メッセージは "after retry" だが実際は1回だけ)。
	static constexpr int    kFirstApplyMaxMs     = 20000;	// 1枚目の露出設定リトライ上限[ms]

	// --- 測光値と撮影露出の「土俵合わせ」(2026-07-21 白飛び暴走の根治) ---
	// 測光リニア値は「測光したときの露出で写る明るさ」である。これを撮影露出の目標(ev0基準)と
	// そのまま比べると、両露出の段差ぶん(実測で最大9段)ずれる。さらに測光値が撮影露出を動かしても
	// 変化しない状況では、比較結果が永久に同じ向きのまま=1/3段/コマで限界まで暴走する
	// (2026-07-20夕方の実機: 1/60→8秒へ8.91段/27コマ=0.33段/コマ、実写は飽和85〜98%)。
	// → カメラ実装は露出成分を割り戻した「場面の明るさ」(sceneRef)を返し、こちらは撮影露出で
	//   撮った場合の値へ投影してから比較する。これでループが閉じ、露出を動かすと比較結果も動く。
	static constexpr double kExposureStepStops   = 1.0 / 3.0;	// 露出制御の1ステップ(段)
	// ヒステリシス帯の下限[段]。刻み q・測光の応答 γ のループは、デッドバンドが γ×q 以上で
	// ないと静止点を持たない(1歩動かすと需要が再び帯を超えて必ず引き返す)。
	//   動き出すしきい値 = max(帯/2, 1歩 - 帯/2)  ← 帯=1歩 がちょうど最小=最も緩い点
	// 実測γ(R100 2026-07-29,30): 中央 1.27〜1.46 / 四分位上側 1.82 / 単発最大 2.27。
	// γ は一定ではない(ピクチャースタイル auto でコマ毎に現像が変わる)ので、帯は上側の
	// γ で決める必要がある → 必要な帯 = 2.27 × 1/3 = 0.76段。
	//
	// 【この値の裏付け】日中の既定帯は 1.0段(=γ≦3.0まで安定)で、実際に日中は振動して
	// いない。夕日/朝日は 0.3段(=γ≦0.90までしか安定しない)で、必ず振動した。
	// 帯=1歩(0.333段)は γ≦1.00 相当で、しかも上式のしきい値が最小になるため
	// **現状より反転が増える**(単体テストで 8回→24回 を確認)。採用してはいけない。
	//
	// 追従の代償は小さい: 動き出すしきい値 0.183段 → 0.400段。ただし現状は振動そのもので
	// ±0.28段ぶれているので、実際の絵の安定性は改善する(テストの最大誤差 0.28→0.34段)。
	static constexpr double kMinHysteresisStops = 0.8;
	// 移動平均の遅れを補う外挿量の上限[段]。
	// 必要な補正量 = 変化速度[段/コマ] × (n-1)/2 で、実測の夜明け(0.46段/コマ・5点平均)では
	// 0.92段。0.5段では半分しか補正できないので、実測に余裕を持たせて 1.5段 とする。
	// 傾きは差分の中央値なので、一過性の光(1コマだけの外れ値)や段差では 0 に落ちる。
	// つまりこの上限は「素性の悪いデータで暴走させない」ための保険であって、
	// 正常な変化を頭打ちにするためのものではない。
	static constexpr double kSceneLeadMaxStops = 1.5;
	// 反転抑制中でも「本物の急変」は通す差[段]。1歩正しく動かした直後に測光がでっち上げる
	// 見かけの逆向き需要は最大 γ×1歩 - 帯/2 ≒ 0.47段(実測γ≒1.9)。その約2倍を境にする。
	static constexpr double kReversalGuardStops = 1.0;
	// 1コマで詰めてよい上限(段)。撮影中は必ず1ステップ(=1/3段)に留め、撮影計画の境目も含めて
	// 露出設定を飛ばさず滑らかに動かす(2026-07-24: 境目の多段ジャンプを禁止)。
	// 大きなズレを速く詰めるのは撮影前の初期収束(initialConverge, シャッター無し)だけの役割とする。
	static constexpr double kMaxCatchUpStops     = kExposureStepStops;

private:
	errCode loop(void);								// 撮影ループ本体(別スレッド)
	bool    establishSession(void);					// startShooting+M設定+設定値テーブル構築(開始/再接続で使用)
	// 1コマぶんの測光(実装は apiBase::meterScene / meterHere)。結果の診断をログ用メンバへ展開し、
	// 差分適用キャッシュ(lastXxxApplied_)と実機の整合を取る。
	//  haveShot=false は「まだ1コマも撮っていない」(撮影窓手前の初期収束)。カメラ実装は
	//  撮影画像を手がかりにできないので別の測り方をするが、返る値の意味は同じである。
	//  return: 測光値が得られたか(mr.ok)。
	bool    meterFrame(const hgc::exposure& shotExp, apiBase::meterResult& mr, bool haveShot);
	// カメラ実装が返す「場面の明るさ」(露出非依存)を、その露出で撮ったときのリニア輝度へ投影する。
	double        linearAtExposure(double sceneRef, const hgc::exposure& e) const;
	// 目標との差(段)から、このコマで踏む 1/3 段ステップ数を決める(1〜kMaxCatchUpStops相当)。
	// 1歩(1/3段)動かすとヒステリシス帯の反対側へ飛び出すなら true(=このコマは動かさない)。
	// 帯が歩幅より狭い制御方法(夕日/朝日=0.3段)で必ず起きていた往復振動の防止。
	// 測光失敗のログ文を作る(待ちのどの通信でつまずいたかを含める。2026-07-30 診断)。
	// シャッターを切る(503の間は締め切りまで粘る。503は接続断に数えない)。
	//  quick=true は粘らず1回だけ試す(既にカメラが応答しないと分かっている状態での追い打ち防止)。
	errCode       fireShutter(const hgc::exposure& shotExp, double intervalSec, int& failStreak,
	                          bool quick = false);
	void          meterLostMsg(const apiBase::meterResult& mr, char* buf, size_t len) const;
	// ヒステリシス帯の実効値(1歩=1/3段を下限とする。設定は書き換えない)。
	// 移動平均バッファから「いまの場面の明るさ」を推定する(平均の遅れを傾きで補う)。
	double        sceneNowFromBuf(const std::vector<double>& buf) const;
	double        effHysteresis(double raw) const;
	// 反転の抑制(2026-07-30): 露出を1歩変えると測光値が0.30段ずれ、移動平均が異なる露出の
	// 値を混ぜるため、直後の逆向きは信用できない。抑制中の反転には帯を超える差を要求する。
	bool          allowStep(int dir, double needStops, double bandStops) const;
	void          noteStep(int dir);		// 動かした向きを記録し抑制期間を張る
	void          resetStepLock(void);	// 抑制状態を捨てる
	int           lastStepDir_ = 0;	// 直前に動かした向き(-1=暗く +1=明るく 0=なし)
	int           stepLock_    = 0;	// 反転を抑える残りコマ数(0=抑制なし)
	bool          wouldOvershoot(double needStops, double bandStops) const;
	int           stepsToClose(double needStops) const;
	// 実際にカメラへ適用できている露出を返す。lastXxxApplied_ は「その軸の設定が成功したときだけ」
	// 更新されるので、一部の軸だけ失敗した場合も含めて実機の状態を正しく表す。
	hgc::exposure appliedExposure(void) const;
	// 実際にカメラへ乗っている露出を内部の記録だけから復元する(通信しない)。
	// 1枚目の適用が失敗したときに「撮ったつもりの露出」ではなく実機の状態を使うため。
	hgc::exposure appliedOrConverge(void) const;
	hgc::exposure convergeLastApplied_{};	// 初期収束が最後に適用できた露出(=失敗時のカメラの状態)
	// HTTPを伴うカメラ操作の失敗メッセージに、直近のHTTPステータスと応答本文を添える。
	//  "actShutter http=503 During shooting or recording" / "actShutter http=応答なし"
	// 撮影ループ中はライブビューを掴まない(不要な方式のときだけ離す)。
	void          releaseLiveView(void);
	std::string   withHttpDetail(const char* what) const;
	errCode applyExposureChanged(const hgc::exposure& exp);	// 変更のあった ss/iso/fn だけを適用
	// 露出設定を budgetMs まで kApplyRetryMs 間隔でリトライ。tries=試行回数を返す。
	errCode applyWithRetry(const hgc::exposure& exp, int& tries, int budgetMs = kApplyMaxMs);
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

	// --- 測光の診断(ログ用。実体は apiBase::meterResult から meterFrame が展開する) ---
	int  meterMs_  = -1;	// 直近の測光取得(rdy+alz)の実測ms。コマ毎にリセットしログへ出す(計測用)
	bool meterOk_  = true;	// 直近の測光の成否。シャッター後にまとめてログ
	int  meterTry_ = 0;	// 直近の測光試行回数(1=一発成功。>1=リトライした)
	uint64_t lvTimeMs_ = 0;	// 直近の測光フレームのカメラ側取得時刻[ms](0=不明)
	int      staleSkip_ = 0;	// このコマで「古い」と判定して捨てられた回数(0=無し)
	uint32_t histSum_ = 0;	// 直近の測光ヒストグラムの内容チェックサム(0=測光なし)
	double   lvP99_  = -1.0;	// ヒストの明るい側1%点(<0=測光なし)
	double   lvPMax_ = -1.0;	// 画素のある最も明るいビン
	std::string meterSsUsed_;			// 今コマ実際に使った測光ss(ログ用。空=撮影ssのまま測光した)
	int         meterSettleMs_ = -1;	// 今コマの「Tv変更→LV反映」の待ち[ms](-1=切替なし)
	bool        lvPinnedLog_    = false;	// 張り付き判定(ログ用)
	bool        meterUsableLog_ = true;		// 測光値を露出制御に使えたか(ログ用。false=帯の外で据え置き)
	double      lvMeanLinLog_ = -1.0;	// 測光統計量の比較用(2026-08-08 診断)
	double      lvP75Log_ = -1.0, lvP90Log_ = -1.0;
	double      lvSatLog_ = -1.0;
	int         meterWaitMs_ = -1, meterFetchMs_ = -1, meterDecodeMs_ = -1, meterFetchTries_ = 0;	// 測光所要の内訳(ログ用)
	int         meterVia_ = 0;		// 何で測ったか(apiBase::via_*。レポートの件数用)
	// カメラ実装からの申告: 直前のコマの撮影結果が現れなかった(撮れていない疑い)。
	// 測光しないコマでは判定できないので false のまま(=据え置き扱い)。
	bool        shotMissing_ = false;
	double      asIsLinear_ = -1.0;		// 切替なしで測ったリニア値(ログ用。切替判断の材料そのもの)
	int         firstApplyTries_ = 0;	// 1枚目の露出適用に要した回数(ログ用。セッション単位)
	convergeInfo converge_{};			// 初期収束の結果(レポート用。セッション単位)
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
