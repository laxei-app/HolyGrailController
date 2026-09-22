#include "apiBuiltin.h"
#include "builtinBridge.h"
#include "device.h"
#include "exposureMath.h"
#include "jpegLuma.h"
#include "dataManager.h"
#include "notice.h"	// 開けない理由をお知らせ番号で上へ返す(文言は UI)
#include "osFile.h"
#include "tool.h"
#include <ctime>
#include <json/nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

const char* apiBuiltin::kSerialPrefix = "BUILTIN:";

namespace
{
	// 露光時間[秒] の下限・上限。端末が答えない項目のよりどころ。
	//  1/8000 は一般的な最速、30秒は Camera2 でよくある最長。**当てにはしない**
	//  (端末が範囲を答えたらそちらを使う)。
	constexpr double kFallbackSsMin = 1.0 / 8000.0;
	constexpr double kFallbackSsMax = 30.0;
	constexpr int    kFallbackIsoMin = 50;
	constexpr int    kFallbackIsoMax = 3200;
	constexpr double kFallbackFn     = 1.8;
}

// ── 値の文字列 ──────────────────────────────────────────────
// 上位(テーブル/計画/ログ)が使う形にする。expo::parseValue が読み戻せる書き方であること。
std::string apiBuiltin::ssText(double sec)
{
	char b[32];
	// 【読み戻して同じ升目に落ちる精度で書く(2026-09-07)】1/12 段の升目は幅 5.9%。文字列の誤差は
	//  その半分(2.9%)より十分小さくする。
	//  ・1/50 秒より速い側は分母を整数にした分数(誤差 ≤ 1%。カメラの表記に合わせる)
	//  ・それ以外は秒を有効数字 3 桁(誤差 ≤ 0.5%。0.0212 / 0.333 / 1.06 / 30.8 / 48)
	//  以前は 1 秒未満をすべて「1/整数」にしていて、1/8〜1/2 秒で升目が潰れ(1/3→1/2 は 0.59 段)、
	//  夕方の露出がのこぎり波になった。
	//  【無段にしたので桁を増やした(2026-09-19)】丸めるのは設定の瞬間だけになり、
	//   文字列の誤差がそのまま制御の誤差になる。有効数字 4 桁で ±0.0007 段。
	if (sec < 0.02)
	{
		const int denom = static_cast<int>(1.0 / sec + 0.5);
		std::snprintf(b, sizeof(b), "1/%d", denom);
	}
	else
	{
		std::snprintf(b, sizeof(b), "%.4g", sec);
	}
	return std::string(b);
}

double apiBuiltin::realOf(const std::vector<std::string>& list, const std::vector<double>& reals,
                          const std::string& v, expo::expoKind k)
{
	if (reals.size() == list.size())
	{
		for (size_t i = 0; i < list.size(); ++i) { if (list[i] == v) { return reals[i]; } }
	}
	return expo::parseValue(v, k);
}

std::string apiBuiltin::isoText(int iso)
{
	char b[16]; std::snprintf(b, sizeof(b), "%d", iso); return std::string(b);
}

std::string apiBuiltin::fnText(double fn)
{
	//  【桁を増やした(2026-09-19)】可変絞り機では 0.1 以内に 2 点並びうる。
	//   1 桁だと同じ綴りになって片方が消える(f/1.85 と f/1.9 など)。
	char b[16]; std::snprintf(b, sizeof(b), "%.2f", fn);
	std::string s = b;
	while (s.size() > 3 && s.back() == '0') { s.pop_back(); }	// "2.00"->"2.0" / "1.80"->"1.8"
	return s;
}

