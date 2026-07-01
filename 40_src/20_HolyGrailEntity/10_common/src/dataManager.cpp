#include "common.h"
#include "dataManager.h"
#include "osFile.h"
#include "csJson.h"
#include "device.h"
#include "exposureMath.h"
#include <json/nlohmann/json.hpp>
#include <vector>
#include <algorithm>
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
	night->limitBright = night->limitDark = hgc::exposure{ "1600", "8", "1.4" };	// 固定露出(3.2)
	night->initial = night->limitBright;	// 夜間の基準=固定露出
	set.night = night;

	auto sunrise = std::make_shared<hgc::ccmSunrise>();
	sunrise->name = "sunrise";
	sunrise->sunAltitude    = -6.0;	// 撮り始め(市民薄明)
	sunrise->sunAltitudeEnd =  0.0;	// 終わり(日の出)
	sunrise->ev = -3.0;
	sunrise->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	sunrise->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	sunrise->initial = sunrise->limitBright;	// 朝日=暗所限界(夜明け前は暗い)から始める
	sunrise->hysteresis = 0.3; sunrise->movingAverage = 3;	// 朝日は急変するので個別平滑化(§7)
	set.sunrise = sunrise;

	auto sunset = std::make_shared<hgc::ccmSunset>();
	sunset->name = "sunset";
	sunset->sunAltitude    =  0.0;	// 撮り始め(日の入り)
	sunset->sunAltitudeEnd = -6.0;	// 終わり(市民薄明)
	sunset->ev = -3.0;
	sunset->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	sunset->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	sunset->initial = sunset->limitDark;	// 夕日=明所限界(日中は明るい)から始める
	sunset->hysteresis = 0.3; sunset->movingAverage = 3;	// 夕日は急変するので個別平滑化(§7)
	set.sunset = sunset;

	auto day = std::make_shared<hgc::ccmDay>();
	day->name = "day";
	day->ev = 0.0;
	day->limitBright = hgc::exposure{ "3200", "8", "1.4" };
	day->limitDark   = hgc::exposure{ "100", "1/4000", "16" };
	day->initial = hgc::exposure{ "640", "1/20", "4.5" };	// 中間点(明所/暗所限界のAPEX中間の目安。編集で変更可)
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
		// 撮影制御方法の初期値は参照専用。ファイルを持たずコード上の出荷時設定から取得する。
		g_ccmDefaults = dataManager::factoryCcmSet();
		g_moonDefault = dataManager::factoryMoon();	// moon は常に用意
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
	// plan_<id>.json のフルパス。
	std::string planFilePath(const std::string& id)
	{
		std::string d = osfile::dir("plan");
		return d.empty() ? std::string() : (d + "/plan_" + id + ".json");
	}
}

bool dataManager::savePlanFile(const std::string& id, const std::string& wrappedJson)
{
	std::string p = planFilePath(id);
	if (p.empty() || id.empty()) { return false; }
	return osfile::writeAll(p, wrappedJson.data(), wrappedJson.size());
}

bool dataManager::loadPlanFile(const std::string& id, std::string& out)
{
	std::string p = planFilePath(id);
	return !p.empty() && !id.empty() && osfile::readAll(p, out);
}

bool dataManager::deletePlanFile(const std::string& id)
{
	if (id.empty()) { return false; }
	return osfile::removeFile("plan", "plan_" + id + ".json");
}

std::vector<std::string> dataManager::listPlanIds(void)
{
	std::vector<std::string> ids;
	for (const std::string& n : osfile::listFiles("plan", "plan_", ".json"))
	{
		// "plan_<id>.json" → "<id>"
		std::string id = n.substr(5, n.size() - 5 - 5);	// 前"plan_"(5) 後".json"(5)
		if (!id.empty()) { ids.push_back(id); }
	}
	std::sort(ids.begin(), ids.end());
	return ids;
}

bool dataManager::saveCapturingIds(const std::vector<std::string>& ids)
{
	std::string d = osfile::dir("asset");
	if (d.empty()) { return false; }
	std::string s = "[";
	for (size_t i = 0; i < ids.size(); ++i) { if (i) { s += ","; } s += "\"" + ids[i] + "\""; }
	s += "]";
	return osfile::writeAll(d + "/capturing.json", s.data(), s.size());
}

bool dataManager::loadCapturingIds(std::vector<std::string>& out)
{
	out.clear();
	std::string d = osfile::dir("asset");
	if (d.empty()) { return false; }
	std::string s;
	if (!osfile::readAll(d + "/capturing.json", s)) { return false; }
	// 簡易パース: "id" を順に取り出す(idは yyyyMMdd-HHmmss 等で特殊文字を含まない)。
	size_t i = 0;
	while (i < s.size())
	{
		size_t q1 = s.find('"', i); if (q1 == std::string::npos) { break; }
		size_t q2 = s.find('"', q1 + 1); if (q2 == std::string::npos) { break; }
		out.push_back(s.substr(q1 + 1, q2 - q1 - 1));
		i = q2 + 1;
	}
	return true;
}

