#include "exposureMath.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace expo
{
	// APEX 値を stepStops 段のグリッドに量子化する。
	double snapStops(double apex, double stepStops)
	{
		const double step = (stepStops > 0.0) ? stepStops : (1.0 / 3.0);
		return std::round(apex / step) * step;
	}

	// 1/3 段(標準テーブルと、刻みを言わない呼び出しの既定)。
	double snapThird(double apex)
	{
		return snapStops(apex, 1.0 / 3.0);
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

	std::vector<expoEntry> buildTable(const std::vector<std::string>& values, expoKind k,
	                                  double stepStops, const std::vector<double>* reals)
	{
		std::vector<expoEntry> t;
		// 論理値は文字列と同じ並びで来る。長さが合わなければ信用せず文字列から作る。
		const bool useReals = (reals != nullptr && reals->size() == values.size());
		for (size_t i = 0; i < values.size(); ++i)
		{
			const std::string& v = values[i];
			double r = useReals ? (*reals)[i] : parseValue(v, k);
			if (r <= 0.0) { continue; }	// 無効値(Bulb等)は除外
			expoEntry e;
			e.value = v;
			e.real  = r;
			e.apex  = snapStops(apexOf(r, k), stepStops);
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

	namespace
	{
		// 目盛りの文字列。expo::parseValue が読み戻せる書き方(apiBuiltin の並びと同じ流儀)。
		std::string gridText(expoKind k, double v)
		{
			char b[32];
			if (k == expoKind::iso) { std::snprintf(b, sizeof(b), "%d", static_cast<int>(std::floor(v + 0.5))); return b; }
			if (k == expoKind::fn)
			{
				std::snprintf(b, sizeof(b), "%.2f", v);		// 1/12 段は "1.54" のように 2 桁が要る
				std::string s = b;
				if (s.size() > 2 && s.back() == '0') { s.pop_back(); }	// "1.50"→"1.5"、"2.00"→"2.0"
				return s;
			}
			const int denom = static_cast<int>(1.0 / v + 0.5);
			if (denom <= 1)
			{
				// 20 秒以上は整数で十分(1/12 段でも 1 秒以上離れる)。"48" が "47.9" と出ないように。
				if (v >= 20.0 || std::fabs(v - std::floor(v + 0.5)) < 0.05) { std::snprintf(b, sizeof(b), "%.0f", v); }
				else                                                         { std::snprintf(b, sizeof(b), "%.1f", v); }
			}
			else { std::snprintf(b, sizeof(b), "1/%d", denom); }
			return b;
		}
		// base × 2^(n/perStop) を lo〜hi(3% の余裕)で並べる。同じ綴りは1つにする。
		std::vector<std::string> grid(expoKind k, int stepsPerStop, double lo, double hi)
		{
			const double base    = (k == expoKind::iso) ? 100.0 : 1.0;
			const double perStop = (k == expoKind::fn) ? 2.0 * stepsPerStop : static_cast<double>(stepsPerStop);	// F は 1段=比√2
			std::vector<std::string> out;
			for (int n = -600; n <= 600; ++n)
			{
				const double v = base * std::pow(2.0, n / perStop);
				if (v < lo / 1.03) { continue; }
				if (v > hi * 1.03) { break; }
				const std::string s = gridText(k, v);
				if (out.empty() || out.back() != s) { out.push_back(s); }
			}
			return out;
		}
		// 慣用の表記の一覧を範囲(1/6 段の余裕)で絞る。
		std::vector<std::string> clip(const std::vector<std::string>& all, expoKind k, double lo, double hi)
		{
			std::vector<std::string> out;
			const double tol = std::pow(2.0, 1.0 / 6.0);
			for (const auto& s : all)
			{
				const double r = parseValue(s, k);
				if (r <= 0.0) { continue; }
				const double rr = (k == expoKind::fn) ? r * r : r;		// F は 2 乗が明るさ比
				const double lo2 = (k == expoKind::fn) ? lo * lo : lo, hi2 = (k == expoKind::fn) ? hi * hi : hi;
				if (rr >= lo2 / tol && rr <= hi2 * tol) { out.push_back(s); }
			}
			return out;
		}
	}

	std::vector<std::string> presetValues(expoKind k, bool forPhone)
	{
		if (forPhone)
		{
			if (k == expoKind::iso) { return grid(k, 12, 20.0, 12800.0); }
			if (k == expoKind::ss)  { return grid(k, 12, 1.0 / 50000.0, 48.0); }
			return grid(k, 12, 1.5, 3.5);
		}
		if (k == expoKind::iso) { return clip(standardValues(k), k, 100.0, 24000.0); }
		if (k == expoKind::ss)
		{
			std::vector<std::string> all = { "1/16000", "1/12800", "1/10000" };
			for (const auto& s : standardValues(k)) { all.push_back(s); }
			return clip(all, k, 1.0 / 16000.0, 30.0);
		}
		std::vector<std::string> all = { "0.5", "0.6", "0.7", "0.8", "0.9" };
		for (const auto& s : standardFn(1.0, 32.0)) { all.push_back(s); }
		return clip(all, k, 0.5, 24.0);
	}

	expoTables standardTables(double fnMin, double fnMax)
	{
		expoTables t;
		t.iso = buildTable(standardValues(expoKind::iso), expoKind::iso);
		t.ss  = buildTable(standardValues(expoKind::ss),  expoKind::ss);
		t.fn  = buildTable(standardFn(fnMin, fnMax),       expoKind::fn);
		t.stepStops = 1.0 / 3.0;
		return t;
	}

	// 表示値の丸めで隣り合う段差は ±0.03 段ほどばらつく(1/125 は本当は 1/128)。
	//  刻みの同定と、デバイスの申告の検算に同じ許容を使う。
	namespace { constexpr double kStepTolStops = 0.06; }

	double medianStepStops(const std::vector<std::string>& values, expoKind k)
	{
		std::vector<double> r;
		r.reserve(values.size());
		for (const auto& v : values) { const double x = parseValue(v, k); if (x > 0.0) { r.push_back(x); } }
		if (r.size() < 3) { return 0.0; }	// 1点だけのF値など。測れない
		std::sort(r.begin(), r.end());
		std::vector<double> d;
		d.reserve(r.size() - 1);
		for (size_t i = 1; i < r.size(); ++i)
		{
			const double g = std::fabs(apexOf(r[i], k) - apexOf(r[i - 1], k));
			if (g > 1e-6) { d.push_back(g); }	// 同じ値が2つ並ぶ並びは段差0になるので数えない
		}
		if (d.empty()) { return 0.0; }
		std::sort(d.begin(), d.end());
		return d[d.size() / 2];
	}

	double detectStepStops(const std::vector<std::string>& values, expoKind k)
	{
		const double med = medianStepStops(values, k);
		if (!(med > 0.0)) { return 0.0; }
		const double cand[] = { 1.0 / 3.0, 0.5, 1.0 };
		for (double c : cand) { if (std::fabs(med - c) <= kStepTolStops) { return c; } }
		return med;	// 見覚えのない刻み(内蔵カメラの細かい並び等)はそのまま返す
	}

	bool stepMatchesValues(const std::vector<std::string>& values, expoKind k, double stepStops)
	{
		if (!(stepStops > 0.0)) { return false; }
		const double med = medianStepStops(values, k);
		if (!(med > 0.0)) { return true; }	// 測れない = 否定する根拠が無いので申告を信じる
		return std::fabs(med - stepStops) <= kStepTolStops;
	}

	expoTables tablesFromRange(const cmdt::shotRange& r)
	{
		expoTables t;
		const double def = (r.stepStops > 0.0) ? r.stepStops : (1.0 / 3.0);
		t.stepStops = def;
		t.isoStep = (r.isoStep > 0.0) ? r.isoStep : def;
		t.ssStep  = (r.ssStep  > 0.0) ? r.ssStep  : def;
		t.fnStep  = (r.fnStep  > 0.0) ? r.fnStep  : def;
		t.iso = buildTable(r.iso,  expoKind::iso, t.isoStep, &r.isoReal);
		t.ss  = buildTable(r.ss,   expoKind::ss,  t.ssStep,  &r.ssReal);
		t.fn  = buildTable(r.fNum, expoKind::fn,  t.fnStep,  &r.fnReal);
		return t;
	}

	namespace
	{
		// 露出値文字列の apex をテーブルから引く。無ければ実数から算出(テーブルと同じ刻みに揃える)。無効は 0。
		double apexFromTable(const std::vector<expoEntry>& tab, const std::string& v, expoKind k, double stepStops)
		{
			for (const auto& e : tab) { if (e.value == v) { return e.apex; } }
			double r = parseValue(v, k);
			return (r > 0.0) ? snapStops(apexOf(r, k), stepStops) : 0.0;
		}

	}

	double excessStops(double predicted, double linD, double linU)
	{
		if (!(predicted > 0.0) || !(linD > 0.0) || !(linU > 0.0)) { return 0.0; }
		if (predicted < linD) { return std::log2(linD / predicted); }	// 暗すぎる → 縁まで明るく(+)
		if (predicted > linU) { return std::log2(linU / predicted); }	// 明るすぎる → 縁まで暗く(−)
		return 0.0;
	}

	double brightnessStops(const hgc::exposure& e, const expoTables& t)
	{
		// Sv - Av - Tv。Sv↑=明るい、Av↑(大F)=暗い、Tv↑(短秒)=暗い。
		return apexFromTable(t.iso, e.iso, expoKind::iso, t.isoStep)
		     - apexFromTable(t.fn,  e.fn,  expoKind::fn,  t.fnStep)
		     - apexFromTable(t.ss,  e.ss,  expoKind::ss,  t.ssStep);
	}

	// --- exposureCtl(無段階。テーブルは語彙と上下限のためだけに持つ) ---

	namespace
	{
		// 実数 → その軸が明るさに与える寄与[段](大きいほど明るい)。
		//  buildTable と同じ計算(apex を理想の格子へ揃える)なので、テーブルにある値なら
		//  その要素の apex と完全に一致する。テーブルに無い値(限界など)も同じ物差しで測れる。
		double contribOfReal(double real, expoKind k, double stepStops)
		{
			if (!(real > 0.0)) { return 0.0; }
			const double a = snapStops(apexOf(real, k), stepStops);
			return (k == expoKind::iso) ? a : -a;
		}
		// テーブルの1要素の寄与[段]。
		double contribOfEntry(const expoEntry& e, expoKind k)
		{
			return (k == expoKind::iso) ? e.apex : -e.apex;
		}
		// i 番の目盛りの幅[段](隣との差のうち細かい方)。隣が無ければ 0。
		double notchAt(const std::vector<expoEntry>& e, int i)
		{
			const int n = static_cast<int>(e.size());
			if (n < 2) { return 0.0; }
			if (i < 0)  { i = 0; }
			if (i >= n) { i = n - 1; }
			double best = 0.0;
			for (int j : { i - 1, i + 1 })
			{
				if (j < 0 || j >= n) { continue; }
				// 同じ apex に落ちる重複はテーブルの作り方次第で起きうる。段差 0 は目盛りではない。
				const double d = std::fabs(e[j].apex - e[i].apex);
				if (d > 1e-6 && (best <= 0.0 || d < best)) { best = d; }
			}
			return best;
		}
	}

	void exposureCtl::init(const expoTables& tables,
	                       const hgc::exposure& limitBright,
	                       const hgc::exposure& limitDark,
	                       const hgc::exposureType priority[hgc::exposureTypeNum])
	{
		iso_.e = tables.iso; ss_.e = tables.ss; fn_.e = tables.fn;	// real昇順済み
		iso_.kind = expoKind::iso; ss_.kind = expoKind::ss; fn_.kind = expoKind::fn;
		// テーブルに無い値(限界など)を同じ物差しで測るために、軸ごとの刻みを覚える。
		iso_.step = (tables.isoStep > 0.0) ? tables.isoStep : tables.stepStops;
		ss_.step  = (tables.ssStep  > 0.0) ? tables.ssStep  : tables.stepStops;
		fn_.step  = (tables.fnStep  > 0.0) ? tables.fnStep  : tables.stepStops;

		// 限界の実数(空=0=限界なし)
		limBIso_ = parseValue(limitBright.iso, expoKind::iso); if (limBIso_ < 0) { limBIso_ = 0; }
		limDIso_ = parseValue(limitDark.iso,   expoKind::iso); if (limDIso_ < 0) { limDIso_ = 0; }
		limBSs_  = parseValue(limitBright.ss,  expoKind::ss);  if (limBSs_  < 0) { limBSs_  = 0; }
		limDSs_  = parseValue(limitDark.ss,    expoKind::ss);  if (limDSs_  < 0) { limDSs_  = 0; }
		limBFn_  = parseValue(limitBright.fn,  expoKind::fn);  if (limBFn_  < 0) { limBFn_  = 0; }
		limDFn_  = parseValue(limitDark.fn,    expoKind::fn);  if (limDFn_  < 0) { limDFn_  = 0; }
		ssCap_   = 0.0;

		for (int i = 0; i < hgc::exposureTypeNum; ++i) { priority_[i] = priority[i]; }
		this->recalcRanges();
		// 出発点はテーブルの先頭(iso/ss は最小、fn は最小F=いちばん明るい)。従来 idx=0 と同じ。
		//  呼び出し側はこの直後に setCurrent / setToBrightLimit / setToDarkLimit で置き換える。
		iso_.b = iso_.e.empty() ? 0.0 : contribOfEntry(iso_.e.front(), expoKind::iso);
		ss_.b  = ss_.e.empty()  ? 0.0 : contribOfEntry(ss_.e.front(),  expoKind::ss);
		fn_.b  = fn_.e.empty()  ? 0.0 : contribOfEntry(fn_.e.front(),  expoKind::fn);
		this->rebuildCurrent();
	}

	// テーブルの端と撮影制御方法の限界から、各軸の動ける範囲を引き直す。
	//  tLo/tHi = テーブルの端。**デバイスに存在しない設定は作らない**ので、ここは必ず守る。
	//  bLo/bHi = それを限界で更に締めたもの。限界は「外から内へ」は通すので、動かすときだけ見る。
	void exposureCtl::recalcRanges()
	{
		// 最長 ss(撮影周期や夜間 ss)は明側の限界を更に締める。どちらか厳しい方。
		double limBSs = limBSs_;
		if (ssCap_ > 0.0 && (limBSs == 0.0 || ssCap_ < limBSs)) { limBSs = ssCap_; }
		const double limB[3] = { limBIso_, limBSs,  limBFn_ };
		const double limD[3] = { limDIso_, limDSs_, limDFn_ };
		ladder* ax[3] = { &iso_, &ss_, &fn_ };
		for (int i = 0; i < 3; ++i)
		{
			ladder& L = *ax[i];
			if (L.e.empty()) { L.tLo = L.tHi = L.bLo = L.bHi = 0.0; continue; }
			double lo = 1e300, hi = -1e300;
			for (const auto& e : L.e)
			{
				const double b = contribOfEntry(e, L.kind);
				if (b < lo) { lo = b; }
				if (b > hi) { hi = b; }
			}
			L.tLo = lo; L.tHi = hi;
			if (limB[i] > 0.0)
			{
				const double b = contribOfReal(limB[i], L.kind, L.step);	// 明側=上限
				if (b < hi) { hi = b; }
			}
			if (limD[i] > 0.0)
			{
				const double b = contribOfReal(limD[i], L.kind, L.step);	// 暗側=下限
				if (b > lo) { lo = b; }
			}
			if (lo > hi) { lo = hi; }	// 限界が食い違っていたら動かない軸にする
			L.bLo = lo; L.bHi = hi;
		}
	}

	exposureCtl::ladder& exposureCtl::axisRef(hgc::exposureType t)
	{
		switch (t)
		{
		case hgc::exposureType::ss: return ss_;
		case hgc::exposureType::fn: return fn_;
		default:                    return iso_;
		}
	}

	// b を「いちばん近い設定できる値」へ丸めて cur_ を作る。丸めた結果は内部(b)へは戻さない。
	//  限界の内側にいるうちは**内側の目盛りだけ**から選ぶ(丸めで限界を踏み越えないように。
	//  最長 ss の上限を半目盛り越えると撮影周期が崩れる)。限界の外にいるとき
	//  (窓の境目で前の窓の露出を引き継いだ直後)は、いちばん近い目盛りをそのまま使う。
	void exposureCtl::rebuildCurrent()
	{
		ladder* ax[3] = { &iso_, &ss_, &fn_ };
		std::string* out[3] = { &cur_.iso, &cur_.ss, &cur_.fn };
		for (int a = 0; a < 3; ++a)
		{
			ladder& L = *ax[a];
			if (L.e.empty()) { continue; }
			const bool inside = (L.b >= L.bLo - 1e-9 && L.b <= L.bHi + 1e-9);
			int    best = -1;
			double bd   = 1e300;
			for (int i = 0; i < static_cast<int>(L.e.size()); ++i)
			{
				const double b = contribOfEntry(L.e[i], L.kind);
				if (inside && (b < L.bLo - 1e-9 || b > L.bHi + 1e-9)) { continue; }
				const double d = std::fabs(b - L.b);
				if (d < bd) { bd = d; best = i; }
			}
			if (best < 0)
			{	// 限界の内側に目盛りが1つも無い(限界が目盛りの隙間に落ちた)。近い方を選ぶ。
				bd = 1e300;
				for (int i = 0; i < static_cast<int>(L.e.size()); ++i)
				{
					const double d = std::fabs(contribOfEntry(L.e[i], L.kind) - L.b);
					if (d < bd) { bd = d; best = i; }
				}
			}
			L.idx = best;
			*out[a] = L.e[best].value;
		}
	}

	double exposureCtl::brightness() const
	{
		return iso_.b + ss_.b + fn_.b;
	}

	// いま踏める最小の段差[段]。宣言のところに理由を書いてある。
	double exposureCtl::minStepStops() const
	{
		const ladder* ax[3] = { &iso_, &ss_, &fn_ };
		double best = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			const double d = notchAt(ax[i]->e, ax[i]->idx);
			if (d > 1e-6 && (best <= 0.0 || d < best)) { best = d; }
		}
		return (best > 0.0) ? best : (1.0 / 3.0);
	}

	// いちばん粗い目盛り[段]。動く余地のある軸だけを見る(宣言のところに理由)。
	double exposureCtl::maxStepStops() const
	{
		const ladder* ax[3] = { &iso_, &ss_, &fn_ };
		double best = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			if (!(ax[i]->bHi - ax[i]->bLo > 1e-9)) { continue; }	// 動けない軸は丸めの誤差を生まない
			const double d = notchAt(ax[i]->e, ax[i]->idx);
			if (d > best) { best = d; }
		}
		return (best > 0.0) ? best : this->minStepStops();
	}

	void exposureCtl::setCurrent(const hgc::exposure& e)
	{
		ladder* ax[3] = { &iso_, &ss_, &fn_ };
		const std::string* v[3] = { &e.iso, &e.ss, &e.fn };
		for (int a = 0; a < 3; ++a)
		{
			ladder& L = *ax[a];
			if (L.e.empty()) { continue; }
			const double r = parseValue(*v[a], L.kind);
			if (!(r > 0.0)) { continue; }
			double b = contribOfReal(r, L.kind, L.step);
			if (b < L.tLo) { b = L.tLo; }	// テーブルの外 = そのデバイスには無い設定
			if (b > L.tHi) { b = L.tHi; }
			L.b = b;
		}
		this->rebuildCurrent();
	}

	void exposureCtl::setToBrightLimit()
	{
		iso_.b = iso_.bHi; ss_.b = ss_.bHi; fn_.b = fn_.bHi;	// 明側 = 各軸の上限
		this->rebuildCurrent();
	}

	void exposureCtl::setToDarkLimit()
	{
		iso_.b = iso_.bLo; ss_.b = ss_.bLo; fn_.b = fn_.bLo;	// 暗側 = 各軸の下限
		this->rebuildCurrent();
	}

	void exposureCtl::capLongestSs(double maxSsSec)
	{
		if (maxSsSec <= 0.0) { return; }
		if (ssCap_ == 0.0 || maxSsSec < ssCap_) { ssCap_ = maxSsSec; }	// 締めるだけ。緩めない
		this->recalcRanges();
		if (ss_.b > ss_.bHi) { ss_.b = ss_.bHi; }	// 現在 ss が上限を超えていれば引き下げる
		this->rebuildCurrent();
	}

	// 1軸を delta 段動かす。上下限は越えない。
	//  いま限界の外にいるなら(窓の境目の引き継ぎ)、**内へ戻る向きだけ**動ける。
	//  外から更に外へは動かない = 従来の「移動先が限界の外なら動かない」と同じ考え方。
	double exposureCtl::moveAxis(ladder& L, double delta)
	{
		if (L.e.empty() || !(std::fabs(delta) > 0.0)) { return 0.0; }
		const double lo = (L.b < L.bLo) ? L.b : L.bLo;
		const double hi = (L.b > L.bHi) ? L.b : L.bHi;
		double want = L.b + delta;
		if (want < lo) { want = lo; }
		if (want > hi) { want = hi; }
		const double did = want - L.b;
		L.b = want;
		return did;
	}

	double exposureCtl::moveStops(double evStops, const hgc::exposure* home)
	{
		if (!(std::fabs(evStops) > 1e-12)) { return 0.0; }
		const bool bright = (evStops > 0.0);
		double     remain = evStops;

		// ① home(基準)へ戻す軸を優先度の逆順で先に(§4.5 往復対称)。home は通り越さない。
		if (home != nullptr)
		{
			const std::string* hv[3] = { &home->iso, &home->ss, &home->fn };
			ladder*            ax[3] = { &iso_, &ss_, &fn_ };
			double             hb[3] = { 0.0, 0.0, 0.0 };
			bool               hok[3] = { false, false, false };
			for (int a = 0; a < 3; ++a)
			{
				const double r = parseValue(*hv[a], ax[a]->kind);
				if (r > 0.0) { hb[a] = contribOfReal(r, ax[a]->kind, ax[a]->step); hok[a] = true; }
			}
			for (int k = hgc::exposureTypeNum - 1; k >= 0; --k)
			{
				if (std::fabs(remain) <= 1e-12) { break; }
				int a = 0;
				switch (priority_[k])
				{
				case hgc::exposureType::ss: a = 1; break;
				case hgc::exposureType::fn: a = 2; break;
				default:                    a = 0; break;
				}
				if (!hok[a]) { continue; }
				const double diff = hb[a] - ax[a]->b;	// home までの差(この軸が戻るべき量)
				// その向きに home が無い軸は、いま戻す相手ではない(②で普通に配る)。
				if (bright ? (diff <= 1e-9) : (diff >= -1e-9)) { continue; }
				const double take = bright ? ((remain < diff) ? remain : diff)
				                           : ((remain > diff) ? remain : diff);
				remain -= this->moveAxis(*ax[a], take);
			}
		}

		// ② 残りを通常の優先度順で配る(上位の軸から限界まで使う)。
		for (int k = 0; k < hgc::exposureTypeNum; ++k)
		{
			if (std::fabs(remain) <= 1e-12) { break; }
			remain -= this->moveAxis(this->axisRef(priority_[k]), remain);
		}
		this->rebuildCurrent();
		return evStops - remain;
	}

	// 軸を指名して1目盛りだけ動かす。移動先が限界の外なら動かない(全部動くか、動かないか)。
	//  無段階の位置(b)からきっかり目盛りぶん動かすので、丸めの端数は持ったまま運ばれる。
	//  配分寄せは「明るい向きへ1目盛り・暗い向きへ1目盛り」を対にして呼ぶので、
	//  端数を捨てないことで**明るさが動かない**という性質が保たれる。
	bool exposureCtl::stepAxis(hgc::exposureType axis, bool bright)
	{
		ladder& L = this->axisRef(axis);
		const double n = notchAt(L.e, L.idx);
		if (!(n > 0.0)) { return false; }
		const double lo   = (L.b < L.bLo) ? L.b : L.bLo;
		const double hi   = (L.b > L.bHi) ? L.b : L.bHi;
		const double want = L.b + (bright ? n : -n);
		if (want < lo - 1e-9 || want > hi + 1e-9) { return false; }
		L.b = want;
		this->rebuildCurrent();
		return true;
	}

	bool exposureCtl::stepOne(bool bright)
	{
		for (int k = 0; k < hgc::exposureTypeNum; ++k)
		{
			if (this->stepAxis(priority_[k], bright)) { return true; }
		}
		return false;
	}

	bool exposureCtl::brighten() { return stepOne(true); }
	bool exposureCtl::darken()   { return stepOne(false); }

	hgc::exposure exposureCtl::applyStops(double evStops)
	{
		this->moveStops(evStops, nullptr);
		return cur_;
	}

	// 窓の境目の配分寄せ(宣言のコメント参照)。
	bool migrateToward(exposureCtl& ctl, exposureCtl& want,
	                   const expoTables& tables, const hgc::exposure& initial)
	{
		// いまの明るさに対して、この撮影制御方法なら選ぶ組み合わせ(=寄せ先)。
		//  基準(initial)から出発し、同じ明るさへ優先度・限界に従って寄せる。境目で一気に
		//  やっていた計算そのもの。違うのは、結果へ飛ばずに1目盛りずつ近づける点だけ。
		want.setCurrent(initial);
		const double nowB = brightnessStops(ctl.current(), tables);
		want.applyStops(nowB - brightnessStops(want.current(), tables));
		const hgc::exposure dest = want.current();
		const hgc::exposure cur  = ctl.current();

		// 軸ごとに「明るくしたい/暗くしたい/そのまま」を出す。iso/ss は実数が大きいほど
		//  明るく、fn は小さいほど明るい。
		auto cmp = [](double a, double b) -> int { return (a > b + 1e-9) ? 1 : ((a < b - 1e-9) ? -1 : 0); };
		const int wantBright[3] = {
			 cmp(parseValue(dest.iso, expoKind::iso), parseValue(cur.iso, expoKind::iso)),
			 cmp(parseValue(dest.ss,  expoKind::ss ), parseValue(cur.ss,  expoKind::ss )),
			-cmp(parseValue(dest.fn,  expoKind::fn ), parseValue(cur.fn,  expoKind::fn )) };
		const hgc::exposureType axis[3] = { hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };

		// 明るい向きへ動かす軸を1つ、暗い向きへ動かす軸を1つ、同時に1目盛りずつ。
		//  こうすると明るさは動かない(打ち消し合う)ので、自動露出の1歩と同じコマに乗せられる。
		//  片側しか動けないなら見送る(明るさがずれ、自動露出の枠を食うため)。
		//  寄せ先は限界の内側なので本来どの組でも通るが、表の端で弾かれても止まらないよう
		//  組は総当たりする。
		for (int u = 0; u < 3; ++u)
		{
			if (wantBright[u] <= 0) { continue; }
			if (!ctl.stepAxis(axis[u], true)) { continue; }
			for (int d = 0; d < 3; ++d)
			{
				if (d == u || wantBright[d] >= 0) { continue; }
				if (ctl.stepAxis(axis[d], false)) { return true; }
			}
			ctl.stepAxis(axis[u], false);	// 相手が見つからなかったので明るさを戻す
		}
		return false;
	}

}
