#include "common.h"
#include "captureRunner.h"
#include "osSystemCall.h"
#include "debugOut.h"
#include "astroSched.h"		// ② 太陽高度(sunHoriz)から ev0 中心bmを算出
#include "netThread.h"		// 失敗した HTTP のステータス/応答をログへ添えるため
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace
{
	// 自動露出制御方法の目標ev(露出補正)を取り出す。
	double targetEv(const hgc::ccmBase* c)
	{
		switch (c->type)
		{
		case hgc::ccmType::sunrise: return static_cast<const hgc::ccmSunrise*>(c)->ev;
		case hgc::ccmType::sunset:  return static_cast<const hgc::ccmSunset*>(c)->ev;
		case hgc::ccmType::day:     return static_cast<const hgc::ccmDay*>(c)->ev;
		default:                    return 0.0;
		}
	}

	bool isAuto(hgc::ccmType t)
	{
		return t == hgc::ccmType::sunrise || t == hgc::ccmType::sunset || t == hgc::ccmType::day;
	}

	bool validExposure(const hgc::exposure& e)
	{
		return !e.iso.empty() && !e.ss.empty() && !e.fn.empty();
	}

	// --- 最初の補正(仕様 4.4)の反復収束パラメータ ---
	// 反復回数の上限。旧4回では未収束のまま打ち切られ1.6〜3.3段外れて撮影開始していた
	// (2026-07-24 17:05 実測)。投影方式(下記)は通常2回で収束するが、張り付き探索も含め余裕を持つ。
	constexpr int    kInitConvergeTries    = 8;
	// 時間予算[ms]。収束よりこちらが先に尽きたら最良推定で撮影に入る(開始を無限に遅らせない)。
	constexpr int    kInitConvergeBudgetMs = 25000;
	// 目標 ev への許容[段]。これ以内に入ったら収束終了して撮影に入る(=1枚目から1/3段以内)。
	constexpr double kInitConvergeTolStops = 1.0 / 3.0;
	// 「露光終了直後に測光」型(meterTimingHint.afterShutterClose)のとき、露光終了から
	// この余裕[ms]を置いて準備を始める。露光中は画像取得ができないため。
	constexpr long   kAfterShutterMarginMs = 300;
}

captureRunner::~captureRunner()
{
	stop();
}

void captureRunner::setCallbacks(stateCb s, progressCb p, capturedCb c, errorCb e)
{
	onState_    = std::move(s);
	onProgress_ = std::move(p);
	onCaptured_ = std::move(c);
	onError_    = std::move(e);
}

errCode captureRunner::ready(const hgc::cs& plan, device* dev,
                             const hgc::exposureSmoothing& smooth, int utcOffsetMin)
{
	if (dev == nullptr) { return ERR_HGC_INVALID_ARG; }
	plan_   = plan;
	dev_    = dev;
	smooth_ = smooth;
	off_    = utcOffsetMin;
	return ERR_HGC_OK;
}

errCode captureRunner::start(void)
{
	if (running_)     { return ERR_HGC_INVALID_STATE; }
	if (dev_ == nullptr) { return ERR_HGC_READY; }
	running_ = true;
	ossc::THREAD_FUNC fn = [this](void*) -> errCode { return this->loop(); };
	thread_ = ossc::threadNet(fn, nullptr);
	if (thread_ == nullptr)
	{	// xTaskCreate 失敗(内部RAM断片化)。握りつぶすと「開始したのに撮らない」になるため呼び出し側へ返す。
		running_ = false;
		return ERR_HGC_NET_THREAD;
	}
	return ERR_HGC_OK;
}

// 呼び出しスレッド上で撮影ループを実行する(ヘッダのコメント参照)。ループ終了まで戻らない。
errCode captureRunner::runInline(void)
{
	if (running_)        { return ERR_HGC_INVALID_STATE; }
	if (dev_ == nullptr) { return ERR_HGC_READY; }
	running_ = true;
	errCode e = loop();
	running_ = false;
	return e;
}

errCode captureRunner::stop(void)
{
	if (!running_ && thread_ == nullptr) { return ERR_HGC_OK; }
	running_ = false;
	if (thread_ != nullptr)
	{
		ossc::threadEnd(thread_);	// join して破棄
		thread_ = nullptr;
	}
	return ERR_HGC_OK;	// runInline 実行中は要求のみ(ループは起動スレッド上。join は呼び出し側の threadEnd で行う)
}

void captureRunner::interruptibleSleep(long ms)
{
	while (running_ && ms > 0)
	{
		if (wake_.exchange(false)) { break; }	// ポーク(取得フェーズの60秒待ち前倒し)で即抜ける
		uint32_t chunk = (ms > 100) ? 100u : static_cast<uint32_t>(ms);
		tool::sleep(chunk);
		ms -= chunk;
	}
}

// 取得フェーズ(apiBase未取得)中のみ、待ちを打ち切って即再探索させる(3b: SSDP出現検知時に呼ぶ)。
// 撮影中など取得フェーズ以外では wake_ を立てない(測光待ち等の正規のsleepを乱さない)。
void captureRunner::pokeAcquire(void)
{
	if (dev_ != nullptr && dev_->apiBase == nullptr) { wake_ = true; }
}

// 1コマぶんの測光。**どう測るか**はカメラ依存なので、実装はすべて apiBase 側
// (cameraController::meterScene / meterHere)にある。ここが知っているのは結果だけである。
// ここで行うのは:
//  ・結果の診断をログ用メンバへ展開(SHOT/LVHISTログの互換を保つ)
//  ・カメラ露出状態の申告(appliedExp/applyFailed)を差分適用キャッシュへ反映(実機とのズレ防止)
// haveShot=false は「まだ1コマも撮っていない」= 撮影画像を手がかりにできない状況
// (撮影窓の手前の初期収束)。カメラ実装は別の測り方でよい。
bool captureRunner::meterFrame(const hgc::exposure& shotExp, apiBase::meterResult& mr, bool haveShot)
{
	auto keep = [this]() { return running_.load(); };
	if (haveShot) { cameraController::meterScene(*dev_, shotExp, mr, keep); }
	else          { cameraController::meterHere(*dev_, mr, keep); }
	// ログ用診断の展開(従来メンバ互換)。
	meterMs_       = mr.rdyMs;
	meterOk_       = mr.ok;
	meterTry_      = mr.tries;
	histSum_       = mr.histSum;
	lvTimeMs_      = mr.lvTimeMs;
	staleSkip_     = mr.staleSkip;
	lvP99_         = mr.p99;
	lvPMax_        = mr.pMax;
	meterSsUsed_   = mr.meterSsUsed;
	meterSettleMs_ = mr.settleMs;
	lvPinnedLog_   = mr.pinned;
	meterUsableLog_ = mr.usable;
	lvMeanLinLog_ = mr.meanLin; lvP75Log_ = mr.p75; lvP90Log_ = mr.p90; lvSatLog_ = mr.satRatio;
	shotMissing_   = mr.shotMissing;
	meterWaitMs_   = mr.waitMs;
	meterFetchMs_  = mr.fetchMs;
	meterDecodeMs_ = mr.decodeMs;
	meterFetchTries_ = mr.fetchTries;
	asIsLinear_    = mr.asIsLinear;
	// カメラ露出状態の整合(契約: 測光のために露出を触ったら必ず申告される)。
	if (!mr.appliedExp.fn.empty())  { lastFnApplied_  = mr.appliedExp.fn; }
	if (!mr.appliedExp.ss.empty())  { lastSsApplied_  = mr.appliedExp.ss; }
	if (!mr.appliedExp.iso.empty()) { lastIsoApplied_ = mr.appliedExp.iso; }
	if (mr.applyFailed)
	{	// 適用失敗=遅延適用の恐れ(IMG_3920事故)。適用記憶を捨て、次の適用で全軸を必ず再送させる。
		lastFnApplied_.clear(); lastSsApplied_.clear(); lastIsoApplied_.clear();
		if (onError_) { onError_(ERR_HGC_RDY_METARING, "meter exposure apply failed → next apply resends all"); }
	}
	return mr.ok;
}

// 「場面の明るさ」を、その露出で撮ったときのリニア輝度へ投影する。
//  測光は撮影とは別の条件で行われるので、測光値をそのまま撮影露出の目標と比べてはいけない
//  (段差ぶんずれ、比較が永久に閉じず露出が暴走する)。カメラ実装が返す sceneRef は露出に
//  依存しない場面の明るさなので、こちらで候補露出へ投影してから比べる(土俵合わせ)。
double captureRunner::linearAtExposure(double sceneRef, const hgc::exposure& e) const
{
	if (sceneRef <= 0.0) { return -1.0; }
	if (!validExposure(e)) { return sceneRef; }
	return sceneRef * std::pow(2.0, expo::brightnessStops(e, tables_));
}

// HTTP を伴うカメラ操作の失敗メッセージに、直近の HTTP 失敗の詳細を添える。
//  例) "actShutter http=503 During shooting or recording" / "actShutter http=応答なし"
//  status>0 = カメラが断った(理由は応答本文に出る) / status=0 = そもそも届かなかった。
//  この区別が無いために 2026-07-21 の actShutter 失敗(code=3)の原因を特定できなかった。
//  失敗した呼び出しの直後に使うこと(それが直近の失敗である前提)。
// 撮影ループ中はライブビューを掴まない(2026-07-30 実験1)。
//
// ライブビューを開始したままだと、カメラ本体のMenu操作が busy で弾かれる = カメラを
// 占有し続けている。サムネ測光ではループ中にライブビューを使わない(必要なのは撮影前の
// 初期収束だけ)ので、用が済んだら離す。R10 が数十分で無応答になる件の切り分けを兼ねる。
void captureRunner::releaseLiveView(void)
{
	if (dev_ == nullptr || dev_->apiBase == nullptr) { return; }
	if (cameraController::liveViewNeededWhileCapturing(*dev_)) { return; }	// LV方式なら掴んだまま
	cameraController::stopLiveView(*dev_);	// 失敗しても撮影は続ける(次コマで測光できる方式なので)
}

std::string captureRunner::withHttpDetail(const char* what) const
{
	int status = 0;
	std::string body;
	netThread::lastHttpFailure(status, body);
	for (auto& c : body) { if (c == '\r' || c == '\n' || c == '\t') { c = ' '; } }	// ログは1行
	char buf[220];
	// status==0(応答なし)のときこそ理由が要る。従来はここで body を捨てていたため、
	// 「TCPが繋がらない(こちら側)」のか「繋がったがカメラが返さない(カメラ側)」のかを
	// 区別できなかった(2026-08-05)。プラットフォーム層が body に理由を載せる。
	if (status > 0)        { std::snprintf(buf, sizeof(buf), "%s http=%d %s", what, status, body.c_str()); }
	else if (!body.empty()){ std::snprintf(buf, sizeof(buf), "%s http=応答なし %s", what, body.c_str()); }
	else                   { std::snprintf(buf, sizeof(buf), "%s http=応答なし", what); }
	return std::string(buf);
}

// 実際にカメラへ適用できている露出。lastXxxApplied_ は各軸の設定が成功したときだけ更新されるので、
//  一部の軸だけ失敗した場合(実測: ISOは通ったが ss だけ通らない)も実機の状態を正しく表す。
hgc::exposure captureRunner::appliedExposure(void) const
{
	hgc::exposure e{};
	e.iso = lastIsoApplied_;
	e.ss  = lastSsApplied_;
	e.fn  = lastFnApplied_;
	return e;
}

