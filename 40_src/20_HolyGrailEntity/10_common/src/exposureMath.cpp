#include "exposureMath.h"
#include "apiBase.h"	// デバイスに段で聞く(expoAxes/expoResolve/expoStops)
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

	// ヒストグラム中央値(0.0～1.0)。仕様 4.3.1。受け皿の幅は問わない。
	template <typename T>
	static double histMedianOf(const T* lumBins, int nBins)
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

	double histMedian(const uint16_t* lumBins, int nBins) { return histMedianOf(lumBins, nBins); }
	double histMedian(const uint32_t* lumBins, int nBins) { return histMedianOf(lumBins, nBins); }

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
			// 【1 秒未満を全部「1/整数」にしない(2026-09-19)】分母を整数にすると、
			//  1/10〜1/3 秒のあたりで 1/12 段の点がいくつも同じ綴りになり(1/3 が 6 連続)、
			//  目盛りが潰れる。編集画面では「スライダーを動かしても値が変わらない」として現れる。
			//  デバイス層(apiBuiltin::ssText)は既に同じ規則へ直してあるので、こちらも揃える。
			//   ・1/50 秒より速い側 … 分母を整数にした分数(カメラの表記に合わせる)
			//   ・それ以外          … 秒を有効数字 4 桁(0.3251 / 1.059 / 19.65 / 48)
			if (v < 0.02)
			{
				const int denom = static_cast<int>(1.0 / v + 0.5);
				std::snprintf(b, sizeof(b), "1/%d", denom);
			}
			else if (std::fabs(v - std::floor(v + 0.5)) < 0.005)
			{	// ほぼ整数秒は整数で。"48" が "47.99" と出ないように。
				//  【20 秒以上を一律に整数へ丸めない(2026-09-19)】1/12 段は 20〜48 秒では
				//   1.2〜2.9 秒あるので、整数へ丸めると 21→23 のように刻みが崩れる(0.13 段)。
				std::snprintf(b, sizeof(b), "%.0f", v);
			}
			else { std::snprintf(b, sizeof(b), "%.4g", v); }
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

	std::vector<std::string> rangeValues(expoKind k, double stepStops, double loReal, double hiReal)
	{
		if (!(loReal > 0.0) || !(hiReal > 0.0)) { return {}; }
		if (hiReal < loReal) { std::swap(loReal, hiReal); }
		double step = (stepStops > 0.0) ? stepStops : (1.0 / 3.0);
		if (step < 1.0 / 24.0) { step = 1.0 / 24.0; }	// これより細かいと綴りが重複して潰れる
		if (step > 1.0)        { step = 1.0; }
		// 【起点はデバイスの下端(2026-09-19)】ISO 100 や 1 秒を起点にすると、端末が答える
		//  下端(例 ISO 44)から刻んだ並びと噛み合わず、計画が持つ値が目盛りから外れる。
		//  外れた値は編集画面を開いた瞬間に最寄りへ吸着して保存されるので、**黙って値が動く**。
		//  下端を起点にすれば、その端末で作った値はそのまま目盛りに乗る。
		//  F は 1 段が比 √2 なので指数を半分にする。
		const double perStep = (k == expoKind::fn) ? (step / 2.0) : step;
		std::vector<std::string> out;
		for (int n = 0; n < 4096; ++n)
		{
			const double v = loReal * std::pow(2.0, perStep * n);
			if (v > hiReal * 1.001) { break; }
			const std::string t = gridText(k, v);
			if (out.empty() || out.back() != t) { out.push_back(t); }
		}
		const std::string last = gridText(k, hiReal);
		if (out.empty() || out.back() != last) { out.push_back(last); }
		return out;
	}

	std::vector<std::string> pickFromValues(const std::vector<std::string>& values,
	                                        expoKind k, double stepStops)
	{
		struct one { double real; double b; std::string v; };
		std::vector<one> all;
		for (const auto& s : values)
		{
			const double r = parseValue(s, k);
			if (!(r > 0.0)) { continue; }	// Bulb / auto / 壊れた綴りは除く
			all.push_back({ r, stopsOfReal(r, k), s });
		}
		if (all.size() < 2) { return {}; }
		std::sort(all.begin(), all.end(), [](const one& a, const one& b) { return a.real < b.real; });

		double step = (stepStops > 0.0) ? stepStops : (1.0 / 3.0);
		if (step > 1.0) { step = 1.0; }
		std::vector<std::string> out;
		out.push_back(all.front().v);
		double last = all.front().b;
		for (size_t i = 1; i + 1 < all.size(); ++i)
		{
			// 刻みぶん離れたものだけ採る。カメラの刻みより細かい指定では全部通る。
			if (std::fabs(all[i].b - last) >= step - 1e-6)
			{
				out.push_back(all[i].v);
				last = all[i].b;
			}
		}
		if (all.back().v != out.back()) { out.push_back(all.back().v); }	// 上端は必ず残す
		return out;
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

	double stopsOfReal(double real, expoKind k)
	{
		if (!(real > 0.0)) { return 0.0; }
		switch (k)
		{
		case expoKind::iso: return  svFromIso(real);	// 大きいほど明るい
		case expoKind::ss:  return -tvFromSs(real);		// log2(秒)
		default:            return -avFromFn(real);		// -log2(F^2)
		}
	}

	double realOfStops(double stops, expoKind k)
	{
		switch (k)
		{
		case expoKind::iso: return 100.0 * std::pow(2.0, stops);
		case expoKind::ss:  return std::pow(2.0, stops);
		default:            return std::pow(2.0, -stops / 2.0);	// F = 2^(-b/2)
		}
	}

	double excessStops(double predicted, double linD, double linU)
	{
		if (!(predicted > 0.0) || !(linD > 0.0) || !(linU > 0.0)) { return 0.0; }
		if (predicted < linD) { return std::log2(linD / predicted); }	// 暗すぎる → 縁まで明るく(+)
		if (predicted > linU) { return std::log2(linU / predicted); }	// 明るすぎる → 縁まで暗く(−)
		return 0.0;
	}

	double shapeVelocity(double& vel, double need, double vmax, double accel, double horizonFrames)
	{
		const double th = (horizonFrames > 1.0) ? horizonFrames : 1.0;
		double tv = need / th;
		if (tv >  vmax) { tv =  vmax; }
		if (tv < -vmax) { tv = -vmax; }
		double dv = tv - vel;
		if (dv >  accel) { dv =  accel; }
		if (dv < -accel) { dv = -accel; }
		vel += dv;
		if (std::fabs(vel) < 1e-12) { vel = 0.0; }
		return vel;
	}

	convergeStep initialConvergeStep(double errStops, double medianX,
	                                 double tolStops, double satMedian, double satStepStops)
	{
		convergeStep r;
		r.saturated = (medianX >= satMedian);
		// 飽和しているときの errStops は過小評価。収束と認めない。
		if (!r.saturated && std::fabs(errStops) <= tolStops) { r.converged = true; return r; }
		// 目標へ直接投影する(無段階なので誤差ぶんきっかり)。飽和中は最低 satStepStops 段は暗く。
		r.delta = -errStops;
		if (r.saturated && r.delta > -satStepStops) { r.delta = -satStepStops; }
		return r;
	}

	double brightnessStops(const hgc::exposure& e, const expoTables& t)
	{
		// Sv - Av - Tv。Sv↑=明るい、Av↑(大F)=暗い、Tv↑(短秒)=暗い。
		return apexFromTable(t.iso, e.iso, expoKind::iso, t.isoStep)
		     - apexFromTable(t.fn,  e.fn,  expoKind::fn,  t.fnStep)
		     - apexFromTable(t.ss,  e.ss,  expoKind::ss,  t.ssStep);
	}

	// --- exposureCtl(無段。テーブルを持たず、デバイスに段で聞く) ---

	bool exposureCtl::init(apiBase* dev,
	                       const hgc::exposure& limitBright,
	                       const hgc::exposure& limitDark,
	                       const hgc::exposureType priority[hgc::exposureTypeNum])
	{
		dev_ = dev;
		iso_ = axis{}; ss_ = axis{}; fn_ = axis{};
		iso_.kind = expoKind::iso; ss_.kind = expoKind::ss; fn_.kind = expoKind::fn;
		ssCap_ = 0.0;
		gotSum_ = 0.0;
		cur_ = hgc::exposure{};
		for (int i = 0; i < hgc::exposureTypeNum; ++i) { priority_[i] = priority[i]; }
		if (dev_ == nullptr) { return false; }

		// 限界を段で測る。軸が空なら「限界なし」。
		expoPoint pb{}, pd{};
		dev_->expoStops(limitBright, pb);
		dev_->expoStops(limitDark,   pd);
		iso_.limB = pb.iso; iso_.hasLimB = pb.hasIso; iso_.limD = pd.iso; iso_.hasLimD = pd.hasIso;
		ss_.limB  = pb.ss;  ss_.hasLimB  = pb.hasSs;  ss_.limD  = pd.ss;  ss_.hasLimD  = pd.hasSs;
		fn_.limB  = pb.fn;  fn_.hasLimB  = pb.hasFn;  fn_.limD  = pd.fn;  fn_.hasLimD  = pd.hasFn;

		axisInfo ai, as, af;
		if (dev_->expoAxes(ai, as, af) != ERR_HGC_OK) { return false; }
		iso_.tLo = ai.lo; iso_.tHi = ai.hi; iso_.notch = ai.notch;
		ss_.tLo  = as.lo; ss_.tHi  = as.hi; ss_.notch  = as.notch;
		fn_.tLo  = af.lo; fn_.tHi  = af.hi; fn_.notch  = af.notch;
		this->recalcRanges();
		// 出発点はデバイスの下端(呼び出し側はこの直後に setCurrent / setToXxxLimit で置き換える)。
		iso_.b = iso_.tLo; ss_.b = ss_.tLo; fn_.b = fn_.tHi;	// fn は明るい側=小さいF
		this->rebuildCurrent();
		return true;
	}

	// デバイスへ範囲と丸めの粗さを聞き直す。粗さは位置で変わるので毎コマ聞いてよい
	//  (デバイス層の中の計算だけで、カメラ通信は起きない約束)。
	void exposureCtl::refreshAxes()
	{
		if (dev_ == nullptr) { return; }
		axisInfo ai, as, af;
		if (dev_->expoAxes(ai, as, af) != ERR_HGC_OK) { return; }
		iso_.tLo = ai.lo; iso_.tHi = ai.hi; iso_.notch = ai.notch;
		ss_.tLo  = as.lo; ss_.tHi  = as.hi; ss_.notch  = as.notch;
		fn_.tLo  = af.lo; fn_.tHi  = af.hi; fn_.notch  = af.notch;
		this->recalcRanges();
	}

	// デバイスの範囲を撮影制御方法の限界で締める。
	void exposureCtl::recalcRanges()
	{
		axis* ax[3] = { &iso_, &ss_, &fn_ };
		for (int i = 0; i < 3; ++i)
		{
			axis& a = *ax[i];
			double lo = a.tLo, hi = a.tHi;
			if (a.hasLimB && a.limB < hi) { hi = a.limB; }	// 明側=上限
			if (a.hasLimD && a.limD > lo) { lo = a.limD; }	// 暗側=下限
			if (lo > hi) { lo = hi; }	// 限界が食い違っていたら動かない軸にする
			a.bLo = lo; a.bHi = hi;
		}
		// 最長 ss(撮影周期や夜間 ss)は明側を更に締める。
		if (ssCap_ > 0.0)
		{
			const double cap = stopsOfReal(ssCap_, expoKind::ss);
			if (cap < ss_.bHi) { ss_.bHi = cap; }
			if (ss_.bLo > ss_.bHi) { ss_.bLo = ss_.bHi; }
		}
	}

	// b をデバイスが出せる値へ。丸めるのはここだけで、丸めた結果は b へは戻さない。
	void exposureCtl::rebuildCurrent()
	{
		if (dev_ == nullptr) { return; }
		expoPoint want;
		want.iso = iso_.b; want.hasIso = true;
		want.ss  = ss_.b;  want.hasSs  = true;
		want.fn  = fn_.b;  want.hasFn  = true;
		expoPoint got;
		if (dev_->expoResolve(want, cur_, got) != ERR_HGC_OK) { return; }
		gotSum_ = got.sum();
	}

	double exposureCtl::brightness() const { return iso_.b + ss_.b + fn_.b; }

	expoPoint exposureCtl::point() const
	{
		expoPoint p;
		p.iso = iso_.b; p.hasIso = true;
		p.ss  = ss_.b;  p.hasSs  = true;
		p.fn  = fn_.b;  p.hasFn  = true;
		return p;
	}

	double exposureCtl::minStepStops() const
	{
		const axis* ax[3] = { &iso_, &ss_, &fn_ };
		double best = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			const double d = ax[i]->notch;
			if (d > 1e-9 && (best <= 0.0 || d < best)) { best = d; }
		}
		return best;	// 0 = どの軸も無段
	}

	double exposureCtl::maxStepStops() const
	{
		const axis* ax[3] = { &iso_, &ss_, &fn_ };
		double best = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			if (!(ax[i]->bHi - ax[i]->bLo > 1e-9)) { continue; }	// 動けない軸は丸めの誤差を生まない
			if (ax[i]->notch > best) { best = ax[i]->notch; }
		}
		return best;
	}

	void exposureCtl::setCurrent(const hgc::exposure& e)
	{
		if (dev_ == nullptr) { return; }
		expoPoint p;
		if (dev_->expoStops(e, p) != ERR_HGC_OK) { return; }
		// デバイスが出せる範囲の外へは出さない(そこに設定は存在しない)。限界では止めない。
		auto set = [](axis& a, double v, bool has)
		{
			if (!has) { return; }
			if (v < a.tLo) { v = a.tLo; }
			if (v > a.tHi) { v = a.tHi; }
			a.b = v;
		};
		set(iso_, p.iso, p.hasIso);
		set(ss_,  p.ss,  p.hasSs);
		set(fn_,  p.fn,  p.hasFn);
		this->rebuildCurrent();
	}

	void exposureCtl::setToBrightLimit()
	{
		iso_.b = iso_.bHi; ss_.b = ss_.bHi; fn_.b = fn_.bHi;
		this->rebuildCurrent();
	}

	void exposureCtl::setToDarkLimit()
	{
		iso_.b = iso_.bLo; ss_.b = ss_.bLo; fn_.b = fn_.bLo;
		this->rebuildCurrent();
	}

	void exposureCtl::capLongestSs(double maxSsSec)
	{
		if (maxSsSec <= 0.0) { return; }
		if (ssCap_ == 0.0 || maxSsSec < ssCap_) { ssCap_ = maxSsSec; }	// 締めるだけ。緩めない
		this->recalcRanges();
		if (ss_.b > ss_.bHi) { ss_.b = ss_.bHi; }
		this->rebuildCurrent();
	}

	// 1軸を delta 段動かす。上下限は越えない。
	//  いま限界の外にいるなら(窓の境目の引き継ぎ)、**内へ戻る向きだけ**動ける。
	double exposureCtl::moveAxis(axis& a, double delta)
	{
		if (!(std::fabs(delta) > 0.0)) { return 0.0; }
		const double lo = (a.b < a.bLo) ? a.b : a.bLo;
		const double hi = (a.b > a.bHi) ? a.b : a.bHi;
		double want = a.b + delta;
		if (want < lo) { want = lo; }
		if (want > hi) { want = hi; }
		const double did = want - a.b;
		a.b = want;
		return did;
	}

	exposureCtl::axis& exposureCtl::axisRef(hgc::exposureType t)
	{
		switch (t)
		{
		case hgc::exposureType::ss: return ss_;
		case hgc::exposureType::fn: return fn_;
		default:                    return iso_;
		}
	}

	double exposureCtl::moveStops(double evStops, const hgc::exposure* home)
	{
		if (dev_ == nullptr) { return 0.0; }
		this->refreshAxes();	// 位置で粗さと範囲が変わる。動く前に聞き直す
		if (!(std::fabs(evStops) > 1e-12)) { return 0.0; }
		const bool bright = (evStops > 0.0);
		double     remain = evStops;

		// ① home(基準)へ戻す軸を優先度の逆順で先に(§4.5 往復対称)。home は通り越さない。
		if (home != nullptr)
		{
			expoPoint hp;
			dev_->expoStops(*home, hp);
			const double hb[3]  = { hp.iso, hp.ss, hp.fn };
			const bool   hok[3] = { hp.hasIso, hp.hasSs, hp.hasFn };
			axis*        ax[3]  = { &iso_, &ss_, &fn_ };
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
				const double diff = hb[a] - ax[a]->b;	// home までの差
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

	// 軸を指名して動かす。目盛りがあればその1目盛り、無ければ amountStops。
	//  移動先が限界の外なら動かない(全部動くか、動かないか)。
	bool exposureCtl::stepAxis(hgc::exposureType axisType, bool bright, double amountStops)
	{
		axis& a = this->axisRef(axisType);
		const double n = (a.notch > 1e-9) ? a.notch : std::fabs(amountStops);
		if (!(n > 1e-9)) { return false; }
		const double lo   = (a.b < a.bLo) ? a.b : a.bLo;
		const double hi   = (a.b > a.bHi) ? a.b : a.bHi;
		const double want = a.b + (bright ? n : -n);
		if (want < lo - 1e-9 || want > hi + 1e-9) { return false; }
		a.b = want;
		this->rebuildCurrent();
		return true;
	}

	bool exposureCtl::stepOne(bool bright, double amountStops)
	{
		for (int k = 0; k < hgc::exposureTypeNum; ++k)
		{
			if (this->stepAxis(priority_[k], bright, amountStops)) { return true; }
		}
		return false;
	}

	bool exposureCtl::brighten(double amountStops) { return stepOne(true,  amountStops); }
	bool exposureCtl::darken(double amountStops)   { return stepOne(false, amountStops); }

	hgc::exposure exposureCtl::applyStops(double evStops)
	{
		this->moveStops(evStops, nullptr);
		return cur_;
	}

	// 窓の境目の配分寄せ(宣言のコメント参照)。
	bool migrateToward(exposureCtl& ctl, exposureCtl& want,
	                   const hgc::exposure& initial, double amountStops)
	{
		// いまの明るさに対して、この撮影制御方法なら選ぶ組み合わせ(=寄せ先)。
		//  基準(initial)から出発し、同じ明るさへ優先度・限界に従って寄せる。境目で一気に
		//  やっていた計算そのもの。違うのは、結果へ飛ばずに1目盛りずつ近づける点だけ。
		want.setCurrent(initial);
		want.applyStops(ctl.appliedBrightness() - want.appliedBrightness());

		// 軸ごとに「明るくしたい/暗くしたい/そのまま」を段で出す。
		//  【文字列を解釈しない(2026-09-19)】値の綴りはデバイスの語彙なので、
		//   共通部分は段だけで比べる。
		const expoPoint d = want.point();
		const expoPoint c = ctl.point();
		auto cmp = [](double a, double b, bool ha, bool hb) -> int
		{
			if (!ha || !hb) { return 0; }
			return (a > b + 1e-9) ? 1 : ((a < b - 1e-9) ? -1 : 0);
		};
		const int wantBright[3] = {
			cmp(d.iso, c.iso, d.hasIso, c.hasIso),
			cmp(d.ss,  c.ss,  d.hasSs,  c.hasSs ),
			cmp(d.fn,  c.fn,  d.hasFn,  c.hasFn ) };
		const hgc::exposureType axis[3] = { hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };

		// 明るい向きへ動かす軸を1つ、暗い向きへ動かす軸を1つ、同時に1目盛りずつ。
		//  こうすると明るさは動かない(打ち消し合う)ので、自動露出の1歩と同じコマに乗せられる。
		//  片側しか動けないなら見送る(明るさがずれ、自動露出の枠を食うため)。
		//  無段の軸には目盛りが無いので amountStops を使う。
		for (int u = 0; u < 3; ++u)
		{
			if (wantBright[u] <= 0) { continue; }
			if (!ctl.stepAxis(axis[u], true, amountStops)) { continue; }
			for (int e = 0; e < 3; ++e)
			{
				if (e == u || wantBright[e] >= 0) { continue; }
				if (ctl.stepAxis(axis[e], false, amountStops)) { return true; }
			}
			ctl.stepAxis(axis[u], false, amountStops);	// 相手が見つからなかったので明るさを戻す
		}
		return false;
	}

}
