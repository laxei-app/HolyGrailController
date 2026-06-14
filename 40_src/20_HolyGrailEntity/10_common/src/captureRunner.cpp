#include "common.h"
#include "captureRunner.h"
#include "osSystemCall.h"
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
		uint32_t chunk = (ms > 100) ? 100u : static_cast<uint32_t>(ms);
		tool::sleep(chunk);
		ms -= chunk;
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
	return hgc::exposure{};
}

// 最初の補正(仕様 4.4)を反復収束で行う。撮影開始直後は初期露出が不定なので、
// 初期値(明所限界 or 暗所限界)から始め、ライブビューを測光して目標 ev へ寄せる。
//  - 測光が信用できる(張り付いていない)ときは仕様 4.4 のニュートン段(目標との差ぶん移動)。
//  - 明側/暗側に張り付いているときは測光値を信用せず、明限界〜暗限界のブラケットを
//    方向で狭めて中央へ二分探索的に寄せる(張り付き解消後はニュートン段に戻る)。
// 最大 kInitConvergeTries 回。許容内に入れば終了、ダメでも最良点で撮影に入る(手順5/6)。
// ctl は呼び出し前に init 済みであること。戻り値=1枚目の露出(ctl もその値になる)。
hgc::exposure captureRunner::initialConverge(expo::exposureCtl& ctl, bool initialBright, double evT)
{
	const double linT = expo::linearFromEv(evT);	// 目標リニア輝度(ev0=0.18 基準)

	// 許容範囲を段で取り、ブラケットの初期値とする(明側=最も明るい/暗側=最も暗い)。
	ctl.setToBrightLimit();
	double hiB = expo::brightnessStops(ctl.current(), tables_);
	ctl.setToDarkLimit();
	double loB = expo::brightnessStops(ctl.current(), tables_);

	// 仕様 4.4 の初期値から開始(明所限界=limitDark / 暗所限界=limitBright)。
	if (initialBright) { ctl.setToDarkLimit(); } else { ctl.setToBrightLimit(); }

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
		if (cameraController::rdyMetering(*dev_) == ERR_HGC_OK &&
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

		const double err = expo::evFromLinear(linT, linear);	// log2(linear/linT): + 明るすぎ / - 暗すぎ
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

errCode captureRunner::loop(void)
{
	if (onState_) { onState_(ST_CAPTURING); }

	// 撮影モードに入る
	errCode err = cameraController::startShooting(*dev_);
	if (err != ERR_HGC_OK)
	{
		if (onError_) { onError_(err, "startShooting"); }
		if (onState_) { onState_(ST_ERROR); }
		running_ = false;
		return err;
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

	const long long startSec = hgc::toUnixUtc(plan_.start, off_);
	const long long endSec   = hgc::toUnixUtc(plan_.end, off_);
	const double interval = (plan_.interval > 0.0) ? plan_.interval : 15.0;
	int total = static_cast<int>((endSec - startSec) / interval);
	if (total < 1) { total = 1; }

	const hgc::ccmWindow* curWin = nullptr;
	expo::exposureCtl autoCtl;		// 自動露出用
	expo::exposureCtl preCtl;		// 夜間前移行用(自動露出→夜間)
	expo::exposureCtl postCtl;		// 夜間後移行用(夜間→次の自動露出)
	std::vector<double> avgBuf;		// リニア輝度の移動平均バッファ
	hgc::exposure lastExp{};		// 直近の露出設定
	int frame = 0;

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

	while (running_)
	{
		const long long now = static_cast<long long>(std::time(nullptr));
		if (now >= endSec) { break; }				// 計画終了
		if (now < startSec) { interruptibleSleep(500); continue; }	// 開始前は待つ

		const hgc::ccmWindow* w = activeWindow(now);
		if (w == nullptr || !w->ccm) { interruptibleSleep(500); continue; }	// 隙間

		const bool windowChanged = (w != curWin);
		curWin = w;
		const hgc::ccmBase* ccm = w->ccm.get();

		hgc::exposure target{};
		double meteredLinear = -1.0;	// 測光したリニア輝度(自動補正時のみ。<0=測光なし)

		if (ccm->type == hgc::ccmType::night)
		{
			// 固定露出(仕様 3.2)
			target = ccm->limitBright;
		}
		else if (ccm->type == hgc::ccmType::preNight)
		{
			// 夜間前移行(仕様 3.7): 自動露出から夜間固定露出へ 1/3 段ずつ収束する。
			hgc::exposure goal = nightGoalAfter(now);
			if (windowChanged)
			{
				preCtl.init(tables_, hgc::exposure{}, hgc::exposure{}, ccm->priority);
				preCtl.setCurrent(validExposure(lastExp) ? lastExp : goal);
			}
			if (validExposure(goal))
			{
				double cur = expo::brightnessStops(preCtl.current(), tables_);
				double gl  = expo::brightnessStops(goal, tables_);
				const double third = 1.0 / 3.0;
				if (cur - gl > third / 2.0)      { preCtl.darken(); }
				else if (gl - cur > third / 2.0) { preCtl.brighten(); }
			}
			target = preCtl.current();
			if (!validExposure(target)) { target = goal; }
		}
		else if (ccm->type == hgc::ccmType::postNight)
		{
			// 夜間後移行(仕様 3.9): 夜間固定露出から次の自動露出へなめらかに移行する。
			// 次の撮影制御方法では優先度の低い(固定したい)露出設定を先に確定させたいので、
			// 次の撮影制御方法の優先度を逆順にして、その初期露出へ 1/3 段ずつ収束させる。
			const hgc::ccmBase* nextC = nextAutoCcmAfter(now);
			hgc::exposure goal = nextC ? (nextC->initialBright ? nextC->limitDark : nextC->limitBright)
			                           : hgc::exposure{};
			// 直前の夜間撮影の「夜間後露出補正」を収束目標の明るさへ加える(仕様 7.4.10)。
			double postEv = 0.0;
			for (const auto& ww : plan_.ccmList)
			{
				if (!ww.ccm || ww.ccm->type != hgc::ccmType::night) { continue; }
				if (hgc::toUnixUtc(ww.start, off_) <= now)
				{ postEv = static_cast<const hgc::ccmNight*>(ww.ccm.get())->postNightEv; }
			}
			if (windowChanged)
			{
				hgc::exposureType rev[hgc::exposureTypeNum];
				for (int k = 0; k < hgc::exposureTypeNum; ++k)
				{
					rev[k] = nextC ? nextC->priority[hgc::exposureTypeNum - 1 - k] : ccm->priority[k];
				}
				postCtl.init(tables_, hgc::exposure{}, hgc::exposure{}, rev);
				postCtl.setCurrent(validExposure(lastExp) ? lastExp : goal);
			}
			if (validExposure(goal))
			{
				double cur = expo::brightnessStops(postCtl.current(), tables_);
				double gl  = expo::brightnessStops(goal, tables_) + postEv;	// 夜間後露出補正
				const double third = 1.0 / 3.0;
				if (cur - gl > third / 2.0)      { postCtl.darken(); }
				else if (gl - cur > third / 2.0) { postCtl.brighten(); }
			}
			target = postCtl.current();
			if (!validExposure(target)) { target = validExposure(lastExp) ? lastExp : goal; }
		}
		else if (isAuto(ccm->type))
		{
			const double evT = targetEv(ccm);
			bool didInitConverge = false;
			if (windowChanged)
			{
				autoCtl.init(tables_, ccm->limitBright, ccm->limitDark, ccm->priority);
				avgBuf.clear();
				if (validExposure(lastExp))
				{
					// 自動露出の開始(仕様 4.8): 撮影継続中の撮影制御方法切替では不連続を避け、
					// 直前の露出の APEX(明るさ)に、自分の初期値から優先度に従って合わせて開始する。
					if (ccm->initialBright) { autoCtl.setToDarkLimit(); }
					else                    { autoCtl.setToBrightLimit(); }
					double prevB = expo::brightnessStops(lastExp, tables_);
					double curB  = expo::brightnessStops(autoCtl.current(), tables_);
					autoCtl.applyStops(prevB - curB);
				}
				else
				{
					// 最初の補正(仕様 4.4): 撮影開始直後は初期露出が不定。初期値から測光しながら
					// 目標 ev へ反復収束させて 1 枚目の露出を決める(張り付き時は二分探索)。
					target = initialConverge(autoCtl, ccm->initialBright, evT);
					didInitConverge = true;
				}
			}

			if (!didInitConverge)
			{
				// 測光(ライブビューのヒストグラム)→ リニア輝度(仕様 4.3)
				double linear = -1.0;
				cmdt::HISTOGRAM hist;
				if (cameraController::rdyMetering(*dev_) == ERR_HGC_OK &&
				    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
				{
					double x = expo::histMedian(hist.y, cmdt::hist_bin);
					linear = expo::srgbToLinear(x);
				}
				meteredLinear = linear;	// 測光値をログ用に保持(失敗時は-1)

				if (linear > 0.0)
				{
					// 露出補正(仕様 4.5): 移動平均とヒステリシス帯
					avgBuf.push_back(linear);
					int n = (smooth_.movingAverage > 0) ? smooth_.movingAverage : 5;
					while (static_cast<int>(avgBuf.size()) > n) { avgBuf.erase(avgBuf.begin()); }
					double avg = 0.0;
					for (double v : avgBuf) { avg += v; }
					avg /= static_cast<double>(avgBuf.size());

					double linU = expo::linearFromEv(evT + smooth_.hysteresis / 2.0);
					double linD = expo::linearFromEv(evT - smooth_.hysteresis / 2.0);
					if (avg > linU)      { autoCtl.darken(); }
					else if (avg < linD) { autoCtl.brighten(); }
				}
				target = autoCtl.current();
			}
		}
		else
		{
			target = ccm->limitBright;	// その他はフォールバック
		}

		// 露出設定が無効ならスキップして待機
		if (!validExposure(target))
		{
			interruptibleSleep(static_cast<long>(interval * 1000.0));
			continue;
		}

		// 露出を設定して撮影(仕様 4章)。周期計測のため経過を測る。
		void* el = tool::startElapse();
		cmdt::shotSet shot(target.ss, target.fn, target.iso);	// カメラ設定値の文字列
		errCode setErr = cameraController::rdyShutter(*dev_, shot);
		if (setErr != ERR_HGC_OK && onError_)
		{
			// 露出設定(iso/ss/fn)がカメラに反映できなかった。握りつぶさず通知する。
			onError_(setErr, "rdyShutter(露出設定失敗)");
		}
		err = cameraController::actShutter(*dev_);
		if (err != ERR_HGC_OK)
		{
			if (onError_) { onError_(err, "actShutter"); }
		}
		lastExp = target;
		++frame;

		if (onCaptured_)
		{
			double lum = expo::brightnessStops(target, tables_);
			onCaptured_(capturedInfo{ frame, target, lum, ccm->name, meteredLinear });
		}
		if (onProgress_)
		{
			onProgress_(progressInfo{ frame, total,
			            static_cast<int>(endSec - now), static_cast<int>(now - startSec) });
		}

		// 撮影周期を維持(仕様 4.1: 撮影間隔は最優先で守る)
		uint32_t spent = tool::getElapse(el);
		long waitMs = static_cast<long>(interval * 1000.0) - static_cast<long>(spent);
		interruptibleSleep(waitMs);
	}

	running_ = false;
	if (onState_) { onState_(ST_IDLE); }
	return ERR_HGC_OK;
}