// 実際にカメラへ乗っている露出を、内部の記録だけから復元する(通信しない)。
//
// 1枚目の露出適用が失敗したときに使う。従来はここで「撮ったつもりの露出」をそのまま
// 撮影露出として扱っていたため、被害が1コマで終わらなかった:
//   カメラは露出X、アプリは露出Yのつもり
//    → そのコマの測光値を Y で割り戻すので場面の明るさを取り違える
//    → 次コマの露出も誤る
// 実機の状態を使えば測光が正しい土俵に乗り、次コマから正常へ戻れる。
//
//  ・lastXxxApplied_ は「その軸の適用が成功したときだけ」更新されるので、一部の軸だけ
//    通った場合(fnは通ったがisoは失敗など)も軸ごとに正しく表せる。
//  ・空の軸は、初期収束が最後に適用できた露出のまま(カメラはそこから動いていない)。
// 通信が失敗している最中にカメラへ問い合わせに行かずに済むのがこの方式の利点。
hgc::exposure captureRunner::appliedOrConverge(void) const
{
	hgc::exposure e = this->appliedExposure();
	if (e.fn.empty())  { e.fn  = convergeLastApplied_.fn; }
	if (e.ss.empty())  { e.ss  = convergeLastApplied_.ss; }
	if (e.iso.empty()) { e.iso = convergeLastApplied_.iso; }
	return e;
}

// 目標との差(段)から、このコマで踏む 1/3 段ステップ数を決める。
//  定常時は1ステップ(=1/3段)に留めてフリッカーを抑え、大きくずれているときだけ速く詰める。
// シャッターを切る。カメラが「記録中(503 Device busy)」を返している間は、そのコマの
// 締め切りまで粘る。
//
// 【2026-07-30 R10 実機で判明】RAW記録中のカメラは actShutter に 503 {"message":"Device busy"}
//  を返す。従来は POST 2回を間髪入れず投げて即失敗扱い(待ち実質0秒)、それが3コマ連続すると
//  切断と判定していた。実測では 10:09:34 に 503 が返った次のコマ(10:09:37)は正常に撮れており、
//  数秒粘れば1コマも落とさずに済んでいた。30秒で「カメラが見つかりません」になっていた。
//
//  もう一点、503 は「カメラが応答している」証拠なので接続断に数えてはいけない。従来は数えて
//  いたため、つながっているカメラを切断と判定していた(直後のログに「再接続成功」が並ぶ)。
//  接続断として数えるのは応答が返らなかった場合(status<=0)だけにする。
errCode captureRunner::fireShutter(const hgc::exposure& shotExp, double intervalSec, int& failStreak,
                                   bool quick)
{
	// そのコマに使える時間 = 周期 - 露光 - 余裕。次のコマの時刻を追い越さないための上限。
	double ssSec = expo::parseValue(shotExp.ss, expo::expoKind::ss);
	if (!(ssSec > 0.0)) { ssSec = 0.0; }
	int budgetMs = static_cast<int>(intervalSec * 1000.0 - ssSec * 1000.0) - kShutterMarginMs;
	if (budgetMs > kShutterBusyMaxMs) { budgetMs = kShutterBusyMaxMs; }
	if (budgetMs < 0)                 { budgetMs = 0; }
	// 既に「撮れていない」と判定済みのカメラには粘らない。busy のカメラへシャッターを
	// 投げ続けると固着が深まる(2026-08-11 実測)。復帰を拾うため1回だけは投げる。
	if (quick) { budgetMs = 0; }

	void*   t0     = tool::startElapse();
	errCode err    = ERR_HGC_OK;
	int     status = 0;
	int     tries  = 0;
	std::string body;
	for (;;)
	{
		++tries;
		err = cameraController::actShutter(*dev_);
		if (err == ERR_HGC_OK) { failStreak = 0; return err; }
		netThread::lastHttpFailure(status, body);
		if (!running_.load())                                 { break; }
		if (static_cast<int>(tool::getElapse(t0)) >= budgetMs) { break; }
		this->interruptibleSleep(kShutterRetryMs);
	}
	if (onError_)
	{
		char eb[240];
		std::snprintf(eb, sizeof(eb), "%s (try=%d %dms budget=%dms)",
		              this->withHttpDetail("actShutter").c_str(), tries,
		              static_cast<int>(tool::getElapse(t0)), budgetMs);
		onError_(err, eb);
	}
	if (status <= 0) { ++failStreak; }	// 応答なし=本当に届いていない。503等は接続断ではない
	return err;
}

// 測光失敗のログ文を作る。原因を後から特定できるよう、どこでつまずいたかまで残す。
//  stage/step の番号の意味はカメラ実装が決める(apiBase::meterResult のコメントに一覧がある)。
//  http 0=応答なし(届いていない) 正数=カメラが断った(その番号) -1=応答の中身が想定外 -2=JSONとして壊れている
void captureRunner::meterLostMsg(const apiBase::meterResult& mr, char* buf, size_t len) const
{
	// waitStep の意味はカメラ実装が決める(apiBase::meterResult のコメント参照)。ここは
	// 番号をそのまま出し、よく出るものにだけ短い名前を添える。
	const char* st = "?";
	switch (mr.waitStep)
	{
		case 0: st = "-";        break;
		case 1: st = "noevent";  break;	// 登録通知APIが使えない
		case 6: st = "pollng";   break;	// 通知の取得が失敗した
		case 7: st = "timeout";  break;	// 時間内に通知が来なかった
		default: break;
	}
	int n = std::snprintf(buf, len, "metering lost (stage=%d %dms try=%d, keep exposure)",
	                     mr.failStage, mr.rdyMs, mr.tries);
	if (mr.waitStep != 0 && n > 0 && static_cast<size_t>(n) < len)
	{
		n += std::snprintf(buf + n, len - n, " where=%s(%d) http=%d", st, mr.waitStep, mr.waitHttp);
		if (!mr.waitBody.empty() && n > 0 && static_cast<size_t>(n) < len)
		{	std::snprintf(buf + n, len - n, " resp=%s", mr.waitBody.c_str()); }
	}
}

// 移動平均バッファから「いまの場面の明るさ」を推定する(2026-08-02)。
//
// 【なぜ単純平均ではいけないか】n点の単純平均は (n-1)/2 コマ分だけ遅れた値になる。
//  遅れ[段] = 場面の変化速度[段/コマ] × (n-1)/2 なので、変化が速いほど大きく膨らむ。
//  実測(2026-08-01 postNight): 空が 0.09段/コマ の間は遅れ 0.18段 で目立たないが、
//  夜明けが加速して 0.46段/コマ になると遅れは 0.92段 になり、写真は目標より 1.45段
//  明るくなった(IMG_4627)。「一部の時間帯だけ明るくずれる」のはこれが原因で、
//  ヒステリシス帯(一定の +0.5段)だけでは説明できない。
//
// 【対策】平均に「傾き × (n-1)/2」を足し戻して現在値を推定する。
//  傾きは最小二乗ではなく **隣り合う差分の中央値** で求める。理由は一過性の光への強さ:
//    1コマだけ2段明るくなった場合(車のライト等)の推定値
//      単純平均      +0.40段
//      最小二乗の傾き +1.20段 … 3倍に過剰反応し、光が消えた後 -0.40段 へ逆振れする
//      差分の中央値  +0.40段 … 外れ値は4つの差分のうち2つにしか効かないので無視できる
//  夜明けのような一定速度の変化には、どちらの傾きでも遅れ 0 になる。
//
// 計算は段(log2)で行う。場面の明るさは掛け算で変化するので、log空間なら一定速度の
// 変化が直線になり外挿が正確になる(線形空間で外挿すると加速側で行き過ぎる)。
//
// 【残る弱点】変化が折り返す瞬間は直前の傾きを外挿し続けるので少し行き過ぎる
//  (夜明けが平坦に転じる場面で +0.18段 程度)。外挿量は kSceneLeadMaxStops で頭打ちにする。
//  return : 推定した場面の明るさ(リニア)。有効な値が無ければ -1
double captureRunner::sceneNowFromBuf(const std::vector<double>& buf) const
{
	std::vector<double> l;
	l.reserve(buf.size());
	for (double v : buf) { if (v > 0.0) { l.push_back(std::log2(v)); } }
	if (l.empty()) { return -1.0; }

	double mean = 0.0;
	for (double v : l) { mean += v; }
	mean /= static_cast<double>(l.size());
	// 差分が2つ未満だと中央値が外れ値に耐えられない。傾きは使わず平均のまま返す。
	if (l.size() < 3) { return std::pow(2.0, mean); }

	std::vector<double> d;
	d.reserve(l.size() - 1);
	for (size_t i = 1; i < l.size(); ++i) { d.push_back(l[i] - l[i - 1]); }
	std::sort(d.begin(), d.end());
	const size_t m = d.size() / 2;
	const double slope = (d.size() % 2 != 0) ? d[m] : (d[m - 1] + d[m]) / 2.0;

	double lead = slope * (static_cast<double>(l.size()) - 1.0) / 2.0;
	if (lead >  kSceneLeadMaxStops) { lead =  kSceneLeadMaxStops; }
	if (lead < -kSceneLeadMaxStops) { lead = -kSceneLeadMaxStops; }
	return std::pow(2.0, mean + lead);
}

// ヒステリシス帯の実効値。1歩(1/3段)より狭い帯は構造的に成立しない(どう動かしても帯の
// 内側へ入れないので、補正するたび必ず反対側へ飛び出す)。よって1歩を下限として扱う。
// 設定そのものは書き換えない(ユーザーの値は保存されたまま、使うときだけ下限を当てる)。
double captureRunner::effHysteresis(double raw) const
{
	const double lo = kMinHysteresisStops;
	return (raw > lo) ? raw : lo;
}

// 直前に動かした向きの逆へ動いてよいか(反転の抑制)。
//
// 【2026-07-30 実測に基づく】露出を1歩(1/3段)変えると、同じ場面でも測光値が 0.30段 ずれる
// (R100 夕日13回=中央0.30段 / 朝日9回=中央0.32段)。原因はサムネイルがカメラ現像のJPEGで
// あることだが、ピクチャースタイルが auto でコマ毎にトーンカーブが変わり、ALO は CCAPI に
// 出てこないため、こちら側では取り除けない(単一ゲインでの補正も実ログで検証して否決した)。
// さらに移動平均は「異なる露出で測った値」を混ぜるので、1歩動かした直後の平均は最大
// (n-1)/n × 0.30段 の偏りを持つ。
// そこで、平均バッファが新しい露出の値で埋まり直すまでの間は、逆向きへ動くのに
// 「帯を丸ごと超える差」を要求する。同じ向きへの追従と、急変(差が大きい)は妨げない。
bool captureRunner::allowStep(int dir, double needStops, double bandStops) const
{
	if (dir == 0)                                { return false; }
	if (lastStepDir_ == 0 || dir == lastStepDir_) { return true; }	// 同じ向き=いつでも動く
	if (stepLock_ <= 0)                          { return true; }	// 偏りは抜けた
	(void)bandStops;
	return std::fabs(needStops) >= kReversalGuardStops;	// 反転は本物の急変のときだけ通す
}

// 動かした向きを記録し、反転を抑える期間を張る(長さ=移動平均のコマ数)。
void captureRunner::noteStep(int dir)
{
	lastStepDir_ = dir;
	stepLock_    = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
}

// 反転抑制の状態を捨てる(制御方法の切替や測光失敗で平均を捨てるときに合わせて呼ぶ)。
void captureRunner::resetStepLock(void)
{
	lastStepDir_ = 0;
	stepLock_    = 0;
}

// 踏み出すと帯の反対側へ飛び出すなら動かない(デッドバンド。2026-07-29 振動の根治)。
//  ヒステリシス帯より1歩(1/3段)が大きいと、補正のたびに必ず反対側へ越えて往復し続ける。
//  夕日/朝日は帯0.3段<歩幅0.333段のため必ず振動していた(日中は帯1.0段なので発動しない)。
//  need=目標までの差[段], band=ヒステリシス全幅[段]。true=このコマは動かさない。
bool captureRunner::wouldOvershoot(double needStops, double bandStops) const
{
	const double a = std::fabs(needStops);
	if (a >= kExposureStepStops) { return false; }	// 1歩以上ずれている→動かすべき
	// 1歩動かすと |a - 1歩| だけ反対側へ出る。それが帯の外なら動かさない。
	return (kExposureStepStops - a) > (bandStops / 2.0);
}