// ── テーブルの合成 ──────────────────────────────────────────
// 【なぜ合成するか】内蔵カメラは連続に設定できるが、上位の露出制御は「目盛りのテーブル」で
//  動く。連続値を扱えるようモデルを広げると、限界の判定・ログ・解析・機材マスタ・エッジまで
//  波及して代償が大きい。目盛りの側を細かくして乗せる方が安く、精度も足りる(2026-09-05 判断)。
//
// 【重複を捨てる】1/12 段刻みで整数へ丸めると、値が小さいところで同じ数字が続く
//  (ISO 50→53→56 …は良いが、1/8000 付近の ss は分母が同じになる)。同じ文字列は1つにする。
//  同じ値が並ぶと exposureCtl の1目盛りが 0 段になり、制御が空回りするため。
void apiBuiltin::buildTables(void)
{
	ssList_.clear(); isoList_.clear(); fnList_.clear();
	ssReal_.clear(); isoReal_.clear(); fnReal_.clear();

	// ここで作るのは「記録と画面表示」用の両端だけ。制御は段でやり取りするので並びは要らない。
	//  同じ綴りが続いたら後を捨てる(並びと論理値の長さを揃える)。
	auto push = [](std::vector<std::string>& texts, std::vector<double>& reals,
	               const std::string& t, double r)
	{
		if (!texts.empty() && texts.back() == t) { return; }
		texts.push_back(t); reals.push_back(r);
	};

	// --- ss(両端だけ。この端末は無段である。2026-09-19) ---
	//  以前は 1/12 段の等比で並びを合成していたが、それは**この端末の性能を落としていた**。
	//  露出制御は apiBase::expoAxes/expoResolve で段のまま扱うので、並びは要らない。
	//  ここで作る両端は「機材の記録と画面表示(min/max しか見ていない)」のためだけ。
	//  上端はセンサーの上限ではなく「加算込みで設定できる上限」(RAW が出せれば 48 秒)。
	{
		double lo = (expMinNs_ > 0) ? (static_cast<double>(expMinNs_) / 1e9) : kFallbackSsMin;
		double hi = this->maxSettableSsSec();
		if (hi <= 0.0) { hi = kFallbackSsMax; }
		if (hi < lo) { std::swap(lo, hi); }
		push(ssList_, ssReal_, ssText(lo), lo);
		push(ssList_, ssReal_, ssText(hi), hi);
	}

	// --- iso(両端だけ。整数だが刻みは 1/12 段よりはるかに細かいので無段として扱う) ---
	{
		int lo = (isoMin_ > 0) ? isoMin_ : kFallbackIsoMin;
		int hi = (isoMax_ > 0) ? isoMax_ : kFallbackIsoMax;
		if (hi < lo) { std::swap(lo, hi); }
		push(isoList_, isoReal_, isoText(lo), static_cast<double>(lo));
		push(isoList_, isoReal_, isoText(hi), static_cast<double>(hi));
	}

	// --- F値。多くの端末は固定(1点)。可変ならそのまま並べる ---
	if (apertures_.empty()) { push(fnList_, fnReal_, fnText(kFallbackFn), kFallbackFn); }
	else
	{
		for (double a : apertures_) { push(fnList_, fnReal_, fnText(a), a); }
	}

	// 【テーブルは持たない(2026-09-19)】測光値の割り戻しは expoStops で行う。
	//  並びに載っていない値(無段で決めた ss)も文字列から論理値へ読み戻せる。
}

// ── 素性 ────────────────────────────────────────────────────
errCode apiBuiltin::init(class device& device)
{
	// device.urlAccess には detectBuiltin が "論理/物理" を入れてある(IP の代わり)。
	//  配下を持たない端末では両方同じ id になる。
	{
		const std::string& u = device.urlAccess;
		const size_t sl = u.find('/');
		logicalId_ = (sl == std::string::npos) ? u : u.substr(0, sl);
		id_        = (sl == std::string::npos) ? u : u.substr(sl + 1);
	}
	if (id_.empty()) { return ERR_HGC_INVALID_ARG; }

	nlohmann::json j = nlohmann::json::parse(builtinCam::describeJson(id_), nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return ERR_HGC_NOT_FOUND; }

	sensorW_ = j.value("sensorW", 0.0);
	sensorH_ = j.value("sensorH", 0.0);
	pixelW_  = static_cast<uint32_t>(j.value("pixelW", 0));
	pixelH_  = static_cast<uint32_t>(j.value("pixelH", 0));
	isoMin_  = j.value("isoMin", 0);
	isoMax_  = j.value("isoMax", 0);
	expMinNs_ = j.value("expMinNs", static_cast<long long>(0));
	expMaxNs_ = j.value("expMaxNs", static_cast<long long>(0));
	manual_  = j.value("manual", false);
	focalMm_ = j.value("focalMm", 0.0);
	nrOff_     = j.value("nrOff", false);
	nrMinimal_ = j.value("nrMinimal", false);
	edgeOff_   = j.value("edgeOff", false);
	rawOk_     = j.value("raw", false);
	focusMinDpt_  = j.value("focusMinDiopter", 0.0);	// 0=固定焦点。>0 ならピント位置を指定できる
	hyperfocalDpt_ = j.value("hyperfocalDiopter", 0.0);
	name_    = j.value("name", std::string("Built-in camera"));
	apertures_.clear();
	if (j.contains("apertures") && j["apertures"].is_array())
	{
		for (const auto& a : j["apertures"]) { if (a.is_number()) { apertures_.push_back(a.get<double>()); } }
	}
	this->buildTables();

	// 【一度だけ諸元と目盛りの数を残す】端末ごとに範囲が違うので、後から確かめられるようにする。
	//  合成した並びが妥当か(刻みが細かすぎて重複していないか)はここを見れば分かる。
	{
		char b[256];
		std::snprintf(b, sizeof(b),
		              "builtin %s: sensor %.2fx%.2fmm %ux%u iso %d-%d ss %.6f-%.3fs manual=%d "
		              "/ steps iso=%zu ss=%zu fn=%zu",
		              id_.c_str(), sensorW_, sensorH_, pixelW_, pixelH_, isoMin_, isoMax_,
		              (expMinNs_ > 0 ? expMinNs_ / 1e9 : 0.0), (expMaxNs_ > 0 ? expMaxNs_ / 1e9 : 0.0),
		              manual_ ? 1 : 0, isoList_.size(), ssList_.size(), fnList_.size());
		dataManager::logEvent("CAMERA", b);
		std::snprintf(b, sizeof(b), "builtin %s: nrOff=%d nrMinimal=%d edgeOff=%d raw=%d focus=%s",
		              id_.c_str(), nrOff_ ? 1 : 0, nrMinimal_ ? 1 : 0, edgeOff_ ? 1 : 0, rawOk_ ? 1 : 0,
		              (focusMinDpt_ > 0.0) ? "infinity(set)" : "fixed");
		dataManager::logEvent("CAMERA", b);
	}

	device.model        = name_;
	device.manufacturer = "builtin";
	device.assignedName = name_;
	device.serialno     = std::string(kSerialPrefix) + id_;
	return ERR_HGC_OK;
}

