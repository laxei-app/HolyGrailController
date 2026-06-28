#include "exposureMath.h"
#include <algorithm>
#include <cstdlib>

namespace expo
{
	// APEX 値を 1/3 段に量子化する。
	double snapThird(double apex)
	{
		return std::round(apex * 3.0) / 3.0;
	}

	// 測光リニア輝度とその露出設定から ev0 のリニア輝度を求める(仕様 4.3.3 環境光 + 4.3.4)。
	double ev0LinearForMeasure(double linear, const hgc::exposure& e, const ev0Sigmoid& s)
	{
		double iso = parseValue(e.iso, expoKind::iso);
		double ss  = parseValue(e.ss,  expoKind::ss);
		double fn  = parseValue(e.fn,  expoKind::fn);
		if (linear <= 0.0 || iso <= 0.0 || ss <= 0.0 || fn <= 0.0)
		{
			return s.linearHi;	// 露出/測光が無効 → 従来どおり 18%(安全側)
		}
		double bv = ambientBv(linear, avFromFn(fn), tvFromSs(ss), svFromIso(iso));
		return ev0LinearFromBv(bv, s);
	}

	// ヒストグラム中央値(0.0～1.0)。仕様 4.3.1。
	double histMedian(const uint16_t* lumBins, int nBins)
	{
		if (lumBins == nullptr || nBins <= 1) { return 0.0; }
		double total = 0.0;
		for (int i = 0; i < nBins; ++i) { total += lumBins[i]; }
		if (total <= 0.0) { return 0.0; }
		const double half = total / 2.0;
		double cum = 0.0;
		for (int k = 0; k < nBins; ++k)
		{
			const double binCount = lumBins[k];
			if (cum + binCount >= half)
			{
				double frac = (binCount > 0.0) ? (half - cum) / binCount : 0.0;
				double pos = static_cast<double>(k) + frac;
				return pos / static_cast<double>(nBins - 1);
			}
			cum += binCount;
		}
		return 1.0;
	}

	// --- 設定可能値テーブル ---

	double parseValue(const std::string& v, expoKind k)
	{
		if (v.empty()) { return -1.0; }
		if (k == expoKind::ss)
		{
			size_t slash = v.find('/');
			if (slash != std::string::npos)	// "1/4000"
			{
				double num = std::atof(v.substr(0, slash).c_str());
				double den = std::atof(v.substr(slash + 1).c_str());
				if (den == 0.0) { return -1.0; }
				return num / den;
			}
			char* end = nullptr;
			double s = std::strtod(v.c_str(), &end);	// "8" "0.5" "1.3" "30"
			return (end != v.c_str() && s > 0.0) ? s : -1.0;	// "Bulb"等は -1
		}
		char* end = nullptr;
		double r = std::strtod(v.c_str(), &end);	// iso "3200" / fn "1.4"
		return (end != v.c_str() && r > 0.0) ? r : -1.0;
	}

	double npfShutterSec(double sensorW_mm, double pixelW, double focal_mm, double fn)
	{
		if (sensorW_mm <= 0.0 || pixelW <= 0.0 || focal_mm <= 0.0 || fn <= 0.0) { return 0.0; }
		double pitchUm = sensorW_mm / pixelW * 1000.0;	// 画素ピッチ[µm]
		return (35.0 * fn + 30.0 * pitchUm) / focal_mm;
	}

	namespace
	{
		double apexOf(double real, expoKind k)
		{
			switch (k)
			{
			case expoKind::iso: return svFromIso(real);
			case expoKind::fn:  return avFromFn(real);
			default:            return tvFromSs(real);	// ss
			}
		}
	}

	std::vector<expoEntry> buildTable(const std::vector<std::string>& values, expoKind k)
	{
		std::vector<expoEntry> t;
		for (const auto& v : values)
		{
			double r = parseValue(v, k);
			if (r <= 0.0) { continue; }	// 無効値(Bulb等)は除外
			expoEntry e;
			e.value = v;
			e.real  = r;
			e.apex  = snapThird(apexOf(r, k));
			t.push_back(e);
		}
		// real 昇順(iso/ss は idx↑で明るい、fn は idx↑で暗い、になるよう)。
		std::sort(t.begin(), t.end(), [](const expoEntry& a, const expoEntry& b) { return a.real < b.real; });
		return t;
	}