int captureRunner::stepsToClose(double needStops) const
{
	const double a = std::fabs(needStops);
	int n = static_cast<int>(a / kExposureStepStops + 0.5);
	const int maxN = static_cast<int>(kMaxCatchUpStops / kExposureStepStops + 0.5);
	if (n < 1)    { n = 1; }
	if (n > maxN) { n = maxN; }
	return n;
}


// 露出設定を最大 kApplyMaxMs まで kApplyRetryMs 間隔でリトライする。
//  カメラは撮影/記録中に設定PUTを 503 "During shooting or recording"/"Device busy" で即拒否する。
//  ここで諦めるとカメラは古い露出のまま撮り続け、アプリの露出モデルと実機がズレたまま復帰できない
//  (2026-07-16 の通し撮影で露出設定の90%が失敗し夜明けが白飛びした)。通るまで粘る。
//  applyExposureChanged は失敗した項目の lastXxxApplied_ を更新しないので、リトライは自然に未適用分だけを再送する。
errCode captureRunner::applyWithRetry(const hgc::exposure& exp, int& tries, int budgetMs)
{
	void*   t0 = tool::startElapse();
	tries = 0;
	errCode e  = ERR_HGC_OK;
	for (;;)
	{
		++tries;
		e = this->applyExposureChanged(exp);
		if (e == ERR_HGC_OK) { return ERR_HGC_OK; }
		if (!running_.load()) { return e; }							// 中止
		if (static_cast<int>(tool::getElapse(t0)) >= budgetMs) { return e; }		// 上限で諦め
		interruptibleSleep(kApplyRetryMs);
	}
}

// 露出を「変更のあった項目だけ」適用する(タイマ方式tm0)。通常は ss/iso/fn の1つだけ、
// ccm切替の瞬間のみ複数、暗限界張り付き等で変化なしなら0本になる。直近適用値と比較して差分のみ送る。
// establishSession でキャッシュをクリアするので、接続/再接続直後の初回はフル適用になる。
errCode captureRunner::applyExposureChanged(const hgc::exposure& exp)
{
	errCode e = ERR_HGC_OK, r;
	if (exp.fn != lastFnApplied_)   { r = cameraController::setFNumber(*dev_, exp.fn); if (r == ERR_HGC_OK) { lastFnApplied_ = exp.fn; } else { e = r; } }
	if (exp.ss != lastSsApplied_)   { r = cameraController::setSS(*dev_, exp.ss);      if (r == ERR_HGC_OK) { lastSsApplied_ = exp.ss; } else { e = r; } }
	if (exp.iso != lastIsoApplied_) { r = cameraController::setIso(*dev_, exp.iso);    if (r == ERR_HGC_OK) { lastIsoApplied_ = exp.iso; } else { e = r; } }
	return e;
}

// monotonic 基準 mono(tool::startElapse のハンドル)からの経過が targetMs に達するまで待つ。
// 撮影周期の絶対アンカー用。100ms刻みで running_ を見て中断可。既に過ぎていれば即戻る。
void captureRunner::sleepUntilElapse(void* mono, long targetMs)
{
	while (running_)
	{
		long rem = targetMs - static_cast<long>(tool::getElapse(mono));
		if (rem <= 0) { break; }
		uint32_t chunk = (rem > 100) ? 100u : static_cast<uint32_t>(rem);
		tool::sleep(chunk);
	}
}

const hgc::ccmWindow* captureRunner::activeWindow(long long nowSec) const
{
	for (const auto& w : plan_.ccmList)
	{
		long long s = hgc::toUnixUtc(w.start, off_);
		long long e = hgc::toUnixUtc(w.end, off_);
		if (nowSec >= s && nowSec < e) { return &w; }
	}
	return nullptr;
}

hgc::exposure captureRunner::nightGoalAfter(long long nowSec) const
{
	// nowSec 以降で最初の夜間窓の固定露出を返す。無ければ無効値。
	const hgc::ccmWindow* best = nullptr;
	long long bestStart = 0;
	for (const auto& w : plan_.ccmList)
	{
		if (!w.ccm || w.ccm->type != hgc::ccmType::night) { continue; }
		long long s = hgc::toUnixUtc(w.start, off_);
		if (s >= nowSec && (best == nullptr || s < bestStart)) { best = &w; bestStart = s; }
	}
	if (best) { return best->ccm->limitBright; }	// 固定露出(limitBright==limitDark)
	// 夜間ウィンドウがスケジュールに無くても(終了時刻が夜間より前でも)、夜間前/後移行の
	// クランプ・基準として夜間プリセット露出を返す(仕様3.7/3.9。buildSchedule が設定済み)。
	return plan_.nightFixedExposure;
}

// 最初の補正(仕様 4.4)。撮影開始直後は初期露出が不定なので、1枚目の露出を測光で決める。
//
// 【ここは「どう測るか」を知らない(2026-08-13)】撮影窓の手前ではまだ1コマも撮っていないので、
//  カメラ実装は撮影画像以外の手段で測ることになる(CCAPIならライブビュー)。その手段の都合
//  ——測るのに使う露出をどう選ぶか、白飛び/黒潰れをどうずらして抜けるか、反映を何ms待つか——は
//  すべて cameraController::meterHere の中にある。ここが受け取るのは「露出非依存の場面の
//  明るさ」sceneRef だけで、旧実装がここに持っていた測光ssの操作は無くなった。
//
// 収束のやり方(2026-07-26 改定のまま):
//  1. 場面の明るさ sceneRef を測る
//  2. 候補露出で撮ったらどう写るかを投影し、目標リニア輝度へ「一気に」寄せる(二分探索不要)
//  3. もう一度測って誤差 ≤ kInitConvergeTolStops(1/3段) を確認できたら収束
// 通常は測光2回で1/3段以内に入る。収束するまで撮影は始めない(時間予算内)。
// ctl は呼び出し前に init 済みであること。戻り値=1枚目の露出(ctl もその値になる)。
hgc::exposure captureRunner::initialConverge(expo::exposureCtl& ctl, const hgc::exposure& initial, double evT)
{
	ctl.setCurrent(initial);	// 仕様 4.4 の基準(iso/ss/fn)から開始する
	void* t0 = tool::startElapse();

	// 【診断 2026-08-05】収束が「できたのか・できないまま撮り始めたのか」を後から切り分ける。
	// 毎回ログへ出すと溢れるので、回数と各回の所要をためてループの後に1行だけ出す。
	// 例 "測光ms=1180,860!,910" ('!'=その回は値を採用できなかった)。
	int     applyNg    = 0;			// 測光のためのカメラ操作が失敗した回数(実装からの申告)
	int     meterNg    = 0;			// 測光できず値を採用できなかった回数
	bool    converged  = false;		// 目標へ収まった(または露出限界に到達した)か
	errCode lastErr    = ERR_HGC_OK;	// 最後に失敗したときのコード
	char    meterMs[112] = {0};
	int     meterMsLen = 0;

	// 収束の試行回数(measure できた回だけ数える)。測光できなかった回は「やり直し」であって
	// 収束の1歩ではないので、通信の失敗で収束の機会を奪わない。全体は時間予算で頭打ちにする。
	int step = 0;
	while (step < kInitConvergeTries && running_)
	{
		if (static_cast<int>(tool::getElapse(t0)) >= kInitConvergeBudgetMs) { break; }	// 予算切れ→最良推定で開始

		apiBase::meterResult mr;
		void*         tm = tool::startElapse();
		const errCode me = cameraController::meterHere(*dev_, mr, [this]() { return running_.load(); });
		const long    ms = static_cast<long>(tool::getElapse(tm));
		const bool    okThis = (me == ERR_HGC_OK) && mr.ok && mr.usable && (mr.sceneRef > 0.0);
		if (meterMsLen < static_cast<int>(sizeof(meterMs)) - 12)
		{
			meterMsLen += std::snprintf(meterMs + meterMsLen, sizeof(meterMs) - meterMsLen,
			                            "%s%ld%s", (meterMsLen > 0) ? "," : "", ms, okThis ? "" : "!");
		}

		// 測光のためにカメラの露出が動いていたら、実機の状態としてここへ写す。
		//  ・差分適用キャッシュ: 次の適用で送るべき軸を取りこぼさないため
		//  ・convergeLastApplied_: 1枚目の適用が失敗したとき、通信せず実機の露出を復元するため
		if (!mr.appliedExp.fn.empty())  { lastFnApplied_  = mr.appliedExp.fn;  convergeLastApplied_.fn  = mr.appliedExp.fn; }
		if (!mr.appliedExp.ss.empty())  { lastSsApplied_  = mr.appliedExp.ss;  convergeLastApplied_.ss  = mr.appliedExp.ss; }
		if (!mr.appliedExp.iso.empty()) { lastIsoApplied_ = mr.appliedExp.iso; convergeLastApplied_.iso = mr.appliedExp.iso; }
		if (mr.applyFailed)
		{
			// 測光露出を乗せられていない = カメラは別の露出のまま。この状態で得た値は
			// 割り戻す分母が嘘になり、場面の明るさを取り違える。使わずにやり直す。
			++applyNg; lastErr = me;
			interruptibleSleep(kApplyRetryMs);
			continue;	// step は増やさない(収束の1歩として数えない)
		}
		if (!okThis) { ++meterNg; lastErr = me; continue; }	// 測光失敗 → 予算内で再試行

		++step;
		// ev0 のリニア輝度は環境光依存(§4.3.3/4.3.4)。測光値と測光時の露出から都度求める。
		const hgc::exposure& mex = validExposure(mr.meterExp) ? mr.meterExp : ctl.current();
		const double lin0      = expo::ev0LinearForMeasure(mr.linear, mex, ev0cfg_);
		const double linT      = expo::linearFromEvBase(evT, lin0);		// 目標リニア輝度
		const double curB      = expo::brightnessStops(ctl.current(), tables_);
		const double predicted = mr.sceneRef * std::pow(2.0, curB);		// 候補露出で写る明るさ
		if (predicted <= 0.0 || linT <= 0.0) { break; }
		const double err = std::log2(predicted / linT);	// +:明るすぎ / -:暗すぎ

		if (std::fabs(err) <= kInitConvergeTolStops) { converged = true; break; }	// 収束(1枚目から1/3段以内)

		ctl.applyStops(-err);	// 目標へ直接投影(限界・1/3段テーブルへは applyStops がクランプ)
		const double newB = expo::brightnessStops(ctl.current(), tables_);
		// 露出限界に当たって動けない=これ以上詰められない。狙いには届かないが「出せる最良」に
		// 到達しているので、収束できなかった(時間切れ)とは区別して扱う。
		if (std::fabs(newB - curB) < 1e-6) { converged = true; break; }
		// 次の反復で新しい測光により誤差を再確認する(確認が取れたら上で break)。
	}

	// 撮影レポート用に結果を残す。「収束できたのか、できないまま撮り始めたのか」が要点。
	converge_.steps   = step;
	converge_.applyNg = applyNg;
	converge_.meterNg = meterNg;
	converge_.outcome = (step == 0) ? 2 : (converged ? 0 : 1);

	if (onError_)
	{
		char eb[280];
		if (applyNg > 0 || meterNg > 0)
		{
			// 失敗した回は値を採用せずやり直している。step=実際に収束へ使えた回数。
			// step=0 は「一度も測れなかった」= 露出は基準値のまま撮り始めることを意味する。
			std::snprintf(eb, sizeof(eb), "%s (収束: 有効%d回 適用失敗%d 測光失敗%d 測光ms=%s)",
			              this->withHttpDetail("初期収束").c_str(), step, applyNg, meterNg, meterMs);
		}
		else
		{
			std::snprintf(eb, sizeof(eb), "初期収束 有効%d回 測光ms=%s", step, meterMs);
		}
		onError_(lastErr, eb);
	}
	// 収束中にカメラ実装が動かした露出は差分適用キャッシュの外である。ここで無効化して
	// 次の適用に全軸を必ず送らせる(キャッシュと実機がズレたまま1枚目を撮る事故の防止)。
	lastFnApplied_.clear(); lastSsApplied_.clear(); lastIsoApplied_.clear();
	return ctl.current();	// 最後の投影(または収束点)が最良推定=撮影1枚目の露出
}