// ── 加算で作る長秒露光 ──────────────────────────────────────
double apiBuiltin::maxSettableSsSec(void) const
{
	const double hw = this->maxSsSec();
	if (!rawOk_) { return hw; }
	return (hw > kMaxStackSsSec) ? hw : kMaxStackSsSec;
}

int apiBuiltin::stackFrames(double sec) const
{
	const double hw = this->maxSsSec();
	if (!rawOk_ || hw <= 0.0) { return 1; }
	const double sub = hw * kSubExposureRatio;	// 1 コマの上限(ヘッダの説明を参照)
	if (sec <= sub + 1e-6) { return 1; }
	return static_cast<int>(std::ceil(sec / sub - 1e-9));
}

errCode apiBuiltin::readDeviceStatus(deviceStatus& out)
{
	const int t = builtinCam::thermalStatus();
	if (t < 0) { return ERR_HGC_NOT_SUPPORTED; }
	// PowerManager の段階: 0=NONE 1=LIGHT 2=MODERATE 3=SEVERE 4=CRITICAL 5=EMERGENCY 6=SHUTDOWN。
	//  SEVERE から「高温警告」、EMERGENCY から「撮影できない」と扱う(端末が落とす直前)。
	out.tempValid   = true;
	out.tempWarning = (t >= 3);
	out.tempStopped = (t >= 5);
	if (t != lastThermal_)
	{
		lastThermal_ = t;
		char b[64];
		std::snprintf(b, sizeof(b), "builtin thermal status %d", t);
		dataManager::logEvent("CAMERA", b, t >= 3);
	}
	return ERR_HGC_OK;
}

// カメラを開く。開けなかった理由が「この端末のカメラを使う許可が無い」なら、それを覚えて
//  上位が名指しで案内できるようにする(2026-09-09 ユーザー指示)。
//
// 【なぜ理由を分けるか】諸元(画角・ISO範囲・センサー寸法)は許可が無くても読めるので、
//  カメラは一覧に出てくるし計画も作れる。開こうとして初めて断られるが、上位には
//  「開けなかった」しか伝わらず、画面には「カメラが見つかりません。オンラインにしてください」
//  と出ていた。目の前のカメラなので探し直しても永久に直らず、利用者は気づけない
//  (新しい端末 Pixel 8 Pro で実際に起きた)。
errCode apiBuiltin::openCamera(void)
{
	const std::string e = builtinCam::open(logicalId_, id_, rawOk_);
	if (e.empty()) { failNotice_ = 0; opened_ = true; return ERR_HGC_OK; }
	failNotice_ = builtinCam::hasPermission() ? 0 : static_cast<int>(hgc::notice::cameraNoPermission);
	dataManager::logEvent("CAMERA", ("builtin open failed: " + e).c_str(), true);
	return ERR_HGC_NOT_FOUND;
}