// 直近に接続できたカメラのIPを serial ごとに保存する(SSDP広告停止対策)。"*"=最後に繋がった host。
bool dataManager::saveCameraHost(const std::string& serial, const std::string& host)
{
	if (host.empty()) { return false; }
	std::string d = osfile::dir("asset");
	if (d.empty()) { return false; }
	std::string path = d + "/cameraHosts.json";
	json j = json::object();
	std::string body;
	if (osfile::readAll(path, body) && !body.empty())
	{
		try { j = json::parse(body); if (!j.is_object()) { j = json::object(); } }
		catch (const std::exception&) { j = json::object(); }
	}
	if (!serial.empty()) { j[serial] = host; }
	j["*"] = host;	// serial 不明の計画でも使える最後の既知 host
	std::string s = j.dump();
	return osfile::writeAll(path, s.data(), s.size());
}

std::string dataManager::loadCameraHost(const std::string& serial)
{
	std::string d = osfile::dir("asset");
	if (d.empty()) { return std::string(); }
	std::string body;
	if (!osfile::readAll(d + "/cameraHosts.json", body) || body.empty()) { return std::string(); }
	try {
		json j = json::parse(body);
		if (j.is_object())
		{
			if (!serial.empty() && j.contains(serial) && j[serial].is_string()) { return j[serial].get<std::string>(); }
			if (j.contains("*") && j["*"].is_string())                          { return j["*"].get<std::string>(); }
		}
	} catch (const std::exception&) {}
	return std::string();
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

bool dataManager::removeLegacyPlan(void)
{
	return osfile::removeFile("plan", "plan.json");
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
	plan.camera.sensorSize  = 22.4;	// センサー横[mm](APS-C)
	plan.camera.sensorSizeV = 14.9;	// センサー縦[mm]
	plan.camera.sensorPixel = 6000;

	plan.lens.maker = "Sigma";
	plan.lens.name  = "SIGMA 12mm F1.4 DC";
	plan.lens.focalLength = 12.0;
	plan.lens.fn = 1.4;

	plan.interval  = 15.0;	// 撮影周期[秒](EOS最小)
	plan.azimuth   = 90.0;	// 東向き(日の出方向)
	plan.elevation = 10.0;
	plan.landscape = true;
}

// ============================================================================
//  機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6)
// ============================================================================
namespace
{
	// --- 機材マスタ(読取専用。/master/cameras.json・lenses.json) ---
	std::vector<hgc::camera> g_masterCameras;
	std::vector<hgc::lens>   g_masterLenses;
	bool                     g_masterLoaded = false;

	std::string masterPath(const char* file)
	{
		std::string d = osfile::dir("master");
		return d.empty() ? std::string() : (d + "/" + file);
	}

	// 出荷時フォールバック(マスタ未配置時)。最低限 EOS R10 + 16mm が選べるようにする。
	void masterFallback(void)
	{
		hgc::cs fp;
		dataManager::factoryFixedPlan(fp);
		g_masterCameras.clear();
		g_masterCameras.push_back(fp.camera);
		g_masterLenses.clear();
		g_masterLenses.push_back(fp.lens);
	}

	void ensureMaster(void)
	{
		if (g_masterLoaded) { return; }
		g_masterLoaded = true;
		std::string pc = masterPath("cameras.json");
		std::string pl = masterPath("lenses.json");
		std::string body;
		bool okC = (!pc.empty() && osfile::readAll(pc, body) &&
		            csjson::camerasFromMasterJson(body, g_masterCameras) && !g_masterCameras.empty());
		body.clear();
		bool okL = (!pl.empty() && osfile::readAll(pl, body) &&
		            csjson::lensesFromMasterJson(body, g_masterLenses) && !g_masterLenses.empty());
		if (!okC || !okL) { masterFallback(); }	// どちらか欠けたら最低限を用意
	}

	// --- 所持機材(/asset/ownedCameras.json・ownedLenses.json) ---
	std::vector<hgc::ownedCamera> g_ownedCameras;
	std::vector<hgc::lens>        g_ownedLenses;
	bool                          g_ownedLoaded = false;

	std::string ownedCamPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/ownedCameras.json");
	}
	std::string ownedLensPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/ownedLenses.json");
	}

	void ensureOwned(void)
	{
		if (g_ownedLoaded) { return; }
		g_ownedLoaded = true;
		std::string body;
		std::string pc = ownedCamPath();
		if (!pc.empty() && osfile::readAll(pc, body)) { csjson::ownedCamerasFromJson(body, g_ownedCameras); }
		body.clear();
		std::string pl = ownedLensPath();
		if (!pl.empty() && osfile::readAll(pl, body)) { csjson::ownedLensesFromJson(body, g_ownedLenses); }
	}

	bool saveOwnedCameras(void)
	{
		std::string p = ownedCamPath();
		if (p.empty()) { return false; }
		std::string s = csjson::ownedCamerasToJson(g_ownedCameras);
		return osfile::writeAll(p, s.data(), s.size());
	}
	bool saveOwnedLenses(void)
	{
		std::string p = ownedLensPath();
		if (p.empty()) { return false; }
		std::string s = csjson::ownedLensesToJson(g_ownedLenses);
		return osfile::writeAll(p, s.data(), s.size());
	}

	const hgc::camera* findMasterCamera(const std::string& name)
	{
		ensureMaster();
		for (const auto& c : g_masterCameras) { if (c.name == name) { return &c; } }
		return nullptr;
	}

	// device.model("Canon EOS R10")から先頭のメーカー名を除いた型番を得る("EOS R10")。
	std::string stripMaker(const std::string& model, const std::string& maker)
	{
		std::string m = model;
		if (!maker.empty() && m.size() >= maker.size() &&
		    m.compare(0, maker.size(), maker) == 0)
		{
			m.erase(0, maker.size());
			while (!m.empty() && (m.front() == ' ' || m.front() == '\t')) { m.erase(0, 1); }
		}
		return m;
	}

	// dev に最も一致するマスタカメラを返す。完全一致優先、無ければ最長部分一致。
	// ("EOS R6" が "EOS R6 Mark II" を誤って先取りしないよう最長一致を採る)
	const hgc::camera* matchMasterCamera(const device& dev)
	{
		ensureMaster();
		std::string key = stripMaker(dev.model, dev.manufacturer);
		const hgc::camera* best = nullptr;
		size_t bestLen = 0;
		for (const auto& c : g_masterCameras)
		{
			if (!c.name.empty() && c.name == key) { return &c; }		// 完全一致
			if (!c.name.empty() && key.find(c.name) != std::string::npos && c.name.size() > bestLen)
			{
				best = &c; bestLen = c.name.size();
			}
		}
		return best;
	}
	const hgc::lens* findMasterLens(const std::string& name)
	{
		ensureMaster();
		for (const auto& l : g_masterLenses) { if (l.name == name) { return &l; } }
		return nullptr;
	}

	// 所持/計画カメラ cam の型番が device dev と同じ機種か(メーカー名差を吸収)。
	bool camModelMatchesDev(const hgc::camera& cam, const device& dev)
	{
		std::string key = stripMaker(dev.model, dev.manufacturer);
		if (!cam.name.empty()  && (cam.name  == key || cam.name  == dev.model)) { return true; }
		if (!cam.model.empty() && (cam.model == key || cam.model == dev.model)) { return true; }
		return false;
	}

	// 所持カメラリスト内で一意な表示名を返す(同名があれば " (2)"," (3)"... を付す)。name はリストのキー。
	std::string uniqueOwnedName(const std::string& base)
	{
		std::string b = base.empty() ? std::string("カメラ") : base;
		bool taken = false;
		for (const auto& oc : g_ownedCameras) { if (oc.cam.name == b) { taken = true; break; } }
		if (!taken) { return b; }
		for (int n = 2; n < 100; ++n)
		{
			std::string cand = b + " (" + std::to_string(n) + ")";
			bool used = false;
			for (const auto& oc : g_ownedCameras) { if (oc.cam.name == cand) { used = true; break; } }
			if (!used) { return cand; }
		}
		return b;
	}
}