// ライブビュー開始 + M設定(ダイアル無視/オートパワーオフ抑止) + 設定可能値テーブル構築。
// 撮影開始時と、撮影中の再接続後の両方で使う。return: ライブビュー開始に成功したか。
bool captureRunner::establishSession(void)
{
	// カメラ未取得(apiBase==nullptr)ならセッションは張れない(3a: 未検出許容)。
	if (dev_ == nullptr || dev_->apiBase == nullptr) { return false; }
	// 再接続でライブビューのセッションが作り直されるので、古いフレーム判定の基準を捨てる。
	// 前セッションの時刻と比べると、復帰後の正常なフレームまで「古い」と誤判定してしまう。
	// 測光の内部状態(学習値・鮮度基準・撮影画像の登録通知の待ち方)はカメラ依存層が持つ。
	// セッションが作り直されるのでまとめて捨てさせる。
	cameraController::meterReset(*dev_);

	// 変更分のみ適用のキャッシュをクリア(再接続直後はカメラ状態が不定なので次回フル適用させる)。
	lastFnApplied_.clear(); lastSsApplied_.clear(); lastIsoApplied_.clear();

	// 撮影モードに入る(ライブビュー開始)
	errCode err = cameraController::startShooting(*dev_);
	if (err == ERR_HGC_OK) { this->releaseLiveView(); }	// 初期収束が要るときは中で張り直す
	if (err != ERR_HGC_OK)
	{
		if (onError_) { onError_(err, this->withHttpDetail("startShooting")); }
		return false;
	}

	// カメラを当アプリ都合の設定にする(撮影モードダイアル無視ON+マニュアル露出)。仕様8/CCAPI。
	// getSettings(設定可能値テーブル作成)より前に行う: Av等ではtvのabilityが空になり ss テーブルが
	// 作れないため、Mにしてから設定可能値を取得する。終了時に restoreShootingMode で元へ戻す。
	{
		errCode me = cameraController::setupShootingModeManual(*dev_);
		if (me == ERR_HGC_OK)            { interruptibleSleep(800); }	// モード変更/ability更新の反映待ち(初回rdyShutterの取りこぼし防止)
		else if (me == ERR_HGC_NOT_SUPPORTED) { /* モード変更非対応機。そのまま続行 */ }
		else if (onError_)               { onError_(me, this->withHttpDetail("setupShootingModeManual")); }
	}

	// 設定可能値を取得して設定可能値テーブルを作る(仕様 4.2)
	cmdt::shotRange range;
	if (cameraController::getSettings(*dev_, range) == ERR_HGC_OK &&
	    !range.iso.empty() && !range.ss.empty() && !range.fNum.empty())
	{
		tables_.iso = expo::buildTable(range.iso,  expo::expoKind::iso);
		tables_.ss  = expo::buildTable(range.ss,   expo::expoKind::ss);
		tables_.fn  = expo::buildTable(range.fNum, expo::expoKind::fn);
	}
	else
	{	// 取得失敗時は標準テーブル(レンズのf範囲)でフォールバック
		double fmin = (plan_.lens.fn > 0.0) ? plan_.lens.fn : 1.0;
		tables_ = expo::standardTables(fmin, 32.0);
	}
	return true;
}