errCode apiBuiltin::startShooting(void)
{
	return this->openCamera();
}

errCode apiBuiltin::getSettings(cmdt::shotRange& settings)
{
	if (ssList_.empty()) { this->buildTables(); }
	settings.ss   = ssList_;
	settings.iso  = isoList_;
	settings.fNum = fnList_;
	settings.ssReal    = ssReal_;
	settings.isoReal   = isoReal_;
	settings.fnReal    = fnReal_;
	// 【刻みはもう答えない(2026-09-19)】露出制御は expoAxes/expoResolve で段のまま扱う。
	//  ここで返す並びは「機材の記録と画面表示」のためだけ(両端しか入っていない)。
	//  ログはキヤノン層と同じ形で、実態(無段かどうか)を出す。
	{
		axisInfo ai, as, af;
		this->expoAxes(ai, as, af);
		//  notch>0=その刻み / 動けない軸=fixed / それ以外=stepless(無段)
		auto txt = [](char* b, size_t n, const axisInfo& a)
		{
			if (a.notch > 0.0)             { std::snprintf(b, n, "%.3f", a.notch); }
			else if (!(a.hi - a.lo > 1e-9)) { std::snprintf(b, n, "fixed"); }
			else                           { std::snprintf(b, n, "stepless"); }
		};
		char bi[16], bs[16], bf[16], msg[192];
		txt(bi, sizeof(bi), ai); txt(bs, sizeof(bs), as); txt(bf, sizeof(bf), af);
		std::snprintf(msg, sizeof(msg), "exposure step: iso=%s(device) ss=%s(device) fn=%s(device)", bi, bs, bf);
		dataManager::logEvent("CAMERA", msg);
	}
	return ERR_HGC_OK;
}

// ── 露出を「段」で扱う口 ─────────────────────────────────────
// 【この端末は ss と ISO が無段(2026-09-19 ユーザー決定)】
//  SENSOR_EXPOSURE_TIME は ns の整数、SENSOR_SENSITIVITY は整数で、どちらも
//  刻みは 1/12 段よりはるかに細かい(ISO 44 でも整数 1 段差は 0.032 段)。
//  以前は 1/12 段の並びを合成して制御側に渡していたが、それはこの端末の性能を
//  わざわざ落としていた。範囲だけを答え、丸めない(notch=0)。
//  F 値だけは端末が答える並び(多くは 1 点、可変絞り機は複数)のぶんだけ離散。
double apiBuiltin::apertureMin(void) const
{
	if (apertures_.empty()) { return 0.0; }
	double m = apertures_.front();
	for (double a : apertures_) { if (a > 0.0 && (m <= 0.0 || a < m)) { m = a; } }
	return m;
}

double apiBuiltin::apertureMax(void) const
{
	if (apertures_.empty()) { return 0.0; }
	double m = apertures_.front();
	for (double a : apertures_) { if (a > m) { m = a; } }
	return m;
}

errCode apiBuiltin::expoAxes(axisInfo& iso, axisInfo& ss, axisInfo& fn)
{
	if (ssList_.empty()) { this->buildTables(); }
	iso = axisInfo{}; ss = axisInfo{}; fn = axisInfo{};

	// ss: センサーの最短 〜 設定できる最長(加算込み)。無段。
	{
		double lo = (expMinNs_ > 0) ? (static_cast<double>(expMinNs_) / 1e9) : kFallbackSsMin;
		double hi = this->maxSettableSsSec();
		if (hi <= 0.0) { hi = kFallbackSsMax; }
		if (hi < lo) { std::swap(lo, hi); }
		ss.lo = expo::stopsOfReal(lo, expo::expoKind::ss);
		ss.hi = expo::stopsOfReal(hi, expo::expoKind::ss);
		ss.notch = 0.0;	// 無段
	}
	// ISO: 端末の範囲。整数だが刻みは細かいので無段として扱う(丸めは設定の瞬間だけ)。
	{
		int lo = (isoMin_ > 0) ? isoMin_ : kFallbackIsoMin;
		int hi = (isoMax_ > 0) ? isoMax_ : kFallbackIsoMax;
		if (hi < lo) { std::swap(lo, hi); }
		iso.lo = expo::stopsOfReal(static_cast<double>(lo), expo::expoKind::iso);
		iso.hi = expo::stopsOfReal(static_cast<double>(hi), expo::expoKind::iso);
		iso.notch = 0.0;
	}
	// F: 端末が答える並び。1 点なら動かない軸(lo==hi・notch=0)。
	{
		const double amin = this->apertureMin();
		const double amax = this->apertureMax();
		const double f0 = (amin > 0.0) ? amin : kFallbackFn;
		const double f1 = (amax > 0.0) ? amax : f0;
		fn.hi = expo::stopsOfReal(f0, expo::expoKind::fn);	// 小さいFほど明るい=上端
		fn.lo = expo::stopsOfReal(f1, expo::expoKind::fn);
		fn.notch = 0.0;
		if (apertures_.size() > 1)
		{	// 隣り合う絞りの差のうちいちばん細かいもの。丸めの粗さになる。
			std::vector<double> b;
			for (double a : apertures_) { if (a > 0.0) { b.push_back(expo::stopsOfReal(a, expo::expoKind::fn)); } }
			std::sort(b.begin(), b.end());
			double best = 0.0;
			for (size_t i = 1; i < b.size(); ++i)
			{
				const double d = b[i] - b[i - 1];
				if (d > 1e-6 && (best <= 0.0 || d < best)) { best = d; }
			}
			fn.notch = best;
		}
	}
	return ERR_HGC_OK;
}

