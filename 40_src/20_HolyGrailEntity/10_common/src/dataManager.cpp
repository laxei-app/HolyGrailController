#include "common.h"
#include "dataManager.h"
#include "osFile.h"
#include "csJson.h"
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <memory>

using json = nlohmann::json;

// 出荷時設定の撮影制御方法一式(データ構造仕様書43 §3 / §7.2)。
astro::ccmSet dataManager::factoryCcmSet(void)
{
	astro::ccmSet set;

	auto night = std::make_shared<hgc::ccmNight>();
	night->name = "night";
	night->sunAltitude = -18.0;
	night->autoEdge = true;
	night->limitBright = night->limitDark = hgc::exposure{ "1600", "8", "1.4" };	// 固定露出(3.2)
	set.night = night;

	auto sunrise = std::make_shared<hgc::ccmSunrise>();
	sunrise->name = "sunrise";
	sunrise->sunAltitude    = -6.0;	// 撮り始め(市民薄明)
	sunrise->sunAltitudeEnd =  0.0;	// 終わり(日の出)
	sunrise->ev = -3.0;
	sunrise->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	sunrise->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	set.sunrise = sunrise;

	auto sunset = std::make_shared<hgc::ccmSunset>();
	sunset->name = "sunset";
	sunset->sunAltitude    =  0.0;	// 撮り始め(日の入り)
	sunset->sunAltitudeEnd = -6.0;	// 終わり(市民薄明)
	sunset->ev = -3.0;
	sunset->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	sunset->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	set.sunset = sunset;

	auto day = std::make_shared<hgc::ccmDay>();
	day->name = "day";
	day->ev = 0.0;
	day->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	day->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	set.day = day;

	return set;
}

// 出荷時設定の月の影響への対処(データ構造仕様書43 §3.6.2)。
std::shared_ptr<hgc::ccmMoon> dataManager::factoryMoon(void)
{
	auto m = std::make_shared<hgc::ccmMoon>();
	m->name = "moon";
	m->mode = hgc::moonMode::none;
	m->startLuminance = 0.0;
	m->ev = 0.0;
	m->initialExposure = hgc::exposure{ "100", "1/125", "11" };	// 満月の目安(looney 11)
	m->atmosphericExtinction = false;
	m->extinctionCoef = 0.2;
	m->geocentricCorrection = false;
	m->skyBrightnessCoef = 100.0;
	m->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	m->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	return m;
}

// ============================================================================
//  撮影制御方法の初期値(/asset/ccmDefaults.json。仕様書43 §7.6)
// ============================================================================
namespace
{
	astro::ccmSet                 g_ccmDefaults;
	std::shared_ptr<hgc::ccmMoon> g_moonDefault;
	bool                          g_defaultsLoaded = false;

	std::string ccmDefaultsPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/ccmDefaults.json");
	}

	// JSON文字列を ccmSet(night/sunrise/sunset/day)+moon へ復元する。
	// 4種揃わなければ失敗(false)。moon は任意(無ければ nullptr)。
	bool parseDefaults(const std::string& s, astro::ccmSet& set, std::shared_ptr<hgc::ccmMoon>& moon)
	{
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return false; }
		// ESP32 は -fno-rtti のため dynamic_cast 不可。type メンバで確認して static_cast する。
		auto get = [&](const char* k, hgc::ccmType want) -> std::shared_ptr<hgc::ccmBase> {
			if (!j.contains(k)) { return nullptr; }
			auto c = csjson::ccmFromJson(j[k].dump());
			return (c && c->type == want) ? c : nullptr;
		};
		set.night   = std::static_pointer_cast<hgc::ccmNight>(get("night", hgc::ccmType::night));
		set.sunrise = std::static_pointer_cast<hgc::ccmSunrise>(get("sunrise", hgc::ccmType::sunrise));
		set.sunset  = std::static_pointer_cast<hgc::ccmSunset>(get("sunset", hgc::ccmType::sunset));
		set.day     = std::static_pointer_cast<hgc::ccmDay>(get("day", hgc::ccmType::day));
		moon        = std::static_pointer_cast<hgc::ccmMoon>(get("moon", hgc::ccmType::moon));
		return set.night && set.sunrise && set.sunset && set.day;
	}

	void ensureLoaded(void)
	{
		if (g_defaultsLoaded) { return; }
		g_defaultsLoaded = true;
		std::string path = ccmDefaultsPath();
		std::string body;
		bool ok = (!path.empty() && osfile::readAll(path, body) &&
		           parseDefaults(body, g_ccmDefaults, g_moonDefault));
		if (!ok) { g_ccmDefaults = dataManager::factoryCcmSet(); g_moonDefault.reset(); }
		if (!g_moonDefault) { g_moonDefault = dataManager::factoryMoon(); }	// moon は常に用意
	}
}