errCode captureRunner::loop(void)
{
	if (dev_ == nullptr) { running_ = false; return ERR_HGC_READY; }

	// 3a: カメラ取得+セッション確立フェーズ。撮影要求時にカメラが未検出(apiBase==nullptr)でも
	//     中断せず、取得できるまで NOCAMERA(✖点灯)で探し続ける。約60秒ごとに onReconnect_
	//     (=計画カメラの再探索/直近IP直結)を試み、取得できたら establishSession してループへ入る。
	//     「3回で諦める」旧挙動は廃止し、撮影窓の終了または中止(running_=false)まで無限に試行する。
	while (running_)
	{
		if (dev_->apiBase == nullptr)
		{
			if (onState_) { onState_(ST_NOCAMERA); }
			if (!onReconnect_ || !onReconnect_() || dev_->apiBase == nullptr)
			{
				interruptibleSleep(kKeepAliveSec * 1000);	// 60秒後に再探索(窓終了/中止でsleep中断)
				continue;
			}
		}
		// apiBase 取得済み → ライブビュー/M設定/設定可能値テーブルを張る。
		if (establishSession()) { break; }					// 確立成功 → 撮影ループへ
		// 確立失敗(検出はできたがセッションが張れない=接続断)→ apiBaseを落として再取得からやり直す
		// (旧apiBaseは解放しない設計=リーク許容。詳細は device の所有契約)。
		dev_->apiBase = nullptr;
		interruptibleSleep(kReconnectWaitMs);
	}
	if (!running_) { running_ = false; if (onState_) { onState_(ST_IDLE); } return ERR_HGC_OK; }

	// 初期状態: 撮影窓より前ならカメラ点灯(待機=ST_WAITING)、窓内なら撮影中(点滅=ST_CAPTURING)。
	// 窓入場時に一度だけ CAPTURING へ切り替える(inWindow フラグ)。待機と撮影中をUIで区別するため(指示2)。
	bool inWindow = (static_cast<long long>(std::time(nullptr)) >= hgc::toUnixUtc(plan_.start, off_));
	if (onState_) { onState_(inWindow ? ST_CAPTURING : ST_WAITING); }

	errCode err = ERR_HGC_OK;	// ループ内の各カメラ操作の結果を受ける(actShutter 等)

	// 撮影失敗の連続回数(rdyShutter/actShutter が失敗するとカウント、成功でリセット)。
	int  shootFailStreak = 0;
	// 撮影窓まで待機中の接続維持GETの最終送出時刻。
	long long lastKeepAlive = static_cast<long long>(std::time(nullptr));
	int  waitFailStreak = 0;	// 待機中の keepAlive 連続失敗数(健全性チェック=先回り再接続の判定用)
	bool waitDisconnected = false;	// 待機中に接続断と判定済みか(復帰でセッション張り直しが要る)

	const long long startSec = hgc::toUnixUtc(plan_.start, off_);
	const long long endSec   = hgc::toUnixUtc(plan_.end, off_);
	const double interval = (plan_.interval > 0.0) ? plan_.interval : 15.0;
	int total = static_cast<int>((endSec - startSec) / interval);
	if (total < 1) { total = 1; }

	// 最長ss上限(仕様7.4.2/今回の指示3): ss は「夜間ssを超えない」かつ「撮影周期-2秒以内」。
	// 夜間がスケジュール窓に無くても周期決定パラメータとして守る。各 ctl.init 後に capLongestSs で適用。
	// 最大ss=夜間ss(未設定/過大は安全値へ)。
	const double nightSsSec = expo::parseValue(plan_.nightFixedExposure.ss, expo::expoKind::ss);
	double       maxSs      = (nightSsSec > 0.0) ? nightSsSec : (interval * 0.5);
	if (maxSs >= interval) { maxSs = interval * 0.9; }
	const double maxSsCap   = maxSs;	// 露出の ss 上限=最大ss

	const hgc::ccmWindow* curWin = nullptr;
	expo::exposureCtl autoCtl;		// 自動露出用
	expo::exposureCtl preCtl;		// 夜間前移行用(自動露出→夜間)
	expo::exposureCtl postCtl;		// 夜間後移行用(夜間→次の自動露出)
	std::vector<double> avgBuf;		// リニア輝度の移動平均バッファ
	bool pinPrev = false;			// 前コマで測光の張り付きを検出していたか(カメラ実装からの申告)
	hgc::exposure lastExp{};		// 直近の「測光時の」露出(ev0の逆算専用。ssが測光用に差し替わっている)
	hgc::exposure lastShotExp{};	// 直近に実際に撮影した露出。窓切替の「直前から継続」はこちらを使う
	// (lastExp を使うと切替直後の1コマが測光ssで撮れてしまう。7/24-25実測: 夕日/preNight/postNight
	//  入りが数段暗く、朝の日中入りは+4.7段明るく写った。向きと大きさ=測光ssと撮影ssの差そのもの)
	int meterFailStreak = 0;	// 連続測光失敗数(測光の回復用)
	// 「応答するのに撮れていない」カメラの検出(ヘッダ kMaxNoRecordFrames の説明を参照)。
	int  noRecordStreak = 0;	// 撮影結果が現れなかったコマの連続数
	int  noRecordRounds = 0;	// それが上限に達した回数(1回目は静かに張り直す)
	bool cameraOffline  = false;	// 「オンラインでない」と提示済みか
	int frame = 0;
	double curEvT = 0.0;		// 実効目標ev(項目8: 自動露出→自動露出の切替で 1/3 段/枚 緩やかに移行)
	bool   preNightConverge = false;	// 夜間前移行: 終端へ向けた夜間露出への収束フェーズか

	// nowSec 以降で最初の自動露出窓の撮影制御方法を返す(夜間後移行の収束先)。
	auto nextAutoCcmAfter = [&](long long nowSec) -> const hgc::ccmBase*
	{
		const hgc::ccmWindow* best = nullptr; long long bestStart = 0;
		for (const auto& ww : plan_.ccmList)
		{
			if (!ww.ccm || !isAuto(ww.ccm->type)) { continue; }
			long long s = hgc::toUnixUtc(ww.start, off_);
			if (s >= nowSec && (best == nullptr || s < bestStart)) { best = &ww; bestStart = s; }
		}
		return best ? best->ccm.get() : nullptr;
	};

	// nowSec 時点以前で最後の自動露出窓の撮影制御方法を返す(夜間前移行の直前ccm)。
	auto prevAutoCcmBefore = [&](long long nowSec) -> const hgc::ccmBase*
	{
		const hgc::ccmWindow* best = nullptr; long long bestStart = -1;
		for (const auto& ww : plan_.ccmList)
		{
			if (!ww.ccm || !isAuto(ww.ccm->type)) { continue; }
			long long s = hgc::toUnixUtc(ww.start, off_);
			if (s <= nowSec && s > bestStart) { best = &ww; bestStart = s; }
		}
		return best ? best->ccm.get() : nullptr;
	};

	// nowSec 以降で最初の夜間窓の撮影制御方法(preNightEv 取得用)を返す。
	auto nightCcmAfter = [&](long long nowSec) -> const hgc::ccmNight*
	{
		const hgc::ccmWindow* best = nullptr; long long bestStart = 0;
		for (const auto& ww : plan_.ccmList)
		{
			if (!ww.ccm || ww.ccm->type != hgc::ccmType::night) { continue; }
			long long s = hgc::toUnixUtc(ww.start, off_);
			if (s >= nowSec && (best == nullptr || s < bestStart)) { best = &ww; bestStart = s; }
		}
		return best ? static_cast<const hgc::ccmNight*>(best->ccm.get()) : nullptr;
	};

	// --- 周期正確化(タイマ方式・案P) ---
	//  前フェーズ(撮影窓の kPreConvergeSec 秒前)で初期収束(測光のみ・シャッター無し)して1枚目の露出 pending を確定。
	//  以降は撮影窓開始を0とする monotonic アンカー(mono)からの絶対境界で tm0/tm1 を実行:
	//   tm0=境界: 変更分の露出適用 + 測光(次コマ用) / tm1=境界+offset: シャッター(=周期ピッタリで発光)。
	//  露出計算ブロックは行内のまま流用し、ウォームアップ(初回)と通常コマで同じブロックを1回ずつ通す。
	//  mono/boundaryIdx は「周期の刻み(ペーシング)」専用。ccm選択・露出スケジュール・進捗に渡す文脈時刻は
	//  常に実時刻(std::time)を使う → 開始が過去でも計画を過去から再生せず、再接続の再アンカーでも ccm が巻き戻らない。
	hgc::exposure pending{};			// 各境界(tm0)で適用し tm1 で撮る露出。前フェーズの初期収束で確定
	bool          warmedUp   = false;	// 初期収束が済み pending が確定したか
	void*         lastAnchor = nullptr;	// 直近シャッターの[A](最後の1枚保護用)。①④で絶対アンカー mono/boundaryIdx は廃止

	while (running_)
	{
		long long now = static_cast<long long>(std::time(nullptr));
		if (now >= endSec) { break; }				// 計画終了

		// ①④ 相対アンカー: 各コマは[A](このコマのシャッター起点)から周期後を狙う。撮ったコマの露出=shotExp。
		hgc::exposure shotExp{};				// このコマで撮った露出(=前コマで適用済みの pending)。ログ用
		void*         anchorA   = nullptr;		// [A] このコマの周期基準(準備開始/次シャッター算出に使用)
		uint64_t      shutterMs = 0;
		long          lateMs    = -1;			// このコマのシャッターの周期からの遅れ[ms](0=ぴったり。-1=計測なし)
		int           busyMs    = kBusyNotMeasured;	// 露光終了→測光可(カメラが記録で塞がっていた時間)
		int           leadUsed  = 0;			// このコマで準備に与えたリード[ms](周期 - 準備開始)

		if (!warmedUp)
		{
			// 撮影窓の kPreConvergeSec 秒前まで待機。無通信が続くとカメラがセッションを切るため、
			// 約60秒ごとに無害なGET(keepAlive)を送り、連続失敗なら先回り再接続する(健全性チェック)。
			//  ※keepAlive は撤去不可(2026-06-30 実機: 止めると約53分で脱落・再接続不可)。
			if (now < startSec - kPreConvergeSec)
			{
				if (now - lastKeepAlive >= kKeepAliveSec)
				{
					lastKeepAlive = now;
					const bool alive = (cameraController::keepAlive(*dev_) == ERR_HGC_OK);
					if (!waitDisconnected)
					{
						if (alive) { waitFailStreak = 0; }
						else if (++waitFailStreak >= kWaitMaxFail)
						{	// A-1: まず静かに再接続を試みる。一過性の失敗ならここで復帰しNOCAMERA(✖点灯・スマホ
							//      ポップ)を出さず待機を継続する。復帰できなければ接続断と判定して提示する。
							if (onReconnect_ && onReconnect_() && establishSession()) { waitFailStreak = 0; }	// 静かに復帰(ポップ無し)
							else
							{	waitDisconnected = true;
								if (onError_) { onError_(ERR_HGC_NOT_FOUND, "待機中に接続断を検知 → 先回り再接続"); }
								if (onState_) { onState_(ST_NOCAMERA); }	// 待機中のカメラ未検出(✖点灯)
							}
						}
					}
					if (waitDisconnected)
					{	// 電源復帰等でセッションは失われているため、必ず onReconnect_→establishSession で張り直す。
						if (onReconnect_ && onReconnect_() && establishSession())
						{
							waitDisconnected = false; waitFailStreak = 0;
							if (onState_) { onState_(ST_WAITING); }	// まだ撮影窓前なので待機(点灯)へ戻す
						}
					}
				}
				interruptibleSleep(500);
				continue;
			}
			// kPreConvergeSec 秒前に到達 → 初期収束ウォームアップ。nowCtx=startSec でブロックを1回通す(シャッター無し)。
			// 開始時刻が過去(遅れ起動)なら実時刻を文脈に使い、計画を過去から再生しない(1枚目から現在の窓/露出で収束)。
			now = (now > startSec) ? now : startSec;
		}
		else
		{
			// ①④ 通常コマ: [A]起点 → シャッター(前コマで適用済みの pending を撮る) → 周期-リードまで待つ。
			//   準備(測光→計算→設定)はリード手前から連続で行う(ヘッダ kPrepLeadMs の説明を参照)。
			//   露光直後に設定すると 503 "During shooting or recording" で必ず弾かれるため、ここでは何もしない。
			// このコマのシャッターが「前コマ+周期」からどれだけ遅れたか(0=ぴったり)。準備が
			// リードに収まらなかったコマだけ >0 になる。計測の主指標なのでシャッター直前に採る。
			if (lastAnchor != nullptr)
			{
				const long ach = static_cast<long>(tool::getElapse(lastAnchor));	// 実際に空いた間隔[ms]
				lateMs = ach - static_cast<long>(interval * 1000.0);
				if (lateMs < 0) { lateMs = 0; }
			}
			anchorA = tool::startElapse();
			now = static_cast<long long>(std::time(nullptr));
			if (now >= endSec) { break; }
			{
				const hgc::ccmWindow* wS = activeWindow(now);
				if (wS == nullptr || !wS->ccm) { interruptibleSleep(500); continue; }	// 隙間は撮らない
			}
			// 直前の露出適用が失敗していると、カメラは古い露出のままで、このまま撮ると
			// そのコマだけ露出が飛ぶ(2026-07-20 の IMG_1092/IMG_1100)。落とす前に1回だけ試し直す。
			//  ここは露光も記録も終わっている区間なので 503 で弾かれにくい。リトライループは使わず
			//  1パスだけ(シャッターを遅らせないため)。通れば pending を本来の露出へ戻す。
			if (applyFailed_ && validExposure(wantExp_))
			{
				if (this->applyExposureChanged(wantExp_) == ERR_HGC_OK) { pending = wantExp_; }
				applyFailed_ = false;
			}
			shotExp = pending;
			shutterMs = tool::epochMs();	// シャッター投下直前の壁時計(ms精度)
			err = this->fireShutter(shotExp, interval, shootFailStreak, cameraOffline);
			++frame;
			if (onProgress_) { onProgress_(progressInfo{ frame, total, static_cast<int>(endSec - now), static_cast<int>(now - startSec) }); }
			// 準備を始める時刻まで待つ。
			//  周期 <= リード や 周期-リード < SS のような厳しい設定でも、遅れて実行されるだけで破綻はしない
			//  (許可する仕様。カメラ単体のインターバル撮影と同様に周期が伸びる)。
			{
				// 準備(測光→計算→設定)の開始時刻は「測光をいつ呼んでほしいか」のカメラ実装の申告
				// (meterTimingHint)に従う(2026-07-27。方式・メーカーごとに可変):
				//  ・撮影画像フィードバック系: 露光終了+余裕 で最速開始(ソースは直前の撮影画像)
				//  ・ライブビュー系: シャッターの leadMs 手前(一定時間前の輝度を見る設計)
				const apiBase::meterTiming mt = cameraController::meterTimingHint(*dev_);
				const long im = static_cast<long>(interval * 1000.0);
				long prepAt;
				if (mt.afterShutterClose)
				{
					const double ssSec = expo::parseValue(shotExp.ss, expo::expoKind::ss);
					prepAt = static_cast<long>(((ssSec > 0.0) ? ssSec : 0.0) * 1000.0) + kAfterShutterMarginMs;
				}
				else
				{
					prepAt = im - ((mt.leadMs > 0) ? mt.leadMs : kPrepLeadMs);
				}
				if (prepAt < 0)  { prepAt = 0; }
				if (prepAt > im) { prepAt = im; }	// 露光が周期一杯 → 遅れ許容(従来どおり)
				leadUsed = static_cast<int>(im - prepAt);	// 実際に準備へ与えたリード(方式で変わる)
				sleepUntilElapse(anchorA, prepAt);
			}
			if (!running_) { break; }
			if (static_cast<long long>(std::time(nullptr)) >= endSec) { break; }
			now = static_cast<long long>(std::time(nullptr));	// 準備開始時点の文脈時刻
		}

		const hgc::ccmWindow* w = activeWindow(now);
		if (w == nullptr || !w->ccm)
		{	// 隙間: warmup中は待つだけ。通常コマは既に撮ったので周期まで待って次へ。
			if (warmedUp && anchorA != nullptr)
			{
				const long im = static_cast<long>(interval * 1000.0);
				if (static_cast<long>(tool::getElapse(anchorA)) < im) { sleepUntilElapse(anchorA, im); }
			}
			else { interruptibleSleep(500); }
			continue;
		}

		// ここからが準備(測光→露出計算→露出設定)。カメラはこの時点で暇=設定が通る。
		//  測光は「撮ったコマ(=shotExp)の露出」の下で行う(まだ次の露出を適用していないため)。
		//  prep = 準備の合計所要。リード(kPrepLeadMs)に収まったかの判定に使う。
		int     applyMs  = -1;
		int     applyTry = 0;
		errCode applyErr = ERR_HGC_OK;
		void*   prep = warmedUp ? tool::startElapse() : nullptr;
		//  meterExp = 実際に測光を行った露出(カメラ実装が申告する)。ev0 の逆算にはこれを使う
		//  (ここが従来の誤りの本体: 1秒相当しか写っていない値を 8秒露光として換算していた)。
		//  サムネイル測光では「直前に撮ったコマの露出」そのものになる。
		hgc::exposure meterExp = shotExp;	// 実際の測光露出は meterFrame(各分岐)が更新する
		if (warmedUp) { lastShotExp = shotExp; }	// 窓切替の継続用は「実際に撮影した露出」

		const hgc::ccmWindow* prevWin = curWin;
		const bool windowChanged = (w != curWin);
		curWin = w;
		const hgc::ccmBase* ccm = w->ccm.get();
		// 直前の窓も自動露出だったか(項目8: 自動露出→自動露出のみ目標evを緩やかに移行する)。
		const bool prevAuto = (prevWin && prevWin->ccm && isAuto(prevWin->ccm->type));

		hgc::exposure target{};
		double meteredLinear = -1.0;	// 測光したリニア輝度(自動補正時のみ。<0=測光なし)
		meterMs_  = -1;	// このコマの測光実測msをリセット(測光しないコマは -1 のまま)
		meterOk_  = true;	// 測光成否をリセット(測光しないコマは「成功扱い」でログ非表示)
		meterTry_ = 0;	// 測光試行回数をリセット(測光しないコマは 0)
		// 測光の内訳もリセットする。前コマの値が残ると、測光しないコマ(夜間の固定露出)で
		// 前の busy/取得時間をそのまま報告してしまう。
		meterWaitMs_ = -1; meterFetchMs_ = -1; meterDecodeMs_ = -1; meterFetchTries_ = 0;
		shotMissing_ = false;	// 測光しないコマは「撮れていない」の判定ができない=据え置き

		// ② このコマの ev0 中心bmを太陽高度から決める(薄明ほど暗く保つ)。測光を使う制御方法(preNight/postNight/auto)で効く。
		//    now=文脈時刻(実時刻)、plan_.place=撮影地。夜間(固定露出)では ev0 を使わないので影響しない。
		ev0cfg_ = expo::ev0Cfg();
		{
			const hgc::dateTime lt = hgc::fromUnixUtc(now, off_);
			const double sunAltDeg = astro::sunHoriz(lt, off_, plan_.place).altitude;
			ev0cfg_.bm = expo::ev0BmFromAltitude(sunAltDeg, ev0cfg_);
		}

		if (ccm->type == hgc::ccmType::night)
		{
			// 固定露出(仕様 3.2)
			target = ccm->limitBright;
		}
		else if (ccm->type == hgc::ccmType::preNight)
		{
			// 夜間前移行(仕様 3.7 改定): 1本の測光自動露出。
			//  - 露出の上限(暗所限界=最も露出の多い側)=夜間の固定露出にクランプ。基準(home)も夜間露出。
			//  - 下限(明所限界)・優先度は直前の自動露出制御方法(日中/夕日)に合わせる。
			//  - 目標 ev=夜間撮影の preNightEv。明るいうちは測光で露出を決め、暗くなるほど夜間露出へ近づく。
			//  - 窓の終端(夜間開始)で夜間露出にきっかり着地させるため、残りフレーム数が
			//    現在→夜間の所要フレーム数(1/3段/枚)以下になったら測光を止め 1/3 段ずつ収束する。
			const hgc::ccmBase*  prevC    = prevAutoCcmBefore(now);
			const hgc::ccmNight* nC       = nightCcmAfter(now);
			hgc::exposure        nightExp = nightGoalAfter(now);	// 夜間窓が無くてもプリセット夜間露出
			const double         preEv    = nC ? nC->preNightEv : plan_.nightPreNightEv;
			if (windowChanged)
			{
				// 上限=夜間露出(暗所限界)。下限(明所限界)・優先度は直前ccm(日中/夕日)。
				preCtl.init(tables_, nightExp, prevC ? prevC->limitDark : hgc::exposure{},
				            prevC ? prevC->priority : ccm->priority);
				preCtl.capLongestSs(maxSsCap);	// ss は夜間ss/周期-2秒を超えない(指示3)
				avgBuf.clear(); this->resetStepLock();
				preNightConverge = false;
				if (validExposure(lastShotExp)) { preCtl.setCurrent(lastShotExp); }	// 直前(日中/夕日)の撮影露出から継続
				else
				{
					// 撮影開始が夜間前移行の途中: 基準から測光しながら目標evへ収束して開始する(§4.4)。
					hgc::exposure seed = (prevC && validExposure(prevC->initial)) ? prevC->initial
					                   : (validExposure(nightExp) ? nightExp : ccm->initial);
					initialConverge(preCtl, seed, preEv);
					this->releaseLiveView();	// 初期収束が済んだらライブビューは離す
				}
			}

			// 終端で夜間露出へきっかり着地させるための残フレーム判定。
			const long long winEnd  = hgc::toUnixUtc(w->end, off_);
			const int remainFrames  = (interval > 0.0) ? static_cast<int>((winEnd - now) / interval) : 0;
			const double curB       = expo::brightnessStops(preCtl.current(), tables_);
			const double nightB     = validExposure(nightExp) ? expo::brightnessStops(nightExp, tables_) : curB;
			const int needFrames    = static_cast<int>(std::ceil(std::fabs(nightB - curB) / (1.0 / 3.0)));
			if (validExposure(nightExp) && remainFrames <= needFrames) { preNightConverge = true; }

			if (preNightConverge && validExposure(nightExp))
			{
				// 収束フェーズ: 測光を止め夜間露出へ 1/3 段ずつ寄せる(終端できっかり一致)。
				const double third = 1.0 / 3.0;
				if (curB - nightB > third / 2.0)      { preCtl.darken(); }
				else if (nightB - curB > third / 2.0) { preCtl.brighten(); }
				target = preCtl.current();
			}
			else
			{
				// 測光自動露出フェーズ(§4.5)。目標 ev=preEv。home=夜間露出、上限=夜間露出にクランプ済み。
				const bool   haveHome = validExposure(nightExp);
				const double homeB    = haveHome ? nightB : 0.0;
				apiBase::meterResult mr;
				this->meterFrame(shotExp, mr, warmedUp);	// 測光(実装はカメラ依存層。ウォームアップ中は切替なし=従来動作)
				meterExp = mr.meterExp;
				if (warmedUp) { lastExp = meterExp; }	// ev0 は「測光時の露出」
				const double linear = mr.ok ? mr.linear : -1.0;
				meteredLinear = linear;
				// mr.usable=false は「測れたが帯の外=根拠にできない」。値はログへ残し露出は据え置く
				// (2026-08-07。黒つぶれの測光値で撮影露出を絞り切った事故の再発防止)。
				if (linear > 0.0 && mr.usable)
				{
					// 測光値は測光露出で写る明るさ → 露出成分を割り戻してから平均する(土俵合わせ)。
					if (stepLock_ > 0) { --stepLock_; }	// 反転抑制の残りコマ(測光できたコマだけ数える)
					// 張り付きを検出したコマでは、それまでのバッファは「露出変更に反応しないLV」で測った値。
					// そのまま傾きを外挿すると偽のトレンド(絞っているのに明るくなり続ける)を増幅してしまう。
					// 立ち上がりで捨てて積み直す(張り付きが続く間は毎コマ捨てない=平均の平滑化は残す)。
					if (mr.pinned && !pinPrev) { avgBuf.clear(); this->resetStepLock(); }
					pinPrev = mr.pinned;
					avgBuf.push_back(mr.sceneRef);
					int n = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
					while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
					const double avg = this->sceneNowFromBuf(avgBuf);	// 遅れを補った現在値の推定
					double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : preCtl.current(), ev0cfg_);
					double linU = expo::linearFromEvBase(preEv + this->effHysteresis(smooth_.hysteresis) / 2.0, lin0);
					double linD = expo::linearFromEvBase(preEv - this->effHysteresis(smooth_.hysteresis) / 2.0, lin0);
					// 撮影露出で撮った場合の明るさへ投影してから比べる(ループを閉じる)。
					const double predicted = this->linearAtExposure(avg, preCtl.current());
					if (predicted > linU || predicted < linD)
					{
						const double center = expo::linearFromEvBase(preEv, lin0);
						const double need   = (predicted > 0.0) ? std::log2(center / predicted) : 0.0;
						// 帯の反対側へ飛び出すだけなら動かない(振動防止)。反転は抑制期間中は強い証拠が要る。
						const double band = this->effHysteresis(smooth_.hysteresis);
						const int    dir  = (need < 0.0) ? -1 : 1;
						if (this->wouldOvershoot(need, band) || !this->allowStep(dir, need, band)) { meterFailStreak = 0; }
						else
						{
						const int    steps  = this->stepsToClose(need);
						int          moves  = 0;
						for (int s = 0; s < steps; ++s)
						{
							const double cB = expo::brightnessStops(preCtl.current(), tables_);
							bool moved;
							if (need < 0.0) { moved = (haveHome && cB > homeB) ? preCtl.stepHome(false, nightExp) : preCtl.darken(); }
							else            { moved = (haveHome && cB < homeB) ? preCtl.stepHome(true,  nightExp) : preCtl.brighten(); }
							if (!moved) { break; }
							++moves;
						}
						if (moves > 0) { this->noteStep(dir); }
						}
					}
					meterFailStreak = 0;
				}
				else
				{
					if (meterFailStreak == 0) { avgBuf.clear(); this->resetStepLock(); if (onError_) { { char eb[224]; this->meterLostMsg(mr, eb, sizeof(eb)); onError_(ERR_HGC_RDY_METARING, eb); } } }
					++meterFailStreak;
				}
				target = preCtl.current();
				if (!validExposure(target)) { target = validExposure(lastShotExp) ? lastShotExp : nightExp; }
			}
		}
		else if (ccm->type == hgc::ccmType::postNight)
		{
			// 夜間後移行(仕様 3.9 改定): 1本の測光自動露出。露出の上限(暗所限界=最も露出の多い側)を
			// 夜間の固定露出にクランプし、下限(明所限界)・基準(home)・優先度は次の自動露出制御方法に
			// 合わせる。目標 ev=夜間撮影の postNightEv。夜間露出から始まり、明るくなった分だけ測光で下げる
			// (暗いうちは上限=夜間露出のまま)。雲などの明暗にも追従する。
			const hgc::ccmBase* nextC = nextAutoCcmAfter(now);
			hgc::exposure goal = nextC ? (validExposure(nextC->initial) ? nextC->initial : nightGoalAfter(now))	/* 次の制御方法の基準へ向かう(仕様3.9) */
			                           : hgc::exposure{};
			// 直前の夜間撮影の固定露出(=露出の上限)と夜間後露出補正(=目標ev)を取得する。
			// 夜間ウィンドウが無くてもプリセット値(plan_.nightFixedExposure/nightPostNightEv)へフォールバック。
			hgc::exposure nightExp{};
			double postEv = plan_.nightPostNightEv;
			for (const auto& ww : plan_.ccmList)
			{
				if (!ww.ccm || ww.ccm->type != hgc::ccmType::night) { continue; }
				if (hgc::toUnixUtc(ww.start, off_) <= now)
				{
					nightExp = ww.ccm->limitBright;	// 夜間の固定露出
					postEv   = static_cast<const hgc::ccmNight*>(ww.ccm.get())->postNightEv;
				}
			}
			if (!validExposure(nightExp)) { nightExp = nightGoalAfter(now); }	// 窓が無くてもプリセット夜間露出
			if (windowChanged)
			{
				// 上限(暗所限界=最も露出の多い側)=夜間露出にクランプ。下限(明所限界)・優先度は次ccm。
				postCtl.init(tables_, nightExp, nextC ? nextC->limitDark : hgc::exposure{},
				             nextC ? nextC->priority : ccm->priority);
				postCtl.capLongestSs(maxSsCap);	// ss は夜間ss/周期-2秒を超えない(指示3)
				avgBuf.clear(); this->resetStepLock();
				if (validExposure(lastShotExp)) { postCtl.setCurrent(lastShotExp); }	// 夜間の撮影露出から継続
				else
				{
					// 撮影開始が夜間後移行の途中: 他の自動露出と同様、開始前に露出補正(§4.4)してから入る。
					hgc::exposure seed = (nextC && validExposure(nextC->initial)) ? nextC->initial : nightExp;
					initialConverge(postCtl, seed, postEv);	// 測光しながら目標ev=postEv へ収束
					this->releaseLiveView();	// 初期収束が済んだらライブビューは離す
				}
			}
			// home(往復対称の基準)=次ccmの基準(=goal)。
			const bool   haveHome = validExposure(goal);
			const double homeB    = haveHome ? expo::brightnessStops(goal, tables_) : 0.0;
			apiBase::meterResult mr;
			this->meterFrame(shotExp, mr, warmedUp);	// 測光(実装はカメラ依存層。ウォームアップ中は切替なし=従来動作)
			meterExp = mr.meterExp;
			if (warmedUp) { lastExp = meterExp; }	// ev0 は「測光時の露出」
			const double linear = mr.ok ? mr.linear : -1.0;
			meteredLinear = linear;
			// mr.usable=false は「測れたが帯の外=根拠にできない」。値はログへ残し露出は据え置く。
			if (linear > 0.0 && mr.usable)
			{
				// 露出補正(仕様 4.5): 移動平均・ヒステリシス。目標 ev=postEv。往復対称(home=次の基準)。
				// 測光値は測光露出で写る明るさ → 露出成分を割り戻してから平均する(土俵合わせ)。
				if (stepLock_ > 0) { --stepLock_; }	// 反転抑制の残りコマ(測光できたコマだけ数える)
				// 張り付きを検出したコマでは、それまでのバッファは「露出変更に反応しないLV」で測った値。
				// そのまま傾きを外挿すると偽のトレンド(絞っているのに明るくなり続ける)を増幅してしまう。
				// 立ち上がりで捨てて積み直す(張り付きが続く間は毎コマ捨てない=平均の平滑化は残す)。
				if (mr.pinned && !pinPrev) { avgBuf.clear(); this->resetStepLock(); }
				pinPrev = mr.pinned;
				avgBuf.push_back(mr.sceneRef);
				int n = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
				while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
				const double avg = this->sceneNowFromBuf(avgBuf);	// 遅れを補った現在値の推定
				double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : postCtl.current(), ev0cfg_);
				double linU = expo::linearFromEvBase(postEv + this->effHysteresis(smooth_.hysteresis) / 2.0, lin0);
				double linD = expo::linearFromEvBase(postEv - this->effHysteresis(smooth_.hysteresis) / 2.0, lin0);
				// 撮影露出で撮った場合の明るさへ投影してから比べる(ループを閉じる)。
				const double predicted = this->linearAtExposure(avg, postCtl.current());
				if (predicted > linU || predicted < linD)
				{
					const double center = expo::linearFromEvBase(postEv, lin0);
					const double need   = (predicted > 0.0) ? std::log2(center / predicted) : 0.0;
					// 帯の反対側へ飛び出すだけなら動かない(振動防止)。反転は抑制期間中は強い証拠が要る。
					const double band = this->effHysteresis(smooth_.hysteresis);
					const int    dir  = (need < 0.0) ? -1 : 1;
					if (this->wouldOvershoot(need, band) || !this->allowStep(dir, need, band)) { meterFailStreak = 0; }
					else
					{
					const int    steps  = this->stepsToClose(need);
					int          moves  = 0;
					for (int s = 0; s < steps; ++s)
					{
						const double curB = expo::brightnessStops(postCtl.current(), tables_);
						bool moved;
						if (need < 0.0) { moved = (haveHome && curB > homeB) ? postCtl.stepHome(false, goal) : postCtl.darken(); }
						else            { moved = (haveHome && curB < homeB) ? postCtl.stepHome(true,  goal) : postCtl.brighten(); }
						if (!moved) { break; }
						++moves;
					}
					if (moves > 0) { this->noteStep(dir); }
					}
				}
				meterFailStreak = 0;
			}
			else
			{
				if (meterFailStreak == 0) { avgBuf.clear(); this->resetStepLock(); if (onError_) { { char eb[224]; this->meterLostMsg(mr, eb, sizeof(eb)); onError_(ERR_HGC_RDY_METARING, eb); } } }
				++meterFailStreak;
			}
			target = postCtl.current();
			if (!validExposure(target)) { target = validExposure(lastShotExp) ? lastShotExp : nightExp; }
		}
		else if (isAuto(ccm->type))
		{
			const double evTraw = targetEv(ccm);
			// §4.5 往復対称の基準(home)=基準の明るさ。home から離れる→優先度順 / 近づく→逆優先。
			const bool   haveHome = validExposure(ccm->initial);
			const double homeB    = haveHome ? expo::brightnessStops(ccm->initial, tables_) : 0.0;
			// 項目7: 平滑化(ヒステリシス/移動平均)は ccm 個別値があれば優先、無ければ全体設定。
			const double effHyst = (ccm->hysteresis > 0.0)  ? ccm->hysteresis  : smooth_.hysteresis;
			const int    effMA   = (ccm->movingAverage > 0) ? static_cast<int>(ccm->movingAverage)
			                     : ((smooth_.movingAverage > 0) ? smooth_.movingAverage : 5);
			bool didInitConverge = false;
			if (windowChanged)
			{
				autoCtl.init(tables_, ccm->limitBright, ccm->limitDark, ccm->priority);
				autoCtl.capLongestSs(maxSsCap);	// ss は夜間ss/周期-2秒を超えない(指示3)
				avgBuf.clear(); this->resetStepLock();
				// 項目8: 自動露出→自動露出の切替で目標evが急変するとオーバーシュートするため、
				// 実効目標evは前窓の値を保持して以降 1/3 段/枚で寄せる。不連続(開始/非自動から)は即適用。
				if (!(validExposure(lastShotExp) && prevAuto)) { curEvT = evTraw; }
				if (validExposure(lastShotExp))
				{
					// 自動露出の開始(仕様 4.8): 撮影継続中の撮影制御方法切替では不連続を避け、
					// 基準(iso/ss/fn)の構成から始めて直前の「撮影」露出の APEX(明るさ)へ合わせて開始する。
					if (validExposure(ccm->initial)) { autoCtl.setCurrent(ccm->initial); }
					else                    { autoCtl.setCurrent(lastShotExp); }
					double prevB = expo::brightnessStops(lastShotExp, tables_);
					double curB  = expo::brightnessStops(autoCtl.current(), tables_);
					autoCtl.applyStops(prevB - curB);
				}
				else
				{
					// 最初の補正(仕様 4.4): 撮影開始直後は初期露出が不定。基準から測光しながら
					// 目標 ev へ反復収束させて 1 枚目の露出を決める(張り付き時は二分探索)。
					target = initialConverge(autoCtl, ccm->initial, evTraw);
					this->releaseLiveView();	// 初期収束が済んだらライブビューは離す
					didInitConverge = true;
				}
			}

			// 項目8: 実効目標evを新目標へ 1/3 段ずつ寄せる(自動露出→自動露出の緩やか移行)。
			{
				const double third = 1.0 / 3.0;
				if      (curEvT < evTraw - 1e-9) { curEvT = std::min(evTraw, curEvT + third); }
				else if (curEvT > evTraw + 1e-9) { curEvT = std::max(evTraw, curEvT - third); }
			}
			const double evT = curEvT;

			if (!didInitConverge)
			{
				// 測光(実装はカメラ依存層)→ 場面の明るさ(仕様 4.3)
				apiBase::meterResult mr;
				this->meterFrame(shotExp, mr, warmedUp);	// 測光(実装はカメラ依存層。ウォームアップ中は切替なし=従来動作)
				meterExp = mr.meterExp;
				if (warmedUp) { lastExp = meterExp; }	// ev0 は「測光時の露出」
				const double linear = mr.ok ? mr.linear : -1.0;
				meteredLinear = linear;	// 測光値をログ用に保持(失敗時は-1)

				// mr.usable=false は「測れたが帯の外=根拠にできない」。値はログへ残し露出は据え置く。
				if (linear > 0.0 && mr.usable)
				{
					// 露出補正(仕様 4.5): 移動平均とヒステリシス帯(項目7: ccm 個別値を優先)
					// 測光値は「測光露出で写る明るさ」なので、露出成分を割り戻した場面の明るさを
					// 平均する(測光の条件はコマ毎に変わり得るので、生の測光値を平均すると別条件が混ざる)。
					if (stepLock_ > 0) { --stepLock_; }	// 反転抑制の残りコマ(測光できたコマだけ数える)
					// 張り付きを検出したコマでは、それまでのバッファは「露出変更に反応しないLV」で測った値。
					// そのまま傾きを外挿すると偽のトレンド(絞っているのに明るくなり続ける)を増幅してしまう。
					// 立ち上がりで捨てて積み直す(張り付きが続く間は毎コマ捨てない=平均の平滑化は残す)。
					if (mr.pinned && !pinPrev) { avgBuf.clear(); this->resetStepLock(); }
					pinPrev = mr.pinned;
					avgBuf.push_back(mr.sceneRef);
					int n = effMA;
					while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
					const double avg = this->sceneNowFromBuf(avgBuf);	// 遅れを補った現在値の推定

					double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : autoCtl.current(), ev0cfg_);
					double linU = expo::linearFromEvBase(evT + this->effHysteresis(effHyst) / 2.0, lin0);
					double linD = expo::linearFromEvBase(evT - this->effHysteresis(effHyst) / 2.0, lin0);
					// 撮影露出で撮った場合の明るさへ投影してから比べる(土俵合わせ)。これでループが
					// 閉じ、露出を動かすと比較結果も動く(従来は測光値が撮影露出に依存せず暴走した)。
					const double predicted = this->linearAtExposure(avg, autoCtl.current());
					if (predicted > linU || predicted < linD)
					{
						const double center = expo::linearFromEvBase(evT, lin0);
						const double need   = (predicted > 0.0) ? std::log2(center / predicted) : 0.0;	// +:明るく -:暗く
						// 帯の反対側へ飛び出すだけなら動かない(振動防止)。反転は抑制期間中は強い証拠が要る。
						const double band = this->effHysteresis(effHyst);
						const int    dir  = (need < 0.0) ? -1 : 1;
						if (this->wouldOvershoot(need, band) || !this->allowStep(dir, need, band)) { meterFailStreak = 0; }
						else
						{
						const int    steps  = this->stepsToClose(need);
						int          moves  = 0;
						for (int s = 0; s < steps; ++s)
						{
							const double curB = expo::brightnessStops(autoCtl.current(), tables_);
							bool moved;
							if (need < 0.0) { moved = (haveHome && curB > homeB) ? autoCtl.stepHome(false, ccm->initial) : autoCtl.darken(); }
							else            { moved = (haveHome && curB < homeB) ? autoCtl.stepHome(true,  ccm->initial) : autoCtl.brighten(); }
							if (!moved) { break; }	// 限界に到達
							++moves;
						}
						if (moves > 0) { this->noteStep(dir); }
						}
					}
					meterFailStreak = 0;	// 測光成功
				}
				else
				{
					// 測光失敗。カメラの一時不応答などが続くと露出が凍結し日中に白飛びする
					// (実機 06/15 で発生)。移動平均は陳腐化するので破棄し、次コマで測り直す。
					if (meterFailStreak == 0)
					{
						avgBuf.clear(); this->resetStepLock();
						if (onError_) { { char eb[224]; this->meterLostMsg(mr, eb, sizeof(eb)); onError_(ERR_HGC_RDY_METARING, eb); } }
					}
					++meterFailStreak;
				}
				target = autoCtl.current();
			}
		}
		else
		{
			target = ccm->limitBright;	// その他はフォールバック
		}

		// --- ウォームアップ(初期収束)完了処理: シャッターは撃たず 1枚目の露出 pending を確定 ---
		if (!warmedUp)
		{
			if (validExposure(target))       { pending = target; }
			else if (validExposure(lastExp)) { pending = lastExp; }
			else                             { pending = ccm->limitBright; }
			warmedUp = true;
			// 1枚目だけは「前コマの準備」が存在しないので、ここでカメラへ適用しておく。
			// これを省くと1枚目が pending と違う露出(初期収束の最後の試行値=測光露出)で撮れてしまう。
			//
			// 【2026-08-05 実測で判明】ここが失敗したまま撮り始めると、被害は1枚目に留まらない:
			//  カメラは初期収束で最後に使った測光露出のまま1枚目を撮ってしまい、以後のコマまで
			//  巻き添えにする(その1枚を測光した値で次の露出を決めるため)。露出調整が済んで
			//  いないのに撮り始めたことがすべての起点なので、乗るまで待つ。
			// 撮影中の適用と違い、ここは次コマの締め切りが無いので長く待てる(kFirstApplyMaxMs)。
			{
				int t1 = 0;
				const errCode ae = applyWithRetry(pending, t1, kFirstApplyMaxMs);
				// 何回目で乗ったかを残す(SHOTログの fa=)。この事象は放置後の初回にしか出ないので、
				// 「そもそも失敗しなかった」のか「失敗したが待って乗った」のかを後から区別する。
				// onError_ は UI へトーストも出すので、情報の記録には使わない。
				firstApplyTries_ = t1;
				if (ae != ERR_HGC_OK)
				{
					if (onError_)
					{
						char eb[240];
						std::snprintf(eb, sizeof(eb), "%s (try=%d %dms)",
						              this->withHttpDetail("1枚目の露出をカメラへ適用できない").c_str(),
						              t1, kFirstApplyMaxMs);
						onError_(ae, eb);
					}
					// 乗らなかった。狙いは変えず、シャッター直前の再適用へ託す
					// (ループ先頭の applyFailed_ 経路が1回だけ試し直す)。
					applyFailed_ = true;
					wantExp_     = pending;
					// 【2026-08-06】撮る露出は「実機に乗っている値」にする。従来は狙いの値のまま
					//  撮ったことにしていたため、そのコマの測光を誤った露出で割り戻し、次コマの
					//  露出まで巻き添えにしていた(被害が1コマで終わらない)。ログと実写も食い違った。
					//  通信が失敗している最中なので、カメラへ問い合わせず内部の記録から復元する。
					const hgc::exposure act = this->appliedOrConverge();
					if (validExposure(act)) { pending = act; }
				}
			}
			// 収束が撮影窓より早く終わった余り時間は keepAlive で待つ。窓開始で CAPTURING。
			while (running_ && static_cast<long long>(std::time(nullptr)) < startSec)
			{
				cameraController::keepAlive(*dev_);
				interruptibleSleep(1000);
			}
			if (!running_) { break; }
			// 1枚目のシャッターを切る直前に、測光の実装へ「構え直せ」と伝える。
			//  ここまでの準備(撮影モード変更・設定取得・初期収束の露出適用)でカメラ側に
			//  溜まった状態を捨てさせ、次の1コマの結果だけを見られるようにする。
			//  これをしないと CCAPI では1コマ目の測光だけが必ず失敗した(2026-08-13 実測)。
			cameraController::meterArm(*dev_);
			if (onState_) { onState_(ST_CAPTURING); }
			continue;	// 次反復が最初の実コマ([A]起点)
		}

		// --- 通常コマ(①): 測光→算出した target を、次シャッターの手前(=カメラが暇なリード区間)で適用する。
		//   ①の主旨「測光遅れ1コマ削減」は『今回の測光で決めた露出を次のシャッターで撮る』ことで既に果たしており、
		//   適用を露光直後に置く必要はない。むしろ露光直後は 503 で必ず弾かれる(2026-07-16 の事故)。
		//   撮ったコマ(shotExp)を、その露出下で行った今回の測光(meteredLinear)と対応付けてログ。 ---
		if (!validExposure(target)) { target = validExposure(shotExp) ? shotExp : ccm->limitBright; }	// 無効はフォールバック
		if (warmedUp)
		{
			void* ta = tool::startElapse();
			applyErr = applyWithRetry(target, applyTry);	// 通るまでリトライ(最大 kApplyMaxMs)
			applyMs  = static_cast<int>(tool::getElapse(ta));
			if (applyErr != ERR_HGC_OK && onError_)
			{	// リトライしても設定できなかった。放置するとカメラは古い露出のまま撮り続け、
				// アプリの露出モデルと実機がズレる(白飛び/黒潰れの原因)。必ずログへ出して気付けるようにする。
				onError_(applyErr, this->withHttpDetail("setExposure failed after retry"));
			}
		}
		const int prepMs = (prep != nullptr) ? static_cast<int>(tool::getElapse(prep)) : -1;
		// busy(露光終了 → カメラが測光に応じられるまで)。サムネイル測光では「新しい画像の
		// 登録通知を待った時間」がそれに当たる。準備は露光終了 + kAfterShutterMarginMs から
		// 始めているので、その余裕を足したものが露光終了からの実測になる。
		// 撮影周期をどこまで SS へ詰められるかを決めるのはこの時間なので、レポートへ残す。
		if (warmedUp && meterWaitMs_ >= 0)
		{
			busyMs = static_cast<int>(kAfterShutterMarginMs) + meterWaitMs_;
		}
		// このコマは測光していない(夜間の固定露出)。カメラと一度も話さないまま何時間も続くので、
		// 無害なGETを1回だけ入れて接続を保つ。カメラ実装によっては、この1回で測光方式が
		// ためこんでいる状態(CCAPIなら撮影画像の登録通知)も一緒に流れる。
		// 露光も記録も終わっている区間なので、撮影の進行には影響しない。
		if (warmedUp && meterTry_ == 0) { cameraController::keepAlive(*dev_); }
		// 次シャッターで撮る露出。適用に成功していれば target。
		// 失敗しているならカメラは古い露出のまま(軸ごとに一部だけ適用されることもある)なので、
		// target で撮ったことにしてはいけない。実際に適用できている値を次コマの露出とする。
		// これでアプリの露出モデル・ログ・実写が一致する(2026-07-20 IMG_1092/IMG_1100 の食い違いを根治)。
		applyFailed_ = (warmedUp && applyErr != ERR_HGC_OK);
		wantExp_     = target;
		if (!applyFailed_) { pending = target; }
		else
		{
			const hgc::exposure act = this->appliedExposure();
			pending = validExposure(act) ? act : target;	// 読めないときは従来どおり
		}

		if (onCaptured_)
		{
			double lum = expo::brightnessStops(shotExp, tables_);	// 撮ったのは shotExp
			onCaptured_(capturedInfo{ frame, shotExp, lum, ccm->name, meteredLinear, meterMs_, applyMs, prepMs,
			                          static_cast<int>(lateMs), meterOk_, (applyErr == ERR_HGC_OK),
			                          meterTry_, applyTry, histSum_, lvTimeMs_, staleSkip_, shutterMs, lvP99_, lvPMax_,
			                          lvMeanLinLog_, lvP75Log_, lvP90Log_, lvSatLog_,
			                          meterSsUsed_, meterSettleMs_, lvPinnedLog_, meterUsableLog_,
			                          meterWaitMs_, meterFetchMs_, meterDecodeMs_, meterFetchTries_, busyMs, leadUsed,
			                          asIsLinear_, firstApplyTries_, converge_ });
		}

		// 測光の連続失敗は「接続断」ではない(2026-07-28 根治)。
		//  実機で、シャッターは1枚も失敗していないのに測光(サムネイル取得)だけが連続失敗し、
		//  接続断と誤判定 → 探索ループに入って撮影を完全に停止した(2時間20分の空白)。
		//  カメラが撮れている以上つながっているので、測光失敗では露出を据え置いて撮影を続ける。
		//  「撮れていない」の判定は下の noRecordStreak が別に行う(そちらは根拠が違う)。
		if (meterFailStreak >= kMaxMeterFail)
		{
			meterFailStreak = 0;	// 数え直すだけ。セッションは張り直さない(撮影を止めない)
			avgBuf.clear(); this->resetStepLock();
		}

		// --- このコマは「撮影結果が現れた」か(ヘッダ kMaxNoRecordFrames の説明を参照) ---
		//  シャッターが失敗した/カメラ実装が「撮影結果が現れない」と申告した のどちらかなら、
		//  そのコマは撮れていない。測光しないコマ(夜間の固定露出)は判定できないので据え置く。
		if (err != ERR_HGC_OK || shotMissing_)
		{
			++noRecordStreak;
		}
		else if (meterTry_ > 0)
		{	// 画像が現れた = カメラは撮れている。
			noRecordStreak = 0;
			noRecordRounds = 0;
			if (cameraOffline)
			{	// 復帰した(電源入れ直し等)。提示を撮影中へ戻す。
				cameraOffline = false;
				if (onState_) { onState_(ST_CAPTURING); }
				if (onError_) { onError_(ERR_HGC_OK, "カメラの撮影が復帰しました"); }
			}
		}

		// シャッターの連続失敗(=届いていない)、または撮った画像が現れない(=届くのに撮れていない)
		// → 手を打つ。この2つは症状も対処も違うので分けて扱う。
		const bool noRecord = (noRecordStreak >= kMaxNoRecordFrames);
		const bool lostLink = (shootFailStreak >= kMaxConsecutiveFail);
		if (lostLink || noRecord)
		{
			if (noRecord) { noRecordStreak = 0; ++noRecordRounds; }
			// セッションは毎回張り直しておく(電源を入れ直された後はこれが唯一の戻り道)。
			const bool established = (onReconnect_ && onReconnect_() && establishSession());
			// ただし「応答するのに撮れていない」状態では establishSession は**成功してしまう**
			// (情報系もライブビューも生きているため)。1回目だけは復帰したものとして様子を
			// 見るが、2回目以降は成功を復帰の証拠にしない。撮れた画像が現れることだけが証拠。
			const bool trustEstablish = !(noRecord && noRecordRounds > 1);
			bool recovered = established && trustEstablish;
			if (!recovered && !cameraOffline)
			{	// ユーザーへ「オンラインでない」と提示する(✖点灯)。
				cameraOffline = true;
				if (onState_) { onState_(ST_NOCAMERA); }
				if (onError_)
				{
					onError_(ERR_HGC_NOT_FOUND, noRecord
					         ? "カメラが撮影を完了しません(シャッターは通るのに画像が記録されない)。オフラインとして表示します"
					         : "撮影中にカメラ接続が切れました。再接続を試行します(中止するまで継続)");
				}
			}
			if (!recovered && lostLink)
			{	// 本当に届いていない → 中止まで再接続を続ける(従来どおり)。
				while (running_)
				{
					if (onReconnect_ && onReconnect_() && establishSession()) { recovered = true; break; }
					interruptibleSleep(kReconnectWaitMs);
				}
				if (!recovered) { break; }	// 中止された → 終了処理へ
				cameraOffline  = false;
				noRecordRounds = 0;
				if (onState_) { onState_(ST_CAPTURING); }
			}
			shootFailStreak = 0;
			meterFailStreak = 0;
			avgBuf.clear(); this->resetStepLock();
			{	// establishでキャッシュclear済 → 次シャッター前に露出を再適用しカメラ状態を合わせる。
				int t2 = 0;
				const errCode ae = applyWithRetry(pending, t2);
				if (ae != ERR_HGC_OK && onError_) { onError_(ae, this->withHttpDetail("setExposure failed after retry (reconnect)")); }
			}
			// 届いていない側は再接続で時間を食っているので、次コマは即[A]起点にする。
			// 撮れていない側は通信できているので周期を崩さない(復帰したらそのまま定刻へ戻る)。
			if (lostLink) { continue; }
		}

		// --- 次シャッター時刻(④c): [A]から周期後を狙う。超過なら即・未満なら残りを待つ(周期は縮めない/フレーム落とさない)。 ---
		lastAnchor = anchorA;	// 最後の1枚保護用に直近シャッター基準を退避
		{
			const long im = static_cast<long>(interval * 1000.0);
			if (static_cast<long>(tool::getElapse(anchorA)) < im) { sleepUntilElapse(anchorA, im); }
			// else: SS+ヒスト取得が周期を超過 → 即次コマへ(overrun許容)
		}
	}

	// 最後の1枚を守る: 直前に切ったコマはカメラ側でまだ露光・保存中のことがある。ここで即
	// restoreShootingMode(設定変更POST)を送ると busy なカメラで最後の1枚が壊れる/復元失敗する。
	// 次の撮影周期境界(=露光+保存が済む時刻)まで「中断しない待ち」を入れてから終了処理へ入る。
	if (lastAnchor != nullptr && frame > 0)
	{
		const long targetMs = static_cast<long>(interval * 1000.0);	// 直近シャッター([A])から周期分待つ
		for (long left = targetMs - static_cast<long>(tool::getElapse(lastAnchor)); left > 0;
		     left = targetMs - static_cast<long>(tool::getElapse(lastAnchor)))
		{
			uint32_t c = (left > 200) ? 200u : static_cast<uint32_t>(left);
			tool::sleep(c);
		}
	}

	// 撮影終了: カメラの撮影モードを元に戻す(ダイアル無視OFF含む)。仕様8/CCAPI。
	// apiBase 未取得(未検出のまま中止/窓終了)の場合は cameraController 側の null ガードで無害にスキップ。
	cameraController::restoreShootingMode(*dev_);

	running_ = false;
	// 3a: 自前で諦めることは無い(中止まで再試行)。ここに来る=窓終了または中止 → IDLE。
	if (onState_) { onState_(ST_IDLE); }
	return ERR_HGC_OK;
}