std::string dataManager::masterCamerasJson(void)
{
	ensureMaster();
	std::vector<hgc::ownedCamera> tmp;	// camera 配列を所持形式の "camera" キーで出すと UI で扱いやすい
	tmp.reserve(g_masterCameras.size());
	for (const auto& c : g_masterCameras) { hgc::ownedCamera oc; oc.cam = c; tmp.push_back(std::move(oc)); }
	return csjson::ownedCamerasToJson(tmp);
}

std::string dataManager::masterLensesJson(void)
{
	ensureMaster();
	return csjson::ownedLensesToJson(g_masterLenses);
}

std::string dataManager::ownedCamerasJson(void)
{
	ensureOwned();
	return csjson::ownedCamerasToJson(g_ownedCameras);
}

std::string dataManager::ownedLensesJson(void)
{
	ensureOwned();
	return csjson::ownedLensesToJson(g_ownedLenses);
}

bool dataManager::addOwnedCameraFromMaster(const std::string& name)
{
	ensureOwned();
	const hgc::camera* m = findMasterCamera(name);
	if (!m) { return false; }
	// §4a: 同じ機種で「未識別(シリアルもフレンドリ名も空)」の所持カメラが既にあれば、もう一台の未識別は
	//      区別できないため追加しない。識別済み(シリアル or フレンドリ名あり)なら2台目の登録を許可する。
	for (const auto& oc : g_ownedCameras)
	{
		bool sameModel = (!oc.cam.model.empty() && !m->model.empty() && oc.cam.model == m->model)
		              || (!oc.cam.name.empty()  && (oc.cam.name == m->name || oc.cam.name == name));
		if (sameModel && oc.cam.serial.empty() && oc.cam.friendly.empty()) { return false; }
	}
	hgc::ownedCamera oc;
	oc.cam = *m;
	if (oc.cam.model.empty()) { oc.cam.model = m->name; }	// 機種照合の基準
	oc.cam.name = uniqueOwnedName(oc.cam.name);	// 2台目以降は名称を一意化(リストのキー)
	g_ownedCameras.push_back(std::move(oc));
	return saveOwnedCameras();
}