	std::vector<std::string> standardValues(expoKind k)
	{
		if (k == expoKind::iso)
		{
			return { "100","125","160","200","250","320","400","500","640","800","1000","1250",
			         "1600","2000","2500","3200","4000","5000","6400","8000","10000","12800",
			         "16000","20000","25600","32000","40000","51200" };
		}
		// ss(1/3段。Bulb は除外)
		return { "1/8000","1/6400","1/5000","1/4000","1/3200","1/2500","1/2000","1/1600","1/1250",
		         "1/1000","1/800","1/640","1/500","1/400","1/320","1/250","1/200","1/160","1/125",
		         "1/100","1/80","1/60","1/50","1/40","1/30","1/25","1/20","1/15","1/13","1/10",
		         "1/8","1/6","1/5","1/4","0.3","0.4","0.5","0.6","0.8","1","1.3","1.6","2","2.5",
		         "3.2","4","5","6","8","10","13","15","20","25","30" };
	}

	std::vector<std::string> standardFn(double fnMin, double fnMax)
	{
		// 標準的な 1/3 段の F 値(文字列)。レンズの開放〜最小絞り範囲で絞り込む。
		static const std::vector<std::string> all = {
			"1.0","1.1","1.2","1.4","1.6","1.8","2.0","2.2","2.5","2.8","3.2","3.5","4.0","4.5",
			"5.0","5.6","6.3","7.1","8","9","10","11","13","14","16","18","20","22","25","29","32" };
		std::vector<std::string> out;
		for (const auto& v : all)
		{
			double f = std::atof(v.c_str());
			if (f >= fnMin - 1e-6 && f <= fnMax + 1e-6) { out.push_back(v); }
		}
		if (out.empty()) { out = all; }	// 範囲外なら全部
		return out;
	}

	expoTables standardTables(double fnMin, double fnMax)
	{
		expoTables t;
		t.iso = buildTable(standardValues(expoKind::iso), expoKind::iso);
		t.ss  = buildTable(standardValues(expoKind::ss),  expoKind::ss);
		t.fn  = buildTable(standardFn(fnMin, fnMax),       expoKind::fn);
		return t;
	}

	namespace
	{
		// 露出値文字列の apex をテーブルから引く。無ければ実数から算出。無効は 0。
		double apexFromTable(const std::vector<expoEntry>& tab, const std::string& v, expoKind k)
		{
			for (const auto& e : tab) { if (e.value == v) { return e.apex; } }
			double r = parseValue(v, k);
			return (r > 0.0) ? snapThird(apexOf(r, k)) : 0.0;
		}

		int nearestIndexReal(const std::vector<expoEntry>& e, double real)
		{
			int best = 0;
			double bestDiff = 1e300;
			for (int i = 0; i < static_cast<int>(e.size()); ++i)
			{
				double d = std::fabs(e[i].real - real);
				if (d < bestDiff) { bestDiff = d; best = i; }
			}
			return best;
		}
	}

	double brightnessStops(const hgc::exposure& e, const expoTables& t)
	{
		// Sv - Av - Tv。Sv↑=明るい、Av↑(大F)=暗い、Tv↑(短秒)=暗い。
		return apexFromTable(t.iso, e.iso, expoKind::iso)
		     - apexFromTable(t.fn,  e.fn,  expoKind::fn)
		     - apexFromTable(t.ss,  e.ss,  expoKind::ss);
	}

	// --- exposureCtl(テーブル基準) ---