errCode apiBuiltin::expoResolve(const expoPoint& want, hgc::exposure& out, expoPoint& got)
{
	axisInfo ai, as, af;
	const errCode e = this->expoAxes(ai, as, af);
	if (e != ERR_HGC_OK) { return e; }
	out = hgc::exposure{};
	got = expoPoint{};
	auto clamp = [](double v, double lo, double hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); };

	// ss: 望まれた段をそのまま秒にして書き出す。丸めない。
	if (want.hasSs)
	{
		const double b   = clamp(want.ss, as.lo, as.hi);
		const double sec = expo::realOfStops(b, expo::expoKind::ss);
		out.ss = ssText(sec);
		// 文字列へ書いた結果を読み戻した値が「実際に設定される値」。段もそこから答える。
		const double back = expo::parseValue(out.ss, expo::expoKind::ss);
		got.ss = expo::stopsOfReal((back > 0.0) ? back : sec, expo::expoKind::ss);
		got.hasSs = true;
	}
	// ISO: 整数へ丸める(端末が受け取れるのは整数)。範囲で止める。
	if (want.hasIso)
	{
		const double b = clamp(want.iso, ai.lo, ai.hi);
		double v = expo::realOfStops(b, expo::expoKind::iso);
		int    n = static_cast<int>(v + 0.5);
		const int lo = (isoMin_ > 0) ? isoMin_ : kFallbackIsoMin;
		const int hi = (isoMax_ > 0) ? isoMax_ : kFallbackIsoMax;
		if (n < lo) { n = lo; }
		if (n > hi) { n = hi; }
		out.iso = isoText(n);
		got.iso = expo::stopsOfReal(static_cast<double>(n), expo::expoKind::iso);
		got.hasIso = true;
	}
	// F: 端末が答える並びのうち、いちばん近いもの。
	if (want.hasFn)
	{
		double bestA = (this->apertureMin() > 0.0) ? this->apertureMin() : kFallbackFn;
		if (!apertures_.empty())
		{
			double bd = 1e300;
			for (double a : apertures_)
			{
				if (!(a > 0.0)) { continue; }
				const double d = std::fabs(expo::stopsOfReal(a, expo::expoKind::fn) - want.fn);
				if (d < bd) { bd = d; bestA = a; }
			}
		}
		out.fn = fnText(bestA);
		got.fn = expo::stopsOfReal(bestA, expo::expoKind::fn);
		got.hasFn = true;
	}
	return ERR_HGC_OK;
}

// 露出の明るさ[段]。テーブルを持たないので expoStops から作る。
double apiBuiltin::brightnessOf(const hgc::exposure& e)
{
	expoPoint p;
	if (this->expoStops(e, p) != ERR_HGC_OK) { return 0.0; }
	return p.sum();
}

