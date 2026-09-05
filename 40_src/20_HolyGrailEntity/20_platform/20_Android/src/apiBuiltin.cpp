#include "apiBuiltin.h"
#include "builtinBridge.h"
#include "device.h"
#include "exposureMath.h"
#include "jpegLuma.h"
#include "dataManager.h"
#include "tool.h"
#include <json/nlohmann/json.hpp>
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
	if (sec >= 1.0)
	{
		// 1秒以上は小数1桁まで。きりのよい値は整数で書く(ログが読みやすい)。
		if (std::fabs(sec - std::floor(sec + 0.5)) < 0.05) { std::snprintf(b, sizeof(b), "%d", static_cast<int>(sec + 0.5)); }
		else                                               { std::snprintf(b, sizeof(b), "%.1f", sec); }
	}
	else
	{
		// 1秒未満は分数。分母は整数へ丸める(カメラの表記に合わせる)。
		const double denom = 1.0 / sec;
		std::snprintf(b, sizeof(b), "1/%d", static_cast<int>(denom + 0.5));
	}
	return std::string(b);
}

std::string apiBuiltin::isoText(int iso)
{
	char b[16]; std::snprintf(b, sizeof(b), "%d", iso); return std::string(b);
}

std::string apiBuiltin::fnText(double fn)
{
	char b[16]; std::snprintf(b, sizeof(b), "%.1f", fn); return std::string(b);
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

	// --- ss(短い=暗い側 から 長い=明るい側 へ。apex 昇順は上位が作る) ---
	{
		double lo = (expMinNs_ > 0) ? (static_cast<double>(expMinNs_) / 1e9) : kFallbackSsMin;
		double hi = (expMaxNs_ > 0) ? (static_cast<double>(expMaxNs_) / 1e9) : kFallbackSsMax;
		if (hi < lo) { std::swap(lo, hi); }
		const double span = std::log2(hi / lo);
		const int    n    = static_cast<int>(span / kStepStops + 0.5);
		std::string prev;
		for (int k = 0; k <= n; ++k)
		{
			const double t = lo * std::pow(2.0, kStepStops * k);
			const std::string s = ssText(t > hi ? hi : t);
			if (s != prev) { ssList_.push_back(s); prev = s; }
		}
		const std::string top = ssText(hi);
		if (ssList_.empty() || ssList_.back() != top) { ssList_.push_back(top); }
	}

	// --- iso ---
	{
		int lo = (isoMin_ > 0) ? isoMin_ : kFallbackIsoMin;
		int hi = (isoMax_ > 0) ? isoMax_ : kFallbackIsoMax;
		if (hi < lo) { std::swap(lo, hi); }
		const double span = std::log2(static_cast<double>(hi) / lo);
		const int    n    = static_cast<int>(span / kStepStops + 0.5);
		std::string prev;
		for (int k = 0; k <= n; ++k)
		{
			int v = static_cast<int>(lo * std::pow(2.0, kStepStops * k) + 0.5);
			if (v > hi) { v = hi; }
			const std::string s = isoText(v);
			if (s != prev) { isoList_.push_back(s); prev = s; }
		}
		const std::string top = isoText(hi);
		if (isoList_.empty() || isoList_.back() != top) { isoList_.push_back(top); }
	}

	// --- F値。多くの端末は固定(1点)。可変ならそのまま並べる ---
	if (apertures_.empty()) { fnList_.push_back(fnText(kFallbackFn)); }
	else
	{
		std::string prev;
		for (double a : apertures_)
		{
			const std::string s = fnText(a);
			if (s != prev) { fnList_.push_back(s); prev = s; }
		}
	}

	// 測光値の割り戻しに使う APEX テーブル。上位が作るものと同じ手順で作る。
	tables_.iso = expo::buildTable(isoList_, expo::expoKind::iso);
	tables_.ss  = expo::buildTable(ssList_,  expo::expoKind::ss);
	tables_.fn  = expo::buildTable(fnList_,  expo::expoKind::fn);
}

// ── 素性 ────────────────────────────────────────────────────
errCode apiBuiltin::init(class device& device)
{
	// device.urlAccess には detectBuiltin がカメラ id を入れてある(IP の代わり)。
	id_ = device.urlAccess;
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
	}

	device.model        = name_;
	device.manufacturer = "builtin";
	device.assignedName = name_;
	device.serialno     = std::string(kSerialPrefix) + id_;
	return ERR_HGC_OK;
}

errCode apiBuiltin::startShooting(void)
{
	const std::string e = builtinCam::open(id_);
	if (!e.empty())
	{
		dataManager::logEvent("CAMERA", ("builtin open failed: " + e).c_str(), true);
		return ERR_HGC_NOT_FOUND;
	}
	opened_ = true;
	return ERR_HGC_OK;
}

errCode apiBuiltin::getSettings(cmdt::shotRange& settings)
{
	if (ssList_.empty()) { this->buildTables(); }
	settings.ss   = ssList_;
	settings.iso  = isoList_;
	settings.fNum = fnList_;
	return ERR_HGC_OK;
}