	void exposureCtl::init(const expoTables& tables,
	                       const hgc::exposure& limitBright,
	                       const hgc::exposure& limitDark,
	                       const hgc::exposureType priority[hgc::exposureTypeNum])
	{
		iso_.e = tables.iso; ss_.e = tables.ss; fn_.e = tables.fn;	// real昇順済み
		iso_.idx = ss_.idx = fn_.idx = 0;

		// 限界の実数(空=0=限界なし)
		limBIso_ = parseValue(limitBright.iso, expoKind::iso); if (limBIso_ < 0) { limBIso_ = 0; }
		limDIso_ = parseValue(limitDark.iso,   expoKind::iso); if (limDIso_ < 0) { limDIso_ = 0; }
		limBSs_  = parseValue(limitBright.ss,  expoKind::ss);  if (limBSs_  < 0) { limBSs_  = 0; }
		limDSs_  = parseValue(limitDark.ss,    expoKind::ss);  if (limDSs_  < 0) { limDSs_  = 0; }
		limBFn_  = parseValue(limitBright.fn,  expoKind::fn);  if (limBFn_  < 0) { limBFn_  = 0; }
		limDFn_  = parseValue(limitDark.fn,    expoKind::fn);  if (limDFn_  < 0) { limDFn_  = 0; }

		for (int i = 0; i < hgc::exposureTypeNum; ++i) { priority_[i] = priority[i]; }
		rebuildCurrent();
	}

	void exposureCtl::rebuildCurrent()
	{
		if (!iso_.e.empty()) { cur_.iso = iso_.e[iso_.idx].value; }
		if (!ss_.e.empty())  { cur_.ss  = ss_.e[ss_.idx].value; }
		if (!fn_.e.empty())  { cur_.fn  = fn_.e[fn_.idx].value; }
	}

	void exposureCtl::setCurrent(const hgc::exposure& e)
	{
		double ri = parseValue(e.iso, expoKind::iso);
		double rs = parseValue(e.ss,  expoKind::ss);
		double rf = parseValue(e.fn,  expoKind::fn);
		if (!iso_.e.empty() && ri > 0) { iso_.idx = nearestIndexReal(iso_.e, ri); }
		if (!ss_.e.empty()  && rs > 0) { ss_.idx  = nearestIndexReal(ss_.e,  rs); }
		if (!fn_.e.empty()  && rf > 0) { fn_.idx  = nearestIndexReal(fn_.e,  rf); }
		rebuildCurrent();
	}

	void exposureCtl::setToBrightLimit()
	{
		// iso/ss は明るい=大きい実数の上限まで、fn は明るい=小さい実数の下限まで。
		if (!iso_.e.empty())
		{
			iso_.idx = 0;
			for (int i = 0; i < static_cast<int>(iso_.e.size()); ++i)
			{ if (limBIso_ == 0 || iso_.e[i].real <= limBIso_) { iso_.idx = i; } }
		}
		if (!ss_.e.empty())
		{
			ss_.idx = 0;
			for (int i = 0; i < static_cast<int>(ss_.e.size()); ++i)
			{ if (limBSs_ == 0 || ss_.e[i].real <= limBSs_) { ss_.idx = i; } }
		}
		if (!fn_.e.empty())
		{
			fn_.idx = static_cast<int>(fn_.e.size()) - 1;
			for (int i = static_cast<int>(fn_.e.size()) - 1; i >= 0; --i)
			{ if (limBFn_ == 0 || fn_.e[i].real >= limBFn_) { fn_.idx = i; } }
		}
		rebuildCurrent();
	}

	void exposureCtl::setToDarkLimit()
	{
		if (!iso_.e.empty())
		{
			iso_.idx = static_cast<int>(iso_.e.size()) - 1;
			for (int i = static_cast<int>(iso_.e.size()) - 1; i >= 0; --i)
			{ if (limDIso_ == 0 || iso_.e[i].real >= limDIso_) { iso_.idx = i; } }
		}
		if (!ss_.e.empty())
		{
			ss_.idx = static_cast<int>(ss_.e.size()) - 1;
			for (int i = static_cast<int>(ss_.e.size()) - 1; i >= 0; --i)
			{ if (limDSs_ == 0 || ss_.e[i].real >= limDSs_) { ss_.idx = i; } }
		}
		if (!fn_.e.empty())
		{
			fn_.idx = 0;
			for (int i = 0; i < static_cast<int>(fn_.e.size()); ++i)
			{ if (limDFn_ == 0 || fn_.e[i].real <= limDFn_) { fn_.idx = i; } }
		}
		rebuildCurrent();
	}

