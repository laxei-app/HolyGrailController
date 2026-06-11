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
	expo::exposureCtl linCtl;		// リニア移行用
	std::vector<double> avgBuf;		// リニア輝度の移動平均バッファ
	hgc::exposure lastExp{};		// 直近の露出設定
	int frame = 0;

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

		if (ccm->type == hgc::ccmType::night)
		{
			// 固定露出(仕様 3.2)
			target = ccm->limitBright;
		}
		else if (ccm->type == hgc::ccmType::linear)
		{
			// 夜間固定露出へ 1/3 段ずつ収束(仕様 3.7)
			hgc::exposure goal = nightGoalAfter(now);
			if (windowChanged)
			{
				linCtl.init(tables_, hgc::exposure{}, hgc::exposure{}, ccm->priority);
				linCtl.setCurrent(validExposure(lastExp) ? lastExp : goal);
			}
			if (validExposure(goal))
			{
				double cur = expo::brightnessStops(linCtl.current(), tables_);
				double gl  = expo::brightnessStops(goal, tables_);
				const double third = 1.0 / 3.0;
				if (cur - gl > third / 2.0)      { linCtl.darken(); }
				else if (gl - cur > third / 2.0) { linCtl.brighten(); }
			}
			target = linCtl.current();
			if (!validExposure(target)) { target = goal; }
		}
		else if (isAuto(ccm->type))
		{
			const double evT = targetEv(ccm);
			if (windowChanged)
			{
				autoCtl.init(tables_, ccm->limitBright, ccm->limitDark, ccm->priority);
				avgBuf.clear();
			}

			// 測光(ライブビューのヒストグラム)→ リニア輝度(仕様 4.3)
			double linear = -1.0;
			cmdt::HISTOGRAM hist;
			if (cameraController::rdyMetering(*dev_) == ERR_HGC_OK &&
			    cameraController::alzMetering(*dev_, hist) == ERR_HGC_OK)
			{
				double x = expo::histMedian(hist.y, cmdt::hist_bin);
				linear = expo::srgbToLinear(x);
			}

			if (linear > 0.0)
			{
				if (windowChanged)
				{
					// 最初の補正(仕様 4.4): 初期値(明所限界 or 暗所限界)から目標evへ寄せる。
					double evD = expo::evFromLinear(expo::EV0_LINEAR, linear);	// log2(linear/0.18)
					if (ccm->initialBright) { autoCtl.setToDarkLimit(); }   // 明所限界(limitDark)を初期値に
					else                    { autoCtl.setToBrightLimit(); } // 暗所限界(limitBright)を初期値に
					autoCtl.applyStops(evT - evD);
				}
				else
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
			}
			target = autoCtl.current();
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
		cameraController::rdyShutter(*dev_, shot);
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
			onCaptured_(capturedInfo{ frame, target, lum, ccm->name });
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