errCode apiBuiltin::readSensorSpec(double& sensorWmm, double& sensorHmm, uint32_t& pixelW)
{
	if (sensorW_ <= 0.0 || pixelW_ == 0) { return ERR_HGC_NOT_SUPPORTED; }
	sensorWmm = sensorW_; sensorHmm = sensorH_; pixelW = pixelW_;
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
	const std::string e = builtinCam::open(id_);
	if (!e.empty())
	{
		dataManager::logEvent("CAMERA", ("builtin open failed: " + e).c_str(), true);
		return ERR_HGC_NOT_FOUND;
	}
	opened_ = true;
	if (!manual_)
	{
		// 露出を指定できない端末では、撮れはするが露出制御が成立しない。黙って進めない。
		dataManager::logEvent("CAMERA", "builtin camera has no manual sensor control", true);
	}
	return ERR_HGC_OK;
}

errCode apiBuiltin::restoreShootingMode(void)
{
	if (opened_) { builtinCam::close(); opened_ = false; }
	return ERR_HGC_OK;
}

// ── 撮る ────────────────────────────────────────────────────
double apiBuiltin::curSsSec(void) const
{
	const double sec = expo::parseValue(curSs_, expo::expoKind::ss);
	return (sec > 0.0) ? sec : 1.0;
}

bool apiBuiltin::shootStart(void)
{
	const double sec = this->curSsSec();
	const double iso = expo::parseValue(curIso_, expo::expoKind::iso);
	const double fn  = expo::parseValue(curFn_,  expo::expoKind::fn);
	const long long ns = static_cast<long long>(sec * 1e9 + 0.5);
	return builtinCam::capture(id_, (iso > 0.0) ? static_cast<int>(iso + 0.5) : 0,
	                           ns, (fn > 0.0) ? fn : 0.0, 0);
}

bool apiBuiltin::shootTake(std::vector<uint8_t>& out)
{
	// 露光 + 現像と転送の余裕。長秒でも取りこぼさない長さにする。
	const int to = static_cast<int>(this->curSsSec() * 1000.0) + 8000;
	return builtinCam::takeImage(to, out);
}

// 【シャッターは待たずに戻る(2026-09-05 実機で判明)】
//  露光の終わりまで待つ作りにしたら、6秒露光の1コマが 11.9秒かかり、呼び出し側の予算
//  (8秒)を超えて毎コマ失敗した。キヤノンの CCAPI も「シャッターのPOSTは露光を待たずに
//  戻る」ので、そちらへ合わせる。撮れた画像は測光(meterScene)で受け取る。
//  露出制御はもともと露光が終わってから測るので、待つ場所としてそちらが正しい。
errCode apiBuiltin::actShutter(void)
{
	lastJpeg_.clear();	// 前のコマの画像を次の測光へ使い回さない
	if (!this->shootStart()) { return ERR_HGC_TAKE_FAIL; }
	return ERR_HGC_OK;
}

// ── 測る ────────────────────────────────────────────────────
// JPEG から輝度の中央値を出し、リニア輝度に直す。手順はキヤノン機のサムネイル測光と同じで、
//  違うのは「材料がその場にある」ことだけ(待ちも取得も要らない)。
bool apiBuiltin::measure(const std::vector<uint8_t>& jpeg, meterResult& out) const
{
	if (jpeg.empty()) { out.failStage = 1; return false; }
	uint16_t hist[cmdt::hist_bin] = {0};
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

errCode apiBuiltin::meterScene(const hgc::exposure& shotExp, meterResult& out,
                               const std::function<bool()>& keepGoing)
{
	(void)keepGoing;
	// 露光が終わって画像が出てくるのをここで待つ(シャッターは待たずに戻っている)。
	if (lastJpeg_.empty()) { this->shootTake(lastJpeg_); }
	if (!this->measure(lastJpeg_, out))
	{
		// 直前のコマが撮れていない。上位はこれが続いたら「カメラがオンラインでない」と見る。
		out.ok = false; out.shotMissing = lastJpeg_.empty();
		return ERR_HGC_RDY_METARING;
	}
	out.meterExp = shotExp;	// 測ったのは撮影画像そのもの=撮影露出で測った
	out.sceneRef = out.linear / std::pow(2.0, expo::brightnessStops(shotExp, tables_));
	return ERR_HGC_OK;
}

errCode apiBuiltin::meterHere(meterResult& out, const std::function<bool()>& keepGoing)
{
	(void)keepGoing;
	if (builtinCam::open(id_).empty()) { opened_ = true; }
	std::vector<uint8_t> jpeg;
	if (!this->shootStart() || !this->shootTake(jpeg))
	{ out.ok = false; out.failStage = 20; return ERR_HGC_RDY_METARING; }
	if (!this->measure(jpeg, out)) { out.ok = false; return ERR_HGC_RDY_METARING; }
	// いま載せている露出で撮ったので、それが測光露出そのもの。
	hgc::exposure me; me.iso = curIso_; me.ss = curSs_; me.fn = curFn_;
	out.meterExp = me;
	out.sceneRef = out.linear / std::pow(2.0, expo::brightnessStops(me, tables_));
	lastJpeg_ = jpeg;	// 続けて meterScene が呼ばれても材料が揃っている
	return ERR_HGC_OK;
}