astro::ccmSet dataManager::currentCcmSet(void)
{
	ensureLoaded();
	return g_ccmDefaults;
}

std::string dataManager::ccmSetToJson(const astro::ccmSet& set, const std::shared_ptr<hgc::ccmMoon>& moon)
{
	json j;
	j["version"] = 1;
	if (set.night)   { j["night"]   = json::parse(csjson::ccmToJson(*set.night)); }
	if (set.sunrise) { j["sunrise"] = json::parse(csjson::ccmToJson(*set.sunrise)); }
	if (set.sunset)  { j["sunset"]  = json::parse(csjson::ccmToJson(*set.sunset)); }
	if (set.day)     { j["day"]     = json::parse(csjson::ccmToJson(*set.day)); }
	if (moon)        { j["moon"]    = json::parse(csjson::ccmToJson(*moon)); }
	return j.dump();
}

bool dataManager::parseCcmSetJson(const std::string& jsonStr, astro::ccmSet& set, std::shared_ptr<hgc::ccmMoon>& moon)
{
	return parseDefaults(jsonStr, set, moon);
}

std::string dataManager::ccmDefaultsJson(void)
{
	ensureLoaded();
	return ccmSetToJson(g_ccmDefaults, g_moonDefault);
}

bool dataManager::setCcmDefaultsJson(const std::string& jsonStr)
{
	astro::ccmSet set;
	std::shared_ptr<hgc::ccmMoon> moon;
	if (!parseDefaults(jsonStr, set, moon)) { return false; }
	g_ccmDefaults = set;
	g_moonDefault = moon ? moon : factoryMoon();
	g_defaultsLoaded = true;
	std::string path = ccmDefaultsPath();
	if (path.empty()) { return false; }
	return osfile::writeAll(path, jsonStr.data(), jsonStr.size());
}

// ============================================================================
//  撮影計画の永続化(案A /plan。当面は単一ファイル plan.json)
// ============================================================================
namespace
{
	std::string planPath(void)
	{
		std::string d = osfile::dir("plan");
		return d.empty() ? std::string() : (d + "/plan.json");
	}
}

bool dataManager::savePlanJson(const std::string& json)
{
	std::string p = planPath();
	if (p.empty()) { return false; }
	return osfile::writeAll(p, json.data(), json.size());
}

bool dataManager::loadPlanJson(std::string& out)
{
	std::string p = planPath();
	return !p.empty() && osfile::readAll(p, out);
}

bool dataManager::splitSavedPlan(const std::string& wrapped, std::string& planOut, std::string& ccmOut)
{
	ccmOut.clear();
	json w = json::parse(wrapped, nullptr, false);
	if (w.is_discarded() || !w.is_object()) { return false; }
	if (w.contains("plan"))		// ラッパー形式 {"plan":..,"planCcm":..}
	{
		planOut = w["plan"].dump();
		if (w.contains("planCcm")) { ccmOut = w["planCcm"].dump(); }
		return true;
	}
	planOut = wrapped;			// 旧形式: 素の cs JSON
	return true;
}

// 出荷時設定の露出平滑化(データ構造仕様書43 §5.10 の出荷時設定)。
hgc::exposureSmoothing dataManager::factorySmoothing(void)
{
	hgc::exposureSmoothing s;	// 既定値がそのまま出荷時設定(hysteresis=1.0, movingAverage=5)
	return s;
}

// 固定撮影計画の出荷時設定部分(場所=東京・機材=EOS R10 + 16mm 等)。
void dataManager::factoryFixedPlan(hgc::cs& plan)
{
	plan.name = "FixedPlan";

	plan.place.name = "Tokyo";
	plan.place.latitude  = 35.681;
	plan.place.longitude = 139.767;
	plan.place.altitude  = 40.0;

	plan.camera.maker = "Canon";
	plan.camera.model = "EOS R10";
	plan.camera.name  = "EOS R10";
	plan.camera.sensorSize  = 22.3;
	plan.camera.sensorPixel = 6000;

	plan.lens.maker = "Sigma";
	plan.lens.name  = "16mm F1.4 DC DN";
	plan.lens.focalLength = 16.0;
	plan.lens.fn = 1.4;

	plan.interval  = 15.0;	// 撮影周期[秒](EOS最小)
	plan.azimuth   = 90.0;	// 東向き
	plan.elevation = 10.0;
	plan.landscape = true;
}

