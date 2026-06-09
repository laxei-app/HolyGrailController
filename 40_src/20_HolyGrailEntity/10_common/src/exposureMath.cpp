#include "exposureMath.h"
#include <algorithm>

namespace expo
{
	// APEX 値を 1/3 段に量子化する。
	double snapThird(double apex)
	{
		return std::round(apex * 3.0) / 3.0;
	}

	// ヒストグラム中央値(0.0～1.0)。仕様 4.3.1。
	double histMedian(const uint16_t* lumBins, int nBins)
	{
		if (lumBins == nullptr || nBins <= 1) { return 0.0; }

		// 全要素数
		double total = 0.0;
		for (int i = 0; i < nBins; ++i) { total += lumBins[i]; }
		if (total <= 0.0) { return 0.0; }

		// 中央値(全体の半分)の位置を探す
		const double half = total / 2.0;
		double cum = 0.0;
		for (int k = 0; k < nBins; ++k)
		{
			const double binCount = lumBins[k];
			if (cum + binCount >= half)
			{
				// bin k 内での小数位置
				double frac = (binCount > 0.0) ? (half - cum) / binCount : 0.0;
				double pos = static_cast<double>(k) + frac;
				return pos / static_cast<double>(nBins - 1);
			}
			cum += binCount;
		}
		return 1.0;
	}

	// --- ローカル補助 ---
	namespace
	{
		int nearestIndex(const std::vector<double>& vals, double v)
		{
			int best = 0;
			double bestDiff = 1e300;
			for (int i = 0; i < static_cast<int>(vals.size()); ++i)
			{
				double d = std::fabs(vals[i] - v);
				if (d < bestDiff) { bestDiff = d; best = i; }
			}
			return best;
		}
	}

	void exposureCtl::init(const std::vector<uint16_t>& isoList,
	                       const std::vector<double>&   ssList,
	                       const std::vector<double>&   fnList,
	                       const hgc::exposure& limitBright,
	                       const hgc::exposure& limitDark,
	                       const hgc::exposureType priority[hgc::exposureTypeNum])
	{
		iso_.vals.clear();
		for (auto v : isoList) { iso_.vals.push_back(static_cast<double>(v)); }
		ss_.vals = ssList;
		fn_.vals = fnList;
		std::sort(iso_.vals.begin(), iso_.vals.end());	// 昇順(idx↑で明るい)
		std::sort(ss_.vals.begin(),  ss_.vals.end());	// 昇順(idx↑=長秒=明るい)
		std::sort(fn_.vals.begin(),  fn_.vals.end());	// 昇順(idx↑=大F値=暗い)
		iso_.idx = ss_.idx = fn_.idx = 0;

		limitBright_ = limitBright;
		limitDark_   = limitDark;
		for (int i = 0; i < hgc::exposureTypeNum; ++i) { priority_[i] = priority[i]; }

		rebuildCurrent();
	}

	void exposureCtl::rebuildCurrent()
	{
		if (!iso_.vals.empty()) { cur_.iso = static_cast<uint16_t>(std::lround(iso_.vals[iso_.idx])); }
		if (!ss_.vals.empty())  { cur_.ss  = ss_.vals[ss_.idx]; }
		if (!fn_.vals.empty())  { cur_.fn  = fn_.vals[fn_.idx]; }
	}

	void exposureCtl::setCurrent(const hgc::exposure& e)
	{
		if (!iso_.vals.empty()) { iso_.idx = nearestIndex(iso_.vals, static_cast<double>(e.iso)); }
		if (!ss_.vals.empty())  { ss_.idx  = nearestIndex(ss_.vals, e.ss); }
		if (!fn_.vals.empty())  { fn_.idx  = nearestIndex(fn_.vals, e.fn); }
		rebuildCurrent();
	}

	void exposureCtl::setToBrightLimit()
	{
		// iso/ss は明るい=大きい値の上限まで、fn は明るい=小さい値の下限まで。
		if (!iso_.vals.empty())
		{
			iso_.idx = 0;
			for (int i = 0; i < static_cast<int>(iso_.vals.size()); ++i)
			{
				if (limitBright_.iso == 0 || iso_.vals[i] <= static_cast<double>(limitBright_.iso)) { iso_.idx = i; }
			}
		}
		if (!ss_.vals.empty())
		{
			ss_.idx = 0;
			for (int i = 0; i < static_cast<int>(ss_.vals.size()); ++i)
			{
				if (limitBright_.ss <= 0.0 || ss_.vals[i] <= limitBright_.ss) { ss_.idx = i; }
			}
		}
		if (!fn_.vals.empty())
		{
			fn_.idx = static_cast<int>(fn_.vals.size()) - 1;
			for (int i = static_cast<int>(fn_.vals.size()) - 1; i >= 0; --i)
			{
				if (limitBright_.fn <= 0.0 || fn_.vals[i] >= limitBright_.fn) { fn_.idx = i; }
			}
		}
		rebuildCurrent();
	}

