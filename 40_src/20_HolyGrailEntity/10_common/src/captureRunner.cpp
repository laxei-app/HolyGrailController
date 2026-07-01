#include "common.h"
#include "captureRunner.h"
#include "osSystemCall.h"
#include "debugOut.h"
#include <algorithm>
#include <cmath>
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
	// 露出を変えてからライブビューが追従するまでの待ち[ms]。実機で調整可。
	constexpr long   kMeterSettleMs        = 700;
	// 反復回数(初回測定 + 最大3回程度の補正。仕様の手順5/6)。
	constexpr int    kInitConvergeTries    = 4;
	// 目標 ev への許容[段]。これ以内に入ったら収束終了して撮影に入る。
	constexpr double kInitConvergeTolStops = 0.5;
	// ヒストグラム中央値がこの範囲外なら明暗に張り付き(測光値を信用しない)とみなす。
	constexpr double kPegBright = 0.99;
	constexpr double kPegDark   = 0.01;
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
	return ERR_HGC_OK;
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
	return ERR_HGC_OK;
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

// rdyMetering(ライブビュー1枚の取得=HTTP GET)を実測付きで呼ぶ。所要msは meterMs_ に退避するだけで
// ここではログI/Oを行わない(将来の tm0 相当の時間クリティカル点でSD書き込みを走らせないため)。
// 実ログは撮影(シャッター)後の onCaptured 経由で出力する(tm1相当)。2秒窓の予算検討用の計測。
errCode captureRunner::rdyMeterTimed(void)
{
	void* mt = tool::startElapse();
	errCode e = cameraController::rdyMetering(*dev_);
	meterMs_ = static_cast<int>(tool::getElapse(mt));
	return e;
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

// 最初の補正(仕様 4.4)を反復収束で行う。撮影開始直後は初期露出が不定なので、
// 基準(iso/ss/fn)から始め、ライブビューを測光して目標 ev へ寄せる。
//  - 測光が信用できる(張り付いていない)ときは仕様 4.4 のニュートン段(目標との差ぶん移動)。
//  - 明側/暗側に張り付いているときは測光値を信用せず、明限界〜暗限界のブラケットを
//    方向で狭めて中央へ二分探索的に寄せる(張り付き解消後はニュートン段に戻る)。
// 最大 kInitConvergeTries 回。許容内に入れば終了、ダメでも最良点で撮影に入る(手順5/6)。
// ctl は呼び出し前に init 済みであること。戻り値=1枚目の露出(ctl もその値になる)。
hgc::exposure captureRunner::initialConverge(expo::exposureCtl& ctl, const hgc::exposure& initial, double evT)
{
	// 許容範囲を段で取り、ブラケットの初期値とする(明側=最も明るい/暗側=最も暗い)。
	ctl.setToBrightLimit();
	double hiB = expo::brightnessStops(ctl.current(), tables_);
	ctl.setToDarkLimit();
	double loB = expo::brightnessStops(ctl.current(), tables_);

	// 仕様 4.4 の基準(iso/ss/fn)から開始する。
	ctl.setCurrent(initial);

	hgc::exposure best = ctl.current();
	double bestAbsErr = 1e9;

	for (int i = 0; i < kInitConvergeTries && running_; ++i)
	{
		// 現在の露出をカメラへ反映し、ライブビューが追従するのを待ってから測光する。
		hgc::exposure cur = ctl.current();
		cmdt::shotSet shot(cur.ss, cur.fn, cur.iso);
		cameraController::rdyShutter(*dev_, shot);
		interruptibleSleep(kMeterSettleMs);

		double x = -1.0, linear = -1.0;
		cmdt::HISTOGRAM hist;
		if (this->rdyMeterTimed() == ERR_HGC_OK &&
		    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
		{
			x = expo::histMedian(hist.y, cmdt::hist_bin);
			linear = expo::srgbToLinear(x);
		}

		const double curB = expo::brightnessStops(cur, tables_);

		if (linear <= 0.0)
		{	// 測光失敗 → ブラケット中央へ寄せて再試行。
			ctl.applyStops((loB + hiB) * 0.5 - curB);
			continue;
		}

		// ev0 のリニア輝度は環境光依存(§4.3.3/4.3.4)。測光値と測光時の露出から都度求める。
		const double lin0 = expo::ev0LinearForMeasure(linear, cur, expo::ev0Cfg());
		const double linT = expo::linearFromEvBase(evT, lin0);	// 目標リニア輝度
		const double err  = expo::evFromLinear(linT, linear);	// log2(linear/linT): + 明るすぎ / - 暗すぎ
		if (std::fabs(err) < bestAbsErr) { bestAbsErr = std::fabs(err); best = cur; }
		if (std::fabs(err) <= kInitConvergeTolStops) { break; }	// 目標 ev に十分近い(手順5)

		// 方向でブラケットを更新(明るすぎ→上限を現在へ / 暗すぎ→下限を現在へ)。
		if (err > 0.0) { hiB = curB; } else { loB = curB; }

		const bool pegged = (x >= kPegBright) || (x <= kPegDark);
		double nextB;
		if (pegged)
		{	// 張り付き: 測光値が信用できない → 最初の設定と今の設定の間(二分)へ寄せる(手順2/3)。
			nextB = (loB + hiB) * 0.5;
		}
		else
		{	// 仕様 4.4 のニュートン段。ブラケットの枠外に出るなら中央へ。
			nextB = curB - err;
			if (nextB <= loB || nextB >= hiB) { nextB = (loB + hiB) * 0.5; }
		}
		ctl.applyStops(nextB - curB);
	}

	ctl.setCurrent(best);	// 最良点を撮影1枚目の露出にする(手順6)。
	return best;
}

// ライブビュー開始 + M設定(ダイアル無視/オートパワーオフ抑止) + 設定可能値テーブル構築。
// 撮影開始時と、撮影中の再接続後の両方で使う。return: ライブビュー開始に成功したか。
bool captureRunner::establishSession(void)
{
	// カメラ未取得(apiBase==nullptr)ならセッションは張れない(3a: 未検出許容)。
	if (dev_ == nullptr || dev_->apiBase == nullptr) { return false; }

	// 変更分のみ適用のキャッシュをクリア(再接続直後はカメラ状態が不定なので次回フル適用させる)。
	lastFnApplied_.clear(); lastSsApplied_.clear(); lastIsoApplied_.clear();

	// 撮影モードに入る(ライブビュー開始)
	errCode err = cameraController::startShooting(*dev_);
	if (err != ERR_HGC_OK)
	{
		if (onError_) { onError_(err, "startShooting"); }
		return false;
	}

	// カメラを当アプリ都合の設定にする(撮影モードダイアル無視ON+マニュアル露出)。仕様8/CCAPI。
	// getSettings(設定可能値テーブル作成)より前に行う: Av等ではtvのabilityが空になり ss テーブルが
	// 作れないため、Mにしてから設定可能値を取得する。終了時に restoreShootingMode で元へ戻す。
	{
		errCode me = cameraController::setupShootingModeManual(*dev_);
		if (me == ERR_HGC_OK)            { interruptibleSleep(800); }	// モード変更/ability更新の反映待ち(初回rdyShutterの取りこぼし防止)
		else if (me == ERR_HGC_NOT_SUPPORTED) { /* モード変更非対応機。そのまま続行 */ }
		else if (onError_)               { onError_(me, "setupShootingModeManual"); }
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
	const double nightSsSec = expo::parseValue(plan_.nightFixedExposure.ss, expo::expoKind::ss);
	const double ivCap      = (interval > kShutterOffsetSec) ? (interval - kShutterOffsetSec) : interval;	// 最大ss=周期-offset
	const double maxSsCap   = (nightSsSec > 0.0) ? std::min(nightSsSec, ivCap) : ivCap;

	const hgc::ccmWindow* curWin = nullptr;
	expo::exposureCtl autoCtl;		// 自動露出用
	expo::exposureCtl preCtl;		// 夜間前移行用(自動露出→夜間)
	expo::exposureCtl postCtl;		// 夜間後移行用(夜間→次の自動露出)
	std::vector<double> avgBuf;		// リニア輝度の移動平均バッファ
	hgc::exposure lastExp{};		// 直近の露出設定
	int meterFailStreak = 0;	// 連続測光失敗数(ライブビュー停止の検出/回復用)
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
	hgc::exposure pending{};			// 各境界(tm0)で適用し tm1 で撮る露出。前フェーズの初期収束で確定
	bool          warmedUp   = false;	// 初期収束が済み pending が確定したか
	void*         mono       = nullptr;	// 撮影窓開始を0とする monotonic アンカー
	long long     boundaryIdx = 0;		// 何コマ目か(境界時刻 = mono + boundaryIdx*interval)

	while (running_)
	{
		long long now = static_cast<long long>(std::time(nullptr));
		if (now >= endSec) { break; }				// 計画終了

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
						{	// 連続失敗で接続断と判定(検知ログは1回だけ)。
							waitDisconnected = true;
							if (onError_) { onError_(ERR_HGC_NOT_FOUND, "待機中に接続断を検知 → 先回り再接続"); }
							if (onState_) { onState_(ST_NOCAMERA); }	// 待機中のカメラ未検出(✖点灯)
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
			now = startSec;
		}
		else
		{
			// tm0: 次の境界(絶対アンカー)まで待つ。累積ドリフトが出ない。
			sleepUntilElapse(mono, static_cast<long>(boundaryIdx * interval * 1000.0));
			if (!running_) { break; }
			if (static_cast<long long>(std::time(nullptr)) >= endSec) { break; }
			now = startSec + static_cast<long long>(boundaryIdx * interval);	// ブロック用の窓コンテキスト
		}

		const hgc::ccmWindow* w = activeWindow(now);
		if (w == nullptr || !w->ccm) { if (warmedUp) { ++boundaryIdx; } interruptibleSleep(500); continue; }	// 隙間

		// tm0: 変更のあった露出項目だけ適用(通常コマのみ。ウォームアップは pending 未確定なので適用しない)。
		void* tm0    = warmedUp ? tool::startElapse() : nullptr;	// tm0処理時間(適用+測光)の計測開始
		int   applyMs = -1;
		if (warmedUp)
		{
			lastExp = pending;	// ブロックの ev0 は「測光時の露出」= これから測る pending を使う
			void* ta = tool::startElapse();
			applyExposureChanged(pending);
			applyMs = static_cast<int>(tool::getElapse(ta));
		}

		const hgc::ccmWindow* prevWin = curWin;
		const bool windowChanged = (w != curWin);
		curWin = w;
		const hgc::ccmBase* ccm = w->ccm.get();
		// 直前の窓も自動露出だったか(項目8: 自動露出→自動露出のみ目標evを緩やかに移行する)。
		const bool prevAuto = (prevWin && prevWin->ccm && isAuto(prevWin->ccm->type));

		hgc::exposure target{};
		double meteredLinear = -1.0;	// 測光したリニア輝度(自動補正時のみ。<0=測光なし)
		meterMs_ = -1;	// このコマの rdyMetering 実測msをリセット(測光しないコマは -1 のまま)

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
				avgBuf.clear();
				preNightConverge = false;
				if (validExposure(lastExp)) { preCtl.setCurrent(lastExp); }	// 直前(日中/夕日)から継続
				else
				{
					// 撮影開始が夜間前移行の途中: 基準から測光しながら目標evへ収束して開始する(§4.4)。
					hgc::exposure seed = (prevC && validExposure(prevC->initial)) ? prevC->initial
					                   : (validExposure(nightExp) ? nightExp : ccm->initial);
					initialConverge(preCtl, seed, preEv);
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
				double linear = -1.0;
				cmdt::HISTOGRAM hist;
				if (this->rdyMeterTimed() == ERR_HGC_OK &&
				    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
				{
					double x = expo::histMedian(hist.y, cmdt::hist_bin);
					linear = expo::srgbToLinear(x);
				}
				meteredLinear = linear;
				if (linear > 0.0)
				{
					avgBuf.push_back(linear);
					int n = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
					while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
					double avg = 0.0;
					for (double v : avgBuf) { avg += v; }
					avg /= static_cast<double>(avgBuf.size());
					double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : preCtl.current(), expo::ev0Cfg());
					double linU = expo::linearFromEvBase(preEv + smooth_.hysteresis / 2.0, lin0);
					double linD = expo::linearFromEvBase(preEv - smooth_.hysteresis / 2.0, lin0);
					double cB   = expo::brightnessStops(preCtl.current(), tables_);
					if (avg > linU)      { if (haveHome && cB > homeB) { preCtl.stepHome(false, nightExp); } else { preCtl.darken(); } }
					else if (avg < linD) { if (haveHome && cB < homeB) { preCtl.stepHome(true, nightExp); } else { preCtl.brighten(); } }
					meterFailStreak = 0;
				}
				else
				{
					if (meterFailStreak == 0) { avgBuf.clear(); if (onError_) { onError_(ERR_HGC_RDY_METARING, "metering lost -> liveview restart"); } }
					++meterFailStreak;
					cameraController::startShooting(*dev_);
					interruptibleSleep(kMeterSettleMs);
				}
				target = preCtl.current();
				if (!validExposure(target)) { target = validExposure(lastExp) ? lastExp : nightExp; }
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
				avgBuf.clear();
				if (validExposure(lastExp)) { postCtl.setCurrent(lastExp); }	// 夜間から継続
				else
				{
					// 撮影開始が夜間後移行の途中: 他の自動露出と同様、開始前に露出補正(§4.4)してから入る。
					hgc::exposure seed = (nextC && validExposure(nextC->initial)) ? nextC->initial : nightExp;
					initialConverge(postCtl, seed, postEv);	// 測光しながら目標ev=postEv へ収束
				}
			}
			// home(往復対称の基準)=次ccmの基準(=goal)。
			const bool   haveHome = validExposure(goal);
			const double homeB    = haveHome ? expo::brightnessStops(goal, tables_) : 0.0;
			double linear = -1.0;
			cmdt::HISTOGRAM hist;
			if (this->rdyMeterTimed() == ERR_HGC_OK &&
			    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
			{
				double x = expo::histMedian(hist.y, cmdt::hist_bin);
				linear = expo::srgbToLinear(x);
			}
			meteredLinear = linear;
			if (linear > 0.0)
			{
				// 露出補正(仕様 4.5): 移動平均・ヒステリシス。目標 ev=postEv。往復対称(home=次の基準)。
				avgBuf.push_back(linear);
				int n = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
				while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
				double avg = 0.0;
				for (double v : avgBuf) { avg += v; }
				avg /= static_cast<double>(avgBuf.size());
				double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : postCtl.current(), expo::ev0Cfg());
				double linU = expo::linearFromEvBase(postEv + smooth_.hysteresis / 2.0, lin0);
				double linD = expo::linearFromEvBase(postEv - smooth_.hysteresis / 2.0, lin0);
				double curB = expo::brightnessStops(postCtl.current(), tables_);
				if (avg > linU)      { if (haveHome && curB > homeB) { postCtl.stepHome(false, goal); } else { postCtl.darken(); } }
				else if (avg < linD) { if (haveHome && curB < homeB) { postCtl.stepHome(true, goal); } else { postCtl.brighten(); } }
				meterFailStreak = 0;
			}
			else
			{
				if (meterFailStreak == 0) { avgBuf.clear(); if (onError_) { onError_(ERR_HGC_RDY_METARING, "metering lost -> liveview restart"); } }
				++meterFailStreak;
				cameraController::startShooting(*dev_);
				interruptibleSleep(kMeterSettleMs);
			}
			target = postCtl.current();
			if (!validExposure(target)) { target = validExposure(lastExp) ? lastExp : nightExp; }
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
				avgBuf.clear();
				// 項目8: 自動露出→自動露出の切替で目標evが急変するとオーバーシュートするため、
				// 実効目標evは前窓の値を保持して以降 1/3 段/枚で寄せる。不連続(開始/非自動から)は即適用。
				if (!(validExposure(lastExp) && prevAuto)) { curEvT = evTraw; }
				if (validExposure(lastExp))
				{
					// 自動露出の開始(仕様 4.8): 撮影継続中の撮影制御方法切替では不連続を避け、
					// 基準(iso/ss/fn)の構成から始めて直前露出の APEX(明るさ)へ合わせて開始する。
					if (validExposure(ccm->initial)) { autoCtl.setCurrent(ccm->initial); }
					else                    { autoCtl.setCurrent(lastExp); }
					double prevB = expo::brightnessStops(lastExp, tables_);
					double curB  = expo::brightnessStops(autoCtl.current(), tables_);
					autoCtl.applyStops(prevB - curB);
				}
				else
				{
					// 最初の補正(仕様 4.4): 撮影開始直後は初期露出が不定。基準から測光しながら
					// 目標 ev へ反復収束させて 1 枚目の露出を決める(張り付き時は二分探索)。
					target = initialConverge(autoCtl, ccm->initial, evTraw);
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
				// 測光(ライブビューのヒストグラム)→ リニア輝度(仕様 4.3)
				double linear = -1.0;
				cmdt::HISTOGRAM hist;
				if (this->rdyMeterTimed() == ERR_HGC_OK &&
				    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
				{
					double x = expo::histMedian(hist.y, cmdt::hist_bin);
					linear = expo::srgbToLinear(x);
				}
				meteredLinear = linear;	// 測光値をログ用に保持(失敗時は-1)

				if (linear > 0.0)
				{
					// 露出補正(仕様 4.5): 移動平均とヒステリシス帯(項目7: ccm 個別値を優先)
					avgBuf.push_back(linear);
					int n = effMA;
					while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
					double avg = 0.0;
					for (double v : avgBuf) { avg += v; }
					avg /= static_cast<double>(avgBuf.size());

					double lin0 = expo::ev0LinearForMeasure(linear, validExposure(lastExp) ? lastExp : autoCtl.current(), expo::ev0Cfg());
					double linU = expo::linearFromEvBase(evT + effHyst / 2.0, lin0);
					double linD = expo::linearFromEvBase(evT - effHyst / 2.0, lin0);
						double curB = expo::brightnessStops(autoCtl.current(), tables_);
					if (avg > linU)      { if (haveHome && curB > homeB) { autoCtl.stepHome(false, ccm->initial); } else { autoCtl.darken(); } }
					else if (avg < linD) { if (haveHome && curB < homeB) { autoCtl.stepHome(true, ccm->initial); } else { autoCtl.brighten(); } }
					meterFailStreak = 0;	// 測光成功
				}
				else
				{
					// 測光失敗。ライブビュー停止やカメラの一時不応答が起きると、以降ずっと測光
					// できず露出が凍結し日中に白飛びする(実機 06/15 で発生)。ライブビューを
					// 再開して次フレームで測光を回復させる。移動平均は陳腐化するので破棄する。
					if (meterFailStreak == 0)
					{
						avgBuf.clear();
						if (onError_) { onError_(ERR_HGC_RDY_METARING, "metering lost -> liveview restart"); }
					}
					++meterFailStreak;
					cameraController::startShooting(*dev_);	// live view を張り直す
					interruptibleSleep(kMeterSettleMs);
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
			// 収束が撮影窓より早く終わった余り時間は keepAlive で待つ。窓開始で CAPTURING + アンカー設定。
			while (running_ && static_cast<long long>(std::time(nullptr)) < startSec)
			{
				cameraController::keepAlive(*dev_);
				interruptibleSleep(1000);
			}
			if (!running_) { break; }
			if (onState_) { onState_(ST_CAPTURING); }
			mono = tool::startElapse();	// 撮影窓開始を0とする monotonic アンカー
			boundaryIdx = 0;
			continue;
		}

		// --- 通常コマ: tm0処理時間(適用+測光+計算)を確定してログ用に退避し、tm1で撮る ---
		const int tm0Ms = (tm0 != nullptr) ? static_cast<int>(tool::getElapse(tm0)) : -1;
		if (!validExposure(target)) { target = validExposure(pending) ? pending : ccm->limitBright; }	// 無効はフォールバック

		// tm1: 境界+offset まで待ってシャッター(=周期ピッタリで発光)。tm0がoffsetを超えたカメラでは即発光。
		sleepUntilElapse(mono, static_cast<long>(boundaryIdx * interval * 1000.0 + kShutterOffsetSec * 1000.0));
		err = cameraController::actShutter(*dev_);
		if (err != ERR_HGC_OK) { if (onError_) { onError_(err, "actShutter"); } ++shootFailStreak; }
		else { shootFailStreak = 0; }
		++frame;

		if (onCaptured_)
		{
			double lum = expo::brightnessStops(pending, tables_);	// 撮ったのは pending(適用済み)
			onCaptured_(capturedInfo{ frame, pending, lum, ccm->name, meteredLinear, meterMs_, applyMs, tm0Ms });
		}
		if (onProgress_)
		{
			onProgress_(progressInfo{ frame, total,
			            static_cast<int>(endSec - now), static_cast<int>(now - startSec) });
		}

		// 撮影が連続失敗 → カメラ接続が切れたとみなし再接続(3a: 中止まで無限)。復帰後は現在時刻から周期を取り直す。
		if (shootFailStreak >= kMaxConsecutiveFail)
		{
			if (onState_) { onState_(ST_NOCAMERA); }	// 撮影中のカメラ未検出(✖点灯)
			if (onError_) { onError_(ERR_HGC_NOT_FOUND, "撮影中にカメラ接続が切れました。再接続を試行します(中止するまで継続)"); }
			bool recovered = false;
			while (running_)
			{
				if (onReconnect_ && onReconnect_() && establishSession()) { recovered = true; break; }
				interruptibleSleep(kReconnectWaitMs);	// 再試行間隔(中止でsleep中断)
			}
			if (!recovered) { break; }	// 中止された(running_=false)→ 終了処理へ
			shootFailStreak = 0;
			avgBuf.clear();								// 測光移動平均をリセット
			if (onState_) { onState_(ST_CAPTURING); }	// 緑へ戻す
			mono = tool::startElapse(); boundaryIdx = 0;	// 再接続後は現在時刻から周期をアンカーし直す
			pending = target;
			continue;	// 次コマから再開(establishSession でキャッシュclear済=フル適用)
		}

		pending = target;	// 次コマの露出(パイプライン: 測光→計算した露出は 1コマ後に適用/撮影)
		++boundaryIdx;
	}

	// 最後の1枚を守る: 直前に切ったコマはカメラ側でまだ露光・保存中のことがある。ここで即
	// restoreShootingMode(設定変更POST)を送ると busy なカメラで最後の1枚が壊れる/復元失敗する。
	// 次の撮影周期境界(=露光+保存が済む時刻)まで「中断しない待ち」を入れてから終了処理へ入る。
	if (mono != nullptr && frame > 0)
	{
		const long targetMs = static_cast<long>(boundaryIdx * interval * 1000.0);
		for (long left = targetMs - static_cast<long>(tool::getElapse(mono)); left > 0;
		     left = targetMs - static_cast<long>(tool::getElapse(mono)))
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