// ============================================================================
//  動作ログ(データ構造仕様書43 §8)。固定長128Bテキストレコード/日次ファイル。
// ============================================================================
namespace
{
	int g_logOff = 0;	// ログのタイムスタンプ用 UTCオフセット[分]

	// rec の off から幅 w に s を詰める(left=左詰/右詰)。超過は切り捨て、余白は空白のまま。
	void putField(char* rec, int off, int w, const char* s, bool left)
	{
		int n = static_cast<int>(std::strlen(s));
		if (n > w) { n = w; }
		int dst = left ? off : (off + (w - n));
		std::memcpy(rec + dst, s, static_cast<size_t>(n));
	}

	// 現在のローカル時刻文字列(time 用19文字 と date 用10文字)を作る。
	// std::time(UTC秒) + オフセットを gmtime することでタイムゾーン非依存にローカル化する。
	void nowLocal(char timeStr[20], char dateStr[11])
	{
		std::time_t lt = std::time(nullptr) + static_cast<std::time_t>(g_logOff) * 60;
		std::tm g{};
#if defined(_WIN32)
		gmtime_s(&g, &lt);
#else
		gmtime_r(&lt, &g);
#endif
		std::snprintf(timeStr, 20, "%04d-%02d-%02d %02d:%02d:%02d",
		              g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
		std::snprintf(dateStr, 11, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
	}

	// 1レコード(128B固定長)を組み立てて日付ファイルへ追記する。
	// 各列の配置は §8.3 のとおり。空欄は空文字列を渡す。
	void writeRecord(const char* lvl, const char* event, const char* frame,
	                 const char* iso, const char* ss, const char* fn,
	                 const char* lum, const char* detail)
	{
		char rec[128];
		std::memset(rec, ' ', sizeof(rec));

		char timeStr[20], dateStr[11];
		nowLocal(timeStr, dateStr);

		std::memcpy(rec + 0, timeStr, 19);	rec[19] = '|';
		putField(rec, 20, 3,  lvl,    true);	rec[23] = '|';
		putField(rec, 24, 6,  event,  true);	rec[30] = '|';
		putField(rec, 31, 6,  frame,  false);	rec[37] = '|';
		putField(rec, 38, 5,  iso,    false);	rec[43] = '|';
		putField(rec, 44, 11, ss,     true);	rec[55] = '|';
		putField(rec, 56, 6,  fn,     true);	rec[62] = '|';
		putField(rec, 63, 8,  lum,    false);	rec[71] = '|';
		putField(rec, 72, 55, detail, true);
		rec[127] = '\n';

		std::string dir = osfile::logDir();
		if (dir.empty()) { return; }
		std::string path = dir + "/hg_" + dateStr + ".log";
		osfile::append(path, rec, sizeof(rec));
	}

}

void dataManager::setLogOffset(int utcOffsetMin)
{
	g_logOff = utcOffsetMin;
}

void dataManager::logEvent(const char* event, const char* detail, bool error)
{
	writeRecord(error ? "ERR" : "INF", event ? event : "", "", "", "", "", "",
	            detail ? detail : "");
}

std::string dataManager::currentLogPath(void)
{
	char timeStr[20], dateStr[11];
	nowLocal(timeStr, dateStr);
	std::string dir = osfile::logDir();
	if (dir.empty()) { return ""; }
	return dir + "/hg_" + dateStr + ".log";
}

void dataManager::logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName)
{
	char frameStr[8], lumStr[12];
	std::snprintf(frameStr, sizeof(frameStr), "%d", frame);
	std::snprintf(lumStr,   sizeof(lumStr),   "%+.3f", lumStops);
	// 露出値はカメラ設定値の文字列をそのまま記録する。
	writeRecord("INF", "SHOT", frameStr, e.iso.c_str(), e.ss.c_str(), e.fn.c_str(),
	            lumStr, ccmName ? ccmName : "");
}