errCode apiBuiltin::expoStops(const hgc::exposure& e, expoPoint& out)
{
	out = expoPoint{};
	// 文字列はこの層が書いたもの(または計画が持つ値)。論理値へ読み戻して段にする。
	//  テーブルに載っている値なら realOf が正確な論理値を返す。無ければ文字列を読む。
	const double si = realOf(isoList_, isoReal_, e.iso, expo::expoKind::iso);
	const double ss = realOf(ssList_,  ssReal_,  e.ss,  expo::expoKind::ss);
	const double sf = realOf(fnList_,  fnReal_,  e.fn,  expo::expoKind::fn);
	if (!e.iso.empty() && si > 0.0) { out.iso = expo::stopsOfReal(si, expo::expoKind::iso); out.hasIso = true; }
	if (!e.ss.empty()  && ss > 0.0) { out.ss  = expo::stopsOfReal(ss, expo::expoKind::ss);  out.hasSs  = true; }
	if (!e.fn.empty()  && sf > 0.0) { out.fn  = expo::stopsOfReal(sf, expo::expoKind::fn);  out.hasFn  = true; }
	return ERR_HGC_OK;
}

errCode apiBuiltin::readSensorSpec(double& sensorWmm, double& sensorHmm, uint32_t& pixelW, uint32_t& pixelH)
{
	if (sensorW_ <= 0.0 || pixelW_ == 0) { return ERR_HGC_NOT_SUPPORTED; }
	sensorWmm = sensorW_; sensorHmm = sensorH_; pixelW = pixelW_; pixelH = pixelH_;
	return ERR_HGC_OK;
}

// ── 露出を載せる ────────────────────────────────────────────
// 内蔵カメラは要求ごとに露出を渡すので、ここでは覚えるだけ。カメラへは撮る瞬間に渡る。
//  「設定した値が本当に載ったか」を別途確かめる必要が無いので、CCAPI のような
//  リトライ・遅延適用の手当ては要らない。
errCode apiBuiltin::setFNumber(const std::string& fNumber) { curFn_  = fNumber; return ERR_HGC_OK; }
errCode apiBuiltin::setSS(const std::string& ss)           { curSs_  = ss;      return ERR_HGC_OK; }
errCode apiBuiltin::setIso(const std::string& iso)         { curIso_ = iso;     return ERR_HGC_OK; }

errCode apiBuiltin::rdyShutter(const cmdt::shotSet& shotSet)
{
	if (!shotSet.ss.empty())   { curSs_  = shotSet.ss; }
	if (!shotSet.iso.empty())  { curIso_ = shotSet.iso; }
	if (!shotSet.fNum.empty()) { curFn_  = shotSet.fNum; }
	return ERR_HGC_OK;
}

errCode apiBuiltin::setupShootingModeManual(void)
{
	{
		const errCode oe = this->openCamera();
		if (oe != ERR_HGC_OK) { return oe; }
	}
	// 【動画をここで開く(2026-09-05)】撮影の区切りと動画の区切りを一致させる。
	//  出来上がりは Movies/TwyLapse/<計画名>_yyyymmddhhmmss.mp4。10分ごとに「そこまでの完成品」が
	//  置き換わっていく(BuiltinVideo)。名前と置き場は Kotlin 側が決める。
	{
		const std::string name = builtinCam::videoStart(30, sessionLabel_);
		if (name.empty()) { dataManager::logEvent("CAMERA", "builtin video: cannot start", true); }
		else              { dataManager::logEvent("CAMERA", ("builtin video: " + name).c_str()); }
	}
	if (!manual_)
	{
		// 露出を指定できない端末では、撮れはするが露出制御が成立しない。黙って進めない。
		dataManager::logEvent("CAMERA", "builtin camera has no manual sensor control", true);
	}
	return ERR_HGC_OK;
}

errCode apiBuiltin::restoreShootingMode(void)
{
	// 【必ず閉じる】MP4 は最後に閉じないと再生できない。撮影の終わりはここを通る。
	const std::string made = builtinCam::videoFinish();
	if (!made.empty())
	{
		dataManager::logEvent("CAMERA", ("builtin video done: " + made).c_str());
	}
	if (opened_) { builtinCam::close(); opened_ = false; }
	return ERR_HGC_OK;
}

// ── 撮る ────────────────────────────────────────────────────
double apiBuiltin::curSsSec(void) const
{
	const double sec = realOf(ssList_, ssReal_, curSs_, expo::expoKind::ss);	// 論理値(文字列は鍵)
	return (sec > 0.0) ? sec : 1.0;
}