	void exposureCtl::setToDarkLimit()
	{
		if (!iso_.vals.empty())
		{
			iso_.idx = static_cast<int>(iso_.vals.size()) - 1;
			for (int i = static_cast<int>(iso_.vals.size()) - 1; i >= 0; --i)
			{
				if (limitDark_.iso == 0 || iso_.vals[i] >= static_cast<double>(limitDark_.iso)) { iso_.idx = i; }
			}
		}
		if (!ss_.vals.empty())
		{
			ss_.idx = static_cast<int>(ss_.vals.size()) - 1;
			for (int i = static_cast<int>(ss_.vals.size()) - 1; i >= 0; --i)
			{
				if (limitDark_.ss <= 0.0 || ss_.vals[i] >= limitDark_.ss) { ss_.idx = i; }
			}
		}
		if (!fn_.vals.empty())
		{
			fn_.idx = 0;
			for (int i = 0; i < static_cast<int>(fn_.vals.size()); ++i)
			{
				if (limitDark_.fn <= 0.0 || fn_.vals[i] <= limitDark_.fn) { fn_.idx = i; }
			}
		}
		rebuildCurrent();
	}

	bool exposureCtl::stepIso(bool bright)
	{
		if (iso_.vals.empty()) { return false; }
		if (bright)
		{
			int n = iso_.idx + 1;
			if (n < static_cast<int>(iso_.vals.size()) &&
			    (limitBright_.iso == 0 || iso_.vals[n] <= static_cast<double>(limitBright_.iso)))
			{ iso_.idx = n; return true; }
		}
		else
		{
			int n = iso_.idx - 1;
			if (n >= 0 &&
			    (limitDark_.iso == 0 || iso_.vals[n] >= static_cast<double>(limitDark_.iso)))
			{ iso_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepSs(bool bright)
	{
		if (ss_.vals.empty()) { return false; }
		if (bright)
		{
			int n = ss_.idx + 1;	// 長秒=明るい
			if (n < static_cast<int>(ss_.vals.size()) &&
			    (limitBright_.ss <= 0.0 || ss_.vals[n] <= limitBright_.ss))
			{ ss_.idx = n; return true; }
		}
		else
		{
			int n = ss_.idx - 1;
			if (n >= 0 &&
			    (limitDark_.ss <= 0.0 || ss_.vals[n] >= limitDark_.ss))
			{ ss_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepFn(bool bright)
	{
		if (fn_.vals.empty()) { return false; }
		if (bright)
		{
			int n = fn_.idx - 1;	// 小F値=明るい
			if (n >= 0 &&
			    (limitBright_.fn <= 0.0 || fn_.vals[n] >= limitBright_.fn))
			{ fn_.idx = n; return true; }
		}
		else
		{
			int n = fn_.idx + 1;	// 大F値=暗い
			if (n < static_cast<int>(fn_.vals.size()) &&
			    (limitDark_.fn <= 0.0 || fn_.vals[n] <= limitDark_.fn))
			{ fn_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepOne(bool bright)
	{
		// 優先度の上位から、限界に達していない項目を 1 段変更する。
		for (int i = 0; i < hgc::exposureTypeNum; ++i)
		{
			bool ok = false;
			switch (priority_[i])
			{
			case hgc::exposureType::iso: ok = stepIso(bright); break;
			case hgc::exposureType::ss:  ok = stepSs(bright);  break;
			case hgc::exposureType::fn:  ok = stepFn(bright);  break;
			default: break;
			}
			if (ok) { rebuildCurrent(); return true; }
		}
		return false;
	}

	bool exposureCtl::brighten() { return stepOne(true); }
	bool exposureCtl::darken()   { return stepOne(false); }

	hgc::exposure exposureCtl::applyStops(double evStops)
	{
		// 1/3 段刻みで近い段数だけ変更する。
		int steps = static_cast<int>(std::lround(evStops * 3.0));
		bool bright = (steps > 0);
		int n = std::abs(steps);
		for (int i = 0; i < n; ++i)
		{
			if (!stepOne(bright)) { break; }	// 限界に達したら止める
		}
		return cur_;
	}
}