	bool exposureCtl::stepIso(bool bright)
	{
		if (iso_.e.empty()) { return false; }
		if (bright)
		{
			int n = iso_.idx + 1;
			if (n < static_cast<int>(iso_.e.size()) && (limBIso_ == 0 || iso_.e[n].real <= limBIso_))
			{ iso_.idx = n; return true; }
		}
		else
		{
			int n = iso_.idx - 1;
			if (n >= 0 && (limDIso_ == 0 || iso_.e[n].real >= limDIso_))
			{ iso_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepSs(bool bright)
	{
		if (ss_.e.empty()) { return false; }
		if (bright)
		{
			int n = ss_.idx + 1;	// 長秒=明るい
			if (n < static_cast<int>(ss_.e.size()) && (limBSs_ == 0 || ss_.e[n].real <= limBSs_))
			{ ss_.idx = n; return true; }
		}
		else
		{
			int n = ss_.idx - 1;
			if (n >= 0 && (limDSs_ == 0 || ss_.e[n].real >= limDSs_))
			{ ss_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepFn(bool bright)
	{
		if (fn_.e.empty()) { return false; }
		if (bright)
		{
			int n = fn_.idx - 1;	// 小F値=明るい
			if (n >= 0 && (limBFn_ == 0 || fn_.e[n].real >= limBFn_))
			{ fn_.idx = n; return true; }
		}
		else
		{
			int n = fn_.idx + 1;	// 大F値=暗い
			if (n < static_cast<int>(fn_.e.size()) && (limDFn_ == 0 || fn_.e[n].real <= limDFn_))
			{ fn_.idx = n; return true; }
		}
		return false;
	}

	bool exposureCtl::stepOne(bool bright, bool reverse)
	{
		for (int k = 0; k < hgc::exposureTypeNum; ++k)
		{
			// reverse=true なら優先度の低い軸(配列末尾)から先に動かす(§4.5 往復対称)。
			int i = reverse ? (hgc::exposureTypeNum - 1 - k) : k;
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

	bool exposureCtl::brighten(bool reverse) { return stepOne(true, reverse); }
	bool exposureCtl::darken(bool reverse)   { return stepOne(false, reverse); }

	bool exposureCtl::stepHome(bool bright, const hgc::exposure& home)
	{
		// home の各軸を設定可能値テーブルの最近傍 index に合わせる。
		double ri = parseValue(home.iso, expoKind::iso);
		double rs = parseValue(home.ss,  expoKind::ss);
		double rf = parseValue(home.fn,  expoKind::fn);
		int hi = (!iso_.e.empty() && ri > 0) ? nearestIndexReal(iso_.e, ri) : iso_.idx;
		int hs = (!ss_.e.empty()  && rs > 0) ? nearestIndexReal(ss_.e,  rs) : ss_.idx;
		int hf = (!fn_.e.empty()  && rf > 0) ? nearestIndexReal(fn_.e,  rf) : fn_.idx;
		// 優先度の逆順(低い軸が先)で、home からずれている軸を1つだけ戻す。
		for (int k = 0; k < hgc::exposureTypeNum; ++k)
		{
			int i = hgc::exposureTypeNum - 1 - k;
			bool ok = false;
			switch (priority_[i])
			{
			case hgc::exposureType::iso: if (iso_.idx != hi) { ok = stepIso(bright); } break;
			case hgc::exposureType::ss:  if (ss_.idx  != hs) { ok = stepSs(bright);  } break;
			case hgc::exposureType::fn:  if (fn_.idx  != hf) { ok = stepFn(bright);  } break;
			default: break;
			}
			if (ok) { rebuildCurrent(); return true; }
		}
		return stepOne(bright, false);	// ずれた軸が無い/動かせない → 通常の優先度順で1段
	}

	hgc::exposure exposureCtl::applyStops(double evStops)
	{
		int steps = static_cast<int>(std::lround(evStops * 3.0));
		bool bright = (steps > 0);
		int n = std::abs(steps);
		for (int i = 0; i < n; ++i) { if (!stepOne(bright)) { break; } }
		return cur_;
	}
}