bool dataManager::addOwnedLensFromMaster(const std::string& name)
{
	ensureOwned();
	for (const auto& l : g_ownedLenses) { if (l.name == name) { return true; } }	// 既存
	const hgc::lens* m = findMasterLens(name);
	if (!m) { return false; }
	g_ownedLenses.push_back(*m);
	return saveOwnedLenses();
}

bool dataManager::removeOwnedCamera(const std::string& name)
{
	ensureOwned();
	auto it = std::remove_if(g_ownedCameras.begin(), g_ownedCameras.end(),
	                         [&](const hgc::ownedCamera& oc) { return oc.cam.name == name; });
	if (it == g_ownedCameras.end()) { return false; }
	g_ownedCameras.erase(it, g_ownedCameras.end());
	return saveOwnedCameras();
}

bool dataManager::removeOwnedLens(const std::string& name)
{
	ensureOwned();
	auto it = std::remove_if(g_ownedLenses.begin(), g_ownedLenses.end(),
	                         [&](const hgc::lens& l) { return l.name == name; });
	if (it == g_ownedLenses.end()) { return false; }
	g_ownedLenses.erase(it, g_ownedLenses.end());
	return saveOwnedLenses();
}

bool dataManager::setOwnedCameraAutoInsert(const std::string& name, bool autoInsert)
{
	ensureOwned();
	for (auto& oc : g_ownedCameras)
	{
		if (oc.cam.name == name) { oc.autoInsert = autoInsert; return saveOwnedCameras(); }
	}
	return false;
}

namespace
{
	// 設定可能範囲[mn,mx]に入る標準1/3段の値を生成する(real昇順)。Bulb は別途付与。
	std::vector<std::string> sliceStd(expo::expoKind kind, const std::string& mn, const std::string& mx)
	{
		auto all = expo::standardValues(kind);
		double rmin = expo::parseValue(mn, kind);
		double rmax = expo::parseValue(mx, kind);
		if (rmin > 0.0 && rmax > 0.0 && rmin > rmax) { std::swap(rmin, rmax); }
		std::vector<std::string> out;
		for (const auto& v : all)
		{
			double r = expo::parseValue(v, kind);
			if (r <= 0.0) { continue; }
			if ((rmin <= 0.0 || r >= rmin - 1e-9) && (rmax <= 0.0 || r <= rmax + 1e-9)) { out.push_back(v); }
		}
		if (out.empty()) { if (!mn.empty()) { out.push_back(mn); } if (mx != mn && !mx.empty()) { out.push_back(mx); } }
		return out;
	}

	// 現在の設定可能値配列の表示上の min/max(SS は末尾の "Bulb" を除く)。
	std::string listMin(const std::vector<std::string>& v) { return v.empty() ? std::string() : v.front(); }
	std::string listMax(const std::vector<std::string>& v)
	{
		for (auto it = v.rbegin(); it != v.rend(); ++it) { if (*it != "Bulb") { return *it; } }
		return v.empty() ? std::string() : v.back();
	}
}