bool apiBuiltin::shootStart(void)
{
	const double sec = this->curSsSec();
	const double iso = realOf(isoList_, isoReal_, curIso_, expo::expoKind::iso);
	const double fn  = realOf(fnList_,  fnReal_,  curFn_,  expo::expoKind::fn);
	// 【センサーの上限を超える ss は分けて撮って足す(2026-09-06)】24 秒なら 8 秒×3 コマ。
	//  1コマの長さは等分にする(最後だけ短い、より読み出しの隙間が揃う)。
	const int    frames = this->stackFrames(sec);
	const double sub    = sec / frames;
	const long long ns  = static_cast<long long>(sub * 1e9 + 0.5);
	// 加算の内訳をファイルのログにも残す(コマ数が変わったときだけ。毎コマ言わない)。
	//  logcat の TLP-RAW には毎回出るが、PC を外して撮ると残らないため。
	if (frames != lastFrames_)
	{
		lastFrames_ = frames;
		char b[96];
		std::snprintf(b, sizeof(b), "builtin stack: ss %.1fs = %d x %.2fs", sec, frames, sub);
		dataManager::logEvent("CAMERA", b);
	}
	return builtinCam::capture(logicalId_, id_, (iso > 0.0) ? static_cast<int>(iso + 0.5) : 0,
	                           ns, (fn > 0.0) ? fn : 0.0, 0, frames, rawOk_);
}

int apiBuiltin::takeBudgetMs(void) const
{
	// 露光 + 現像と転送の余裕(8 秒) + 撮り直し 2 コマぶん(ヘッダの説明を参照)。
	const double sec = this->curSsSec();
	return static_cast<int>(sec * 1000.0) + 8000 + 2 * static_cast<int>(sec / this->stackFrames(sec) * 1000.0);
}

bool apiBuiltin::shootTake(std::vector<uint8_t>& out)
{
	return builtinCam::takeImage(this->takeBudgetMs(), out);
}

// 【シャッターは待たずに戻る(2026-09-05 実機で判明)】
//  露光の終わりまで待つ作りにしたら、6秒露光の1コマが 11.9秒かかり、呼び出し側の予算
//  (8秒)を超えて毎コマ失敗した。キヤノンの CCAPI も「シャッターのPOSTは露光を待たずに
//  戻る」ので、そちらへ合わせる。撮れた画像は測光(meterScene)で受け取る。
//  露出制御はもともと露光が終わってから測るので、待つ場所としてそちらが正しい。
errCode apiBuiltin::actShutter(void)
{
	// 前のコマの画像がまだ残っていれば、ここで回収して残す。露光はとっくに終わっているので待たない。
	//  夜間のような固定露出の窓では測光が呼ばれないため、回収の口をここにも置かないと
	//  受け取り口が詰まる(2026-09-05 実機で1枚も保存されずに判明)。
	this->collectPending();
	lastJpeg_.clear();	// 前のコマの画像を次の測光へ使い回さない
	if (!this->shootStart()) { return ERR_HGC_TAKE_FAIL; }
	return ERR_HGC_OK;
}

void apiBuiltin::collectPending(void)
{
	if (!lastJpeg_.empty()) { return; }	// 測光が既に受け取っている
	std::vector<uint8_t> jpeg;
	const int to = this->takeBudgetMs();
	const bool got = this->shootTake(jpeg);
	// 【1コマごとに経過を残す(2026-09-07 調査用)】失敗したコマがどこで止まったか(結果が来ない/
	//  画像が来ない/HAL が落とした/現像に失敗)をファイルのログで追えるようにする。
	//  周期は 30 秒以上なので 1 行/コマでも量は知れている。原因が分かったら失敗時だけに戻す。
	{
		char b[640];
		std::snprintf(b, sizeof(b), "builtin frame %s (wait<=%dms) %s",
		              got ? "ok" : "LOST", to, builtinCam::captureReport().c_str());
		dataManager::logEvent("CAMERA", b, !got);
	}
	// フレームを続けて失ったらカメラを開き直す(HAL の状態を一度捨てる)。次の要求(capture)が開き直す。
	if (got) { lostStreak_ = 0; }
	else if (++lostStreak_ >= kMaxLostFrames)
	{
		lostStreak_ = 0;
		dataManager::logEvent("CAMERA", "builtin: frames lost in a row, reopening camera", true);
		builtinCam::close(); opened_ = false;
	}
	if (got)
	{
		// 【狙ったセンサーで撮れたか(2026-09-05)】端末が勝手に切り替えると露出制御の土台が
		//  崩れる。推測できないので申告を見る。違っていたら一度だけ残す(毎コマ言わない)。
		const std::string act = builtinCam::activePhysicalId();
		if (!act.empty() && act != id_ && !physWarned_)
		{
			physWarned_ = true;
			dataManager::logEvent("CAMERA",
				("builtin: wanted physical " + id_ + " but got " + act).c_str(), true);
		}
		this->saveShot(jpeg);
		// 受け取ったその場で動画へ1コマ足す。周期が15秒以上あるので符号化は間に合う。
		builtinCam::videoAddJpeg(jpeg);
		lastJpeg_.swap(jpeg);
	}
}