bool dataManager::setOwnedCameraDetailJson(const std::string& origName, const std::string& jsonStr)
{
	ensureOwned();
	json j = json::parse(jsonStr, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return false; }

	// 対象を探す(無ければ新規)
	hgc::ownedCamera* oc = nullptr;
	for (auto& c : g_ownedCameras) { if (c.cam.name == origName) { oc = &c; break; } }
	if (!oc) { g_ownedCameras.emplace_back(); oc = &g_ownedCameras.back(); }

	hgc::camera& cam = oc->cam;
	cam.maker       = j.value("maker", cam.maker);
	cam.model       = j.value("model", cam.model);
	cam.name        = j.value("name", cam.name);
	cam.friendly    = j.value("friendly", cam.friendly);
	cam.serial      = j.value("serial", cam.serial);
	cam.sensorSize  = j.value("sensorSize", cam.sensorSize);
	cam.sensorSizeV = j.value("sensorSizeV", cam.sensorSizeV);
	cam.sensorPixel = j.value("sensorPixel", cam.sensorPixel);

	// ISO/SS: min/max が現状と変わったときだけ標準1/3段で再生成(マスタ/カメラ取得値は維持)。
	std::string isoMin = j.value("isoMin", std::string());
	std::string isoMax = j.value("isoMax", std::string());
	if (!isoMin.empty() && !isoMax.empty() &&
	    (cam.isoList.empty() || isoMin != listMin(cam.isoList) || isoMax != listMax(cam.isoList)))
	{
		cam.isoList = sliceStd(expo::expoKind::iso, isoMin, isoMax);
	}
	std::string ssMin = j.value("ssMin", std::string());
	std::string ssMax = j.value("ssMax", std::string());
	if (!ssMin.empty() && !ssMax.empty() &&
	    (cam.ssList.empty() || ssMin != listMin(cam.ssList) || ssMax != listMax(cam.ssList)))
	{
		bool keepBulb = false;
		for (const auto& s : cam.ssList) { if (s == "Bulb") { keepBulb = true; break; } }
		cam.ssList = sliceStd(expo::expoKind::ss, ssMin, ssMax);
		if (keepBulb) { cam.ssList.push_back("Bulb"); }
	}

	oc->autoInsert = j.value("autoInsert", oc->autoInsert);

	// 組み合わせるレンズ(先頭が初期値)。所持レンズ名から順に解決する。
	if (j.contains("lensNames") && j["lensNames"].is_array())
	{
		oc->lensList.clear();
		for (const auto& nm : j["lensNames"])
		{
			if (!nm.is_string()) { continue; }
			hgc::lens l;
			if (findOwnedLens(nm.get<std::string>(), l)) { oc->lensList.push_back(l); }
		}
	}

	return saveOwnedCameras();
}

// ============================================================================
//  システム共通の色(全体設定。/asset/settings.json の "colors")
// ============================================================================
namespace
{
	json g_settings;
	bool g_settingsLoaded = false;

	std::string settingsPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/settings.json");
	}
	void ensureSettings(void)
	{
		if (g_settingsLoaded) { return; }
		g_settingsLoaded = true;
		std::string body, p = settingsPath();
		if (!p.empty() && osfile::readAll(p, body))
		{
			json j = json::parse(body, nullptr, false);
			if (!j.is_discarded() && j.is_object()) { g_settings = j; }
		}
		if (!g_settings.is_object()) { g_settings = json::object(); }
	}
	bool saveSettings(void)
	{
		std::string p = settingsPath();
		if (p.empty()) { return false; }
		std::string s = g_settings.dump();
		return osfile::writeAll(p, s.data(), s.size());
	}
	json factoryColors(void)
	{
		auto mk = [](uint32_t bg) { json o; o["text"] = 0x222222u; o["bg"] = bg; return o; };
		json c;
		c["night"]     = mk(0xB39DDBu);
		c["sunrise"]   = mk(0xFFF59Du);
		c["sunset"]    = mk(0xFFCC80u);
		c["day"]       = mk(0x90CAF9u);
		c["moon"]      = mk(0xCE93D8u);
		c["preNight"]  = mk(0xA5D6A7u);
		c["postNight"] = mk(0x80CBC4u);
		return c;
	}
}

std::string dataManager::colorsJson(void)
{
	ensureSettings();
	json def = factoryColors();
	if (g_settings.contains("colors") && g_settings["colors"].is_object())
	{
		for (auto& [k, v] : g_settings["colors"].items()) { if (v.is_object()) { def[k] = v; } }
	}
	return def.dump();
}

bool dataManager::setColorsJson(const std::string& jsonStr)
{
	ensureSettings();
	json j = json::parse(jsonStr, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return false; }
	g_settings["colors"] = j;
	return saveSettings();
}

// 全体設定の露出平滑化(/asset/settings.json の "smoothing")
hgc::exposureSmoothing dataManager::currentSmoothing(void)
{
	ensureSettings();
	hgc::exposureSmoothing s = factorySmoothing();
	if (g_settings.contains("smoothing") && g_settings["smoothing"].is_object())
	{
		const auto& o = g_settings["smoothing"];
		s.hysteresis    = o.value("hysteresis", s.hysteresis);
		s.movingAverage = static_cast<uint16_t>(o.value("movingAverage", static_cast<int>(s.movingAverage)));
	}
	return s;
}

std::string dataManager::smoothingJson(void)
{
	hgc::exposureSmoothing s = currentSmoothing();
	json j; j["hysteresis"] = s.hysteresis; j["movingAverage"] = s.movingAverage;
	return j.dump();
}

bool dataManager::setSmoothingJson(const std::string& jsonStr)
{
	ensureSettings();
	json j = json::parse(jsonStr, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return false; }
	json o;
	o["hysteresis"]    = j.value("hysteresis", 1.0);
	o["movingAverage"] = j.value("movingAverage", 5);
	g_settings["smoothing"] = o;
	return saveSettings();
}

// ============================================================================
//  起動時のログ整理(当日以外が5件以上なら古い順に削除し最新4件まで残す)
// ============================================================================
int dataManager::pruneOldLogs(int offMin)
{
	// 当日(ローカル)の日付文字列を作る。
	std::time_t lt = std::time(nullptr) + static_cast<std::time_t>(offMin) * 60;
	std::tm g{};
#if defined(_WIN32)
	gmtime_s(&g, &lt);
#else
	gmtime_r(&lt, &g);
#endif
	char today[24];
	std::snprintf(today, sizeof(today), "hg_%04d-%02d-%02d.log", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);

	std::vector<std::string> all = osfile::logFileNames();	// hg_YYYY-MM-DD.log 群
	std::vector<std::string> others;
	for (const auto& f : all) { if (f != today) { others.push_back(f); } }
	if (others.size() < 5) { return 0; }		// 当日以外が5件未満なら何もしない
	std::sort(others.begin(), others.end());	// 名前=日付昇順 → 先頭が最古
	int removed = 0;
	const size_t keep = 4;						// 当日以外は最新4件まで残す
	for (size_t i = 0; i + keep < others.size(); ++i)
	{
		if (osfile::removeLog(others[i])) { ++removed; }
	}
	return removed;
}

// ============================================================================
//  撮影制御方法の初期値プリセット(/asset/ccmPresets.json。型ごとに複数)
// ============================================================================
namespace
{
	json g_presets;
	bool g_presetsLoaded = false;
	const char* kPresetTypes[5] = { "night", "sunrise", "sunset", "day", "moon" };

	std::string presetsPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/ccmPresets.json");
	}
	bool savePresets(void)
	{
		std::string p = presetsPath();
		if (p.empty()) { return false; }
		std::string s = g_presets.dump();
		return osfile::writeAll(p, s.data(), s.size());
	}
	void ensurePresets(void)
	{
		if (g_presetsLoaded) { return; }
		g_presetsLoaded = true;
		std::string body, p = presetsPath();
		if (!p.empty() && osfile::readAll(p, body))
		{
			json j = json::parse(body, nullptr, false);
			if (!j.is_discarded() && j.is_object()) { g_presets = j; }
		}
		if (!g_presets.is_object()) { g_presets = json::object(); }

		// 種まき: 型ごとに最低1件。撮影制御方法の初期値(ccmDefaults)から「標準」を作る。
		json def = json::parse(dataManager::ccmDefaultsJson(), nullptr, false);
		ensureSettings();
		bool changed = false, settingsChanged = false;
		if (!g_settings.contains("preferredCcm") || !g_settings["preferredCcm"].is_object())
		{
			g_settings["preferredCcm"] = json::object();
		}
		for (const char* t : kPresetTypes)
		{
			if (!g_presets.contains(t) || !g_presets[t].is_array() || g_presets[t].empty())
			{
				json one = (def.is_object() && def.contains(t)) ? def[t] : json::object();
				one["name"] = "標準";
				g_presets[t] = json::array({ one });
				changed = true;
				if (!g_settings["preferredCcm"].contains(t)) { g_settings["preferredCcm"][t] = "標準"; settingsChanged = true; }
			}
		}
		if (changed) { savePresets(); }
		if (settingsChanged) { saveSettings(); }
	}
}

std::string dataManager::ccmPresetsJson(const std::string& type)
{
	ensurePresets();
	if (g_presets.contains(type) && g_presets[type].is_array()) { return g_presets[type].dump(); }
	return "[]";
}

bool dataManager::setCcmPresetJson(const std::string& type, const std::string& origName, const std::string& ccmJson)
{
	ensurePresets();
	json c = json::parse(ccmJson, nullptr, false);
	if (c.is_discarded() || !c.is_object()) { return false; }
	if (!g_presets.contains(type) || !g_presets[type].is_array()) { g_presets[type] = json::array(); }
	auto& arr = g_presets[type];
	for (auto& e : arr)
	{
		if (e.is_object() && e.value("name", std::string()) == origName) { e = c; return savePresets(); }
	}
	arr.push_back(c);
	return savePresets();
}

bool dataManager::removeCcmPreset(const std::string& type, const std::string& name)
{
	ensurePresets();
	if (!g_presets.contains(type) || !g_presets[type].is_array()) { return false; }
	auto& arr = g_presets[type];
	for (auto it = arr.begin(); it != arr.end(); ++it)
	{
		if (it->is_object() && it->value("name", std::string()) == name) { arr.erase(it); return savePresets(); }
	}
	return false;
}

std::string dataManager::preferredCcmName(const std::string& type)
{
	ensureSettings();
	if (g_settings.contains("preferredCcm") && g_settings["preferredCcm"].is_object() &&
	    g_settings["preferredCcm"].contains(type) && g_settings["preferredCcm"][type].is_string())
	{
		return g_settings["preferredCcm"][type].get<std::string>();
	}
	return std::string();
}