// ── 測る ────────────────────────────────────────────────────
// JPEG から輝度の中央値を出し、リニア輝度に直す。手順はキヤノン機のサムネイル測光と同じで、
//  違うのは「材料がその場にある」ことだけ(待ちも取得も要らない)。
bool apiBuiltin::measure(const std::vector<uint8_t>& jpeg, meterResult& out) const
{
	if (jpeg.empty()) { out.failStage = 1; return false; }
	uint32_t hist[cmdt::hist_bin] = {0};
	int w = 0, h = 0;
	void* t0 = tool::startElapse();
	// 内蔵カメラの JPEG は素の画角そのままで、レターボックスの黒帯が無い。切り落とさない。
	if (!jpglm::lumaHistogram(jpeg.data(), jpeg.size(), hist, w, h, 0.0))
	{
		out.failStage = 4; return false;
	}
	out.decodeMs = static_cast<int>(tool::getElapse(t0));
	out.x      = expo::histMedian(hist, cmdt::hist_bin);
	out.linear = expo::srgbToLinear(out.x);
	out.via    = meterResult::via_shotThumb;
	out.ok     = true;
	out.usable = true;
	return true;
}

// 撮った画像を残す。連番で書くだけの素朴な作り。
//  置き場所はログや計画と同じアプリの領域(osfile の下)。撮影のたびに増えるので、
//  動画の書き出しが入ったらここは既定で切る。
void apiBuiltin::saveShot(const std::vector<uint8_t>& jpeg)
{
	if (jpeg.empty()) { return; }
	const std::string dir = osfile::dir("shot");
	if (dir.empty()) { return; }
	char name[64];
	std::snprintf(name, sizeof(name), "/tlp_%05d.jpg", ++shotSeq_);	// 改名の取りこぼし(2026-09-23)
	if (!osfile::writeAll(dir + name, reinterpret_cast<const char*>(jpeg.data()), jpeg.size()))
	{
		// 書けないときは残量不足が疑わしい。毎コマ言っても仕方ないので最初の1回だけ。
		if (shotSeq_ == 1) { dataManager::logEvent("CAMERA", "builtin: cannot save shot", true); }
	}
}

errCode apiBuiltin::meterScene(const hgc::exposure& shotExp, meterResult& out,
                               const std::function<bool()>& keepGoing)
{
	(void)keepGoing;
	// 露光が終わって画像が出てくるのをここで待つ(シャッターは待たずに戻っている)。
	//  受け取った時点で残す。測光と保存で同じ1枚を使う(撮り直さない)。
	this->collectPending();
	if (!this->measure(lastJpeg_, out))
	{
		// 直前のコマが撮れていない。上位はこれが続いたら「カメラがオンラインでない」と見る。
		out.ok = false; out.shotMissing = lastJpeg_.empty();
		return ERR_HGC_RDY_METARING;
	}
	out.meterExp = shotExp;	// 測ったのは撮影画像そのもの=撮影露出で測った
	out.sceneRef = out.linear / std::pow(2.0, this->brightnessOf(shotExp));
	return ERR_HGC_OK;
}

errCode apiBuiltin::meterHere(meterResult& out, const std::function<bool()>& keepGoing)
{
	(void)keepGoing;
	if (builtinCam::open(logicalId_, id_, rawOk_).empty()) { opened_ = true; }
	std::vector<uint8_t> jpeg;
	if (!this->shootStart() || !this->shootTake(jpeg))
	{ out.ok = false; out.failStage = 20; return ERR_HGC_RDY_METARING; }
	if (!this->measure(jpeg, out)) { out.ok = false; return ERR_HGC_RDY_METARING; }
	// いま載せている露出で撮ったので、それが測光露出そのもの。
	hgc::exposure me; me.iso = curIso_; me.ss = curSs_; me.fn = curFn_;
	out.meterExp = me;
	out.sceneRef = out.linear / std::pow(2.0, this->brightnessOf(me));
	lastJpeg_ = jpeg;	// 続けて meterScene が呼ばれても材料が揃っている
	return ERR_HGC_OK;
}