bool dataManager::setPreferredCcm(const std::string& type, const std::string& name)
{
	ensureSettings();
	if (!g_settings.contains("preferredCcm") || !g_settings["preferredCcm"].is_object())
	{
		g_settings["preferredCcm"] = json::object();
	}
	g_settings["preferredCcm"][type] = name;
	return saveSettings();
}

bool dataManager::setOwnedLensDetailJson(const std::string& origName, const std::string& jsonStr)
{
	ensureOwned();
	json j = json::parse(jsonStr, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { return false; }

	hgc::lens* lp = nullptr;
	for (auto& l : g_ownedLenses) { if (l.name == origName) { lp = &l; break; } }
	if (!lp) { g_ownedLenses.emplace_back(); lp = &g_ownedLenses.back(); }

	lp->maker       = j.value("maker", lp->maker);
	lp->name        = j.value("name", lp->name);
	lp->focalLength = j.value("focalLength", lp->focalLength);
	lp->fn          = j.value("fn", lp->fn);
	lp->fnMax       = j.value("fnMax", lp->fnMax);
	lp->hasContact  = j.value("hasContact", lp->hasContact);
	return saveOwnedLenses();
}

bool dataManager::findOwnedCamera(const std::string& name, hgc::camera& out)
{
	ensureOwned();
	for (const auto& oc : g_ownedCameras) { if (oc.cam.name == name) { out = oc.cam; return true; } }
	return false;
}

bool dataManager::findOwnedLens(const std::string& name, hgc::lens& out)
{
	ensureOwned();
	for (const auto& l : g_ownedLenses) { if (l.name == name) { out = l; return true; } }
	return false;
}

bool dataManager::recordConnectedCamera(const device& dev)
{
	ensureOwned();
	// device.model 例 "Canon EOS R10"。所持/マスタは型番のみ("EOS R10")なのでメーカー名を除いて照合。
	std::string key = stripMaker(dev.model, dev.manufacturer);

	// 1) シリアル一致の所持カメラ → 同一個体。フレンドリ名のみ更新する(serialは識別子なので変えない)。
	if (!dev.serialno.empty())
	{
		for (auto& oc : g_ownedCameras)
		{
			if (!oc.cam.serial.empty() && oc.cam.serial == dev.serialno)
			{
				bool changed = false;
				if (!dev.friendName.empty() && oc.cam.friendly != dev.friendName) { oc.cam.friendly = dev.friendName; changed = true; }
				return changed ? saveOwnedCameras() : true;
			}
		}
	}

	// 2) シリアル一致なし → 同機種で「未識別(serial空)」の所持カメラがあれば、それを確定させる(プレースホルダを埋める)。
	//    §4a: 未識別の同機種は最大1台なので、これが該当する唯一のものになる。
	for (auto& oc : g_ownedCameras)
	{
		if (camModelMatchesDev(oc.cam, dev) && oc.cam.serial.empty())
		{
			oc.cam.serial = dev.serialno;
			if (!dev.friendName.empty()) { oc.cam.friendly = dev.friendName; }
			return saveOwnedCameras();
		}
	}

	// 3) 同機種は全て別シリアルで確定済み(またはモデル不一致) → 新規の別個体として追加。
	//    §4a: 既存が識別済みなら2台目の登録は可。device は実機なのでシリアルを持ち、未識別の重複は作らない。
	hgc::ownedCamera oc;
	const hgc::camera* m = matchMasterCamera(dev);
	if (m) { oc.cam = *m; }
	else
	{
		oc.cam.model = key.empty() ? dev.model : key;
		oc.cam.name  = oc.cam.model;
		oc.cam.maker = dev.manufacturer;
	}
	if (oc.cam.model.empty()) { oc.cam.model = key.empty() ? dev.model : key; }	// 機種照合の基準(名称は一意化で変わるため model を確実に持たせる)
	if (oc.cam.name.empty()) { oc.cam.name = dev.friendName.empty() ? (key.empty() ? dev.model : key) : dev.friendName; }
	oc.cam.name     = uniqueOwnedName(oc.cam.name);	// 同機種2台目以降は名称を一意化(リストのキー)
	oc.cam.serial   = dev.serialno;
	oc.cam.friendly = dev.friendName;
	g_ownedCameras.push_back(std::move(oc));
	return saveOwnedCameras();
}

// §4b: 計画カメラの friendly から所持リストを引き、実シリアルを解決する(接続済みなら serial が入っている)。
bool dataManager::serialForFriendly(const std::string& friendly, std::string& outSerial)
{
	if (friendly.empty()) { return false; }
	ensureOwned();
	for (const auto& oc : g_ownedCameras)
	{
		if (oc.cam.friendly == friendly && !oc.cam.serial.empty()) { outSerial = oc.cam.serial; return true; }
	}
	return false;
}

// §4b: 所持カメラに登録済みのシリアル一覧(空は除く)。「それ以外のカメラ」判定に使う。
void dataManager::ownedCameraSerials(std::vector<std::string>& out)
{
	ensureOwned();
	for (const auto& oc : g_ownedCameras) { if (!oc.cam.serial.empty()) { out.push_back(oc.cam.serial); } }
}

// §4b: device のモデルが計画カメラ(cam)と同機種か。
bool dataManager::cameraModelMatches(const device& dev, const hgc::camera& cam)
{
	return camModelMatchesDev(cam, dev);
}

// ============================================================================
//  動作ログ(データ構造仕様書43 §8)。固定長128Bテキストレコード/日次ファイル。
// ============================================================================
namespace
{
	int g_logOff = 0;	// ログのタイムスタンプ用 UTCオフセット[分]

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

	// 1レコードを組み立てて日付ファイルへ追記する(可変長・改行終端。仕様書 §8.3 改定)。
	// 固定フォーマット部 = 「time(19) | lvl(3) | event(6)」まで。それ以降の body は各イベント自由形式。
	// SHOT のみ body 内に frame〜lum を列整形する(他イベントは body を event 直後へ左詰めで置く)。
	void writeRecord(const char* lvl, const char* event, const std::string& body)
	{
		char timeStr[20], dateStr[11];
		nowLocal(timeStr, dateStr);

		// 固定フォーマット部。lvl は3桁・event は6桁に左詰めで揃える。
		char head[40];
		std::snprintf(head, sizeof(head), "%s|%-3.3s|%-6.6s|", timeStr, lvl, event);

		std::string rec(head);
		rec += body;
		rec += '\n';

		std::string dir = osfile::logDir();
		if (dir.empty()) { return; }
		std::string path = dir + "/hg_" + dateStr + ".log";
		osfile::append(path, rec.data(), rec.size());
	}

}

void dataManager::setLogOffset(int utcOffsetMin)
{
	g_logOff = utcOffsetMin;
}

void dataManager::logEvent(const char* event, const char* detail, bool error)
{
	writeRecord(error ? "ERR" : "INF", event ? event : "", detail ? detail : "");
}

std::string dataManager::currentLogPath(void)
{
	char timeStr[20], dateStr[11];
	nowLocal(timeStr, dateStr);
	std::string dir = osfile::logDir();
	if (dir.empty()) { return ""; }
	return dir + "/hg_" + dateStr + ".log";
}

void dataManager::logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName,
                          double meteredLinear, int rdyMeteringMs, int rdyShutterMs, int tm0Ms,
                          int offMs, bool rdyOk, bool setOk)
{
	char lumStr[12];
	std::snprintf(lumStr, sizeof(lumStr), "%+.3f", lumStops);
	// detail = ccm名 + 測光輝度(自動補正時のみ)。Y=リニア輝度, ev=中庸グレー(0.18)基準の段差。
	const char* nm = ccmName ? ccmName : "";
	char detail[128];
	int  dn = 0;
	if (meteredLinear > 0.0)
	{
		double ev = std::log2(meteredLinear / 0.18);
		dn = std::snprintf(detail, sizeof(detail), "%s Y=%.4f ev%+.2f", nm, meteredLinear, ev);
	}
	else
	{
		dn = std::snprintf(detail, sizeof(detail), "%s", nm);
	}
	// タイマ方式の計測(offset調整・カメラ性能判断用)。>=0 のときだけ付与。
	//  tm0=tm0処理時間, off=そのコマのoffset, rdy=測光時間(OK/NG), set=露出設定時間(OK/NG)。
	if (dn > 0 && dn < static_cast<int>(sizeof(detail)))
	{
		if (tm0Ms >= 0)                                                 { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " tm0=%dms", tm0Ms); }
		if (offMs >= 0 && dn < static_cast<int>(sizeof(detail)))        { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " off=%dms", offMs); }
		if (rdyMeteringMs >= 0 && dn < static_cast<int>(sizeof(detail))) { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " rdy=%dms(%s)", rdyMeteringMs, rdyOk ? "OK" : "NG"); }
		if (rdyShutterMs  >= 0 && dn < static_cast<int>(sizeof(detail))) { std::snprintf(detail + dn, sizeof(detail) - dn, " set=%dms(%s)", rdyShutterMs, setOk ? "OK" : "NG"); }
	}
	// SHOT のみ frame〜lum を使う。body を列整形(frame|iso|ss|fn|lum|detail)。露出はカメラ設定値の文字列。
	char body[208];
	std::snprintf(body, sizeof(body), "%5d|%5s|%-11s|%-6s|%8s|%s",
	              frame, e.iso.c_str(), e.ss.c_str(), e.fn.c_str(), lumStr, detail);
	writeRecord("INF", "SHOT", body);
}
