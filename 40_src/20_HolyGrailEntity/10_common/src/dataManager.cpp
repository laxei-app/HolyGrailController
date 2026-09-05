#include "osClock.h"	// 出荷時/新規の場所のタイムゾーン既定値
#include "common.h"
#include "dataManager.h"
#include "secret.h"
#include "httpAuth.h"
#include "osFile.h"
#include "csJson.h"
#include "device.h"
#include "exposureMath.h"
#include "cameraData.h"	// 登録時にカメラからISO/SSを取る
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

	// 露出限界の呼称: limitBright = 暗所限界(高ISO・長SS・開放側) / limitDark = 明所限界(低ISO・高速SS・絞り側)。
	// 出荷時は全種別とも 暗所限界 iso1600/ss8/fn1.4 〜 明所限界 iso100/ss1/4000/fn16、基準(initial)=暗所限界。
	const hgc::exposure limDarkPlace  { "1600", "8",      "1.4" };	// 暗所限界
	const hgc::exposure limBrightPlace{ "100",  "1/4000", "16"  };	// 明所限界

	auto night = std::make_shared<hgc::ccmNight>();
	night->name = "night";
	night->sunAltitude = -18.0;
	night->limitBright = night->limitDark = limDarkPlace;	// 固定露出(3.2): iso1600 / ss8 / fn1.4
	night->initial = night->limitBright;	// 夜間の基準=固定露出(=暗所限界)
	night->preNightEv  = 0.0;	// 夜間前露出補正
	night->postNightEv = 0.0;	// 夜間後露出補正
	set.night = night;

	auto sunrise = std::make_shared<hgc::ccmSunrise>();
	sunrise->name = "sunrise";
	sunrise->sunAltitude    = -6.0;	// 撮り始め(市民薄明)
	sunrise->sunAltitudeEnd =  0.0;	// 終わり(日の出)
	sunrise->ev = -3.0;
	sunrise->limitBright = limDarkPlace;
	sunrise->limitDark   = limBrightPlace;
	sunrise->initial = sunrise->limitBright;	// 基準=暗所限界
	sunrise->hysteresis = 0.3; sunrise->movingAverage = 3;	// 朝日は急変するので個別平滑化(§7)
	set.sunrise = sunrise;

	auto sunset = std::make_shared<hgc::ccmSunset>();
	sunset->name = "sunset";
	sunset->sunAltitude    =  0.0;	// 撮り始め(日の入り)
	sunset->sunAltitudeEnd = -6.0;	// 終わり(市民薄明)
	sunset->ev = -3.0;
	sunset->limitBright = limDarkPlace;
	sunset->limitDark   = limBrightPlace;
	sunset->initial = sunset->limitBright;	// 基準=暗所限界(朝日と同じ設定)
	sunset->hysteresis = 0.3; sunset->movingAverage = 3;	// 夕日は急変するので個別平滑化(§7)
	set.sunset = sunset;

	auto day = std::make_shared<hgc::ccmDay>();
	day->name = "day";
	day->ev = 0.0;
	day->limitBright = limDarkPlace;
	day->limitDark   = limBrightPlace;
	day->initial = day->limitBright;	// 基準=暗所限界
	set.day = day;

	return set;
}

// ============================================================================
//  撮影制御方法の初期値(/asset/ccmDefaults.json。仕様書43 §7.6)
// ============================================================================
namespace
{
	astro::ccmSet                 g_ccmDefaults;
	bool                          g_defaultsLoaded = false;

	// JSON文字列を ccmSet(night/sunrise/sunset/day)へ復元する。4種揃わなければ失敗(false)。
	bool parseDefaults(const std::string& s, astro::ccmSet& set)
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
		return set.night && set.sunrise && set.sunset && set.day;
	}

	void ensureLoaded(void)
	{
		if (g_defaultsLoaded) { return; }
		g_defaultsLoaded = true;
		// 撮影制御方法の初期値は参照専用。ファイルを持たずコード上の出荷時設定から取得する。
		g_ccmDefaults = dataManager::factoryCcmSet();
		}
}

astro::ccmSet dataManager::currentCcmSet(void)
{
	ensureLoaded();
	return g_ccmDefaults;
}

std::string dataManager::ccmSetToJson(const astro::ccmSet& set)
{
	json j;
	j["version"] = 1;
	if (set.night)   { j["night"]   = json::parse(csjson::ccmToJson(*set.night)); }
	if (set.sunrise) { j["sunrise"] = json::parse(csjson::ccmToJson(*set.sunrise)); }
	if (set.sunset)  { j["sunset"]  = json::parse(csjson::ccmToJson(*set.sunset)); }
	if (set.day)     { j["day"]     = json::parse(csjson::ccmToJson(*set.day)); }
	return j.dump();
}

bool dataManager::parseCcmSetJson(const std::string& jsonStr, astro::ccmSet& set)
{
	return parseDefaults(jsonStr, set);
}

std::string dataManager::ccmDefaultsJson(void)
{
	ensureLoaded();
	return ccmSetToJson(g_ccmDefaults);
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
	// tpl_<id>.json のフルパス(撮影計画ひな形)。中身は計画とまったく同じ形。
	std::string tplFilePath(const std::string& id)
	{
		std::string d = osfile::dir("plan");
		return d.empty() ? std::string() : (d + "/tpl_" + id + ".json");
	}
}

bool dataManager::savePlanFile(const std::string& id, const std::string& wrappedJson)
{
	std::string p = planFilePath(id);
	if (p.empty() || id.empty()) { return false; }
	return osfile::writeAll(p, wrappedJson.data(), wrappedJson.size());
}

bool dataManager::saveTplFile(const std::string& id, const std::string& wrappedJson)
{
	std::string p = tplFilePath(id);
	if (p.empty() || id.empty()) { return false; }
	return osfile::writeAll(p, wrappedJson.data(), wrappedJson.size());
}

bool dataManager::loadTplFile(const std::string& id, std::string& out)
{
	std::string p = tplFilePath(id);
	return !p.empty() && !id.empty() && osfile::readAll(p, out);
}

bool dataManager::deleteTplFile(const std::string& id)
{
	if (id.empty()) { return false; }
	return osfile::removeFile("plan", "tpl_" + id + ".json");
}

std::vector<std::string> dataManager::listTplIds(void)
{
	std::vector<std::string> ids;
	for (const std::string& n : osfile::listFiles("plan", "tpl_", ".json"))
	{
		ids.push_back(n.substr(4, n.size() - 4 - 5));	// 前"tpl_"(4) 後".json"(5)
	}
	std::sort(ids.begin(), ids.end());
	return ids;
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

// (廃止 2026-07-05) saveCameraHost/loadCameraHost(cameraHosts.json 直近カメラIPキャッシュ)は削除。
// 既知IP直結フォールバックの廃止(誤接続防止)に伴い不要。カメラ発見は M-SEARCH(再送化済み)のみ。

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

// 出荷時設定の露出平滑化(データ構造仕様書43 §5.10 の出荷時設定)。
hgc::exposureSmoothing dataManager::factorySmoothing(void)
{
	hgc::exposureSmoothing s;	// 既定値がそのまま出荷時設定(hysteresis=1.0, movingAverage=5)
	return s;
}

// 固定撮影計画の出荷時設定部分(場所=東京・機材=EOS R10 + 16mm 等)。
// 出荷時の機材(EOS R10 + SIGMA 12mm)。**機材マスタと同じ値**にする。
//  ここは2つの用途で使う。食い違うと後から必ず混乱するので、1か所にまとめて両方から呼ぶ。
//   ・固定撮影計画の初期値(factoryFixedPlan)
//   ・マスタが読めないときの代わり(masterFallback)
//  【meterLv と一覧を落とさないこと(2026-08-27)】以前ここは maker/model/センサー寸法しか
//   埋めておらず、iso/ss の一覧が空で meterLv も false だった。R10 はサムネイル取得が
//   電源投入あたり200回程度で止まる機種なので、false のままだと一晩持たない。
static void factoryCamera(hgc::camera& c)
{
	c.maker = "Canon";
	c.model = "EOS R10";
	c.name  = "EOS R10";
	c.sensorSize  = 22.3;	// センサー横[mm]
	c.sensorSizeV = 15.0;	// センサー縦[mm]
	c.sensorPixel = 6000;
	c.sensorPixelV = 4000;
	c.meterLv     = true;	// ライブビュー主体で測る機種か
	c.isoList = {
		"100", "125", "160", "200", "250", "320", "400", "500",
		"640", "800", "1000", "1250", "1600", "2000", "2500", "3200",
		"4000", "5000", "6400", "8000", "10000", "12800", "16000", "20000",
		"25600", "32000"
	};
	c.ssList = {
		"1/4000", "1/3200", "1/2500", "1/2000", "1/1600", "1/1250",
		"1/1000", "1/800", "1/640", "1/500", "1/400", "1/320",
		"1/250", "1/200", "1/160", "1/125", "1/100", "1/80",
		"1/60", "1/50", "1/40", "1/30", "1/25", "1/20",
		"1/15", "1/13", "1/10", "1/8", "1/6", "1/5",
		"1/4", "1/3", "0.4", "0.5", "0.6", "0.8",
		"1", "1.3", "1.6", "2", "2.5", "3.2",
		"4", "5", "6", "8", "10", "13",
		"15", "20", "25", "30", "Bulb"
	};
}

static void factoryLens(hgc::lens& l)
{
	l.maker       = "Sigma";
	l.name        = "12mm F1.4 DC DN | Contemporary";	// マスタと同じ綴りにすること(名前で引けなくなる)
	l.focalLength = 12.0;
	l.fn          = 1.4;
	l.fnMax       = 16.0;
	l.hasContact  = true;
	l.fisheye     = false;
}

// 出荷時の場所。**固定計画と場所リストの両方がここを見る**(定義を1つにする)。
//  名前を ASCII にしているのは Entity に日本語を置かない決まりのため(f6a4e8d)。
//  タイムゾーンは端末の現在値。ファイルに固定値を焼くと、どこで使い始めても同じ値になる。
static hgc::place factoryPlace(void)
{
	hgc::place q;
	q.name      = "Tokyo";
	q.latitude  = 35.681;
	q.longitude = 139.767;
	q.altitude  = 40.0;
	q.tzOffMin  = osclock::utcOffsetMin();
	return q;
}

void dataManager::factoryFixedPlan(hgc::cs& plan)
{
	plan.name = "FixedPlan";

	plan.place = factoryPlace();	// 場所リストの種と同じ定義を使う(2026-09-03)

	factoryCamera(plan.camera);
	factoryLens(plan.lens);


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
		hgc::camera c; factoryCamera(c);
		hgc::lens   l; factoryLens(l);
		g_masterCameras.clear(); g_masterCameras.push_back(std::move(c));
		g_masterLenses.clear();  g_masterLenses.push_back(std::move(l));
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

	// 下で定義する。ensureOwned から使うので前に宣言だけ置く。
	const hgc::camera* findMasterCamera(const std::string& name);
	bool saveOwnedCameras(void);

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

		// センサー縦[pixel]は後から足した項目なので、それ以前に登録したカメラには入っていない。
		//  機材マスタに同じ型番があれば埋める(横[pixel]やセンサー寸法と同じ扱い)。1回埋めたら保存する。
		//  findMasterCamera がマスタを読み込むので、ここで呼んでよい。
		bool filled = false;
		for (auto& oc : g_ownedCameras)
		{
			if (oc.cam.sensorPixelV != 0) { continue; }
			const hgc::camera* m = findMasterCamera(oc.cam.model.empty() ? oc.cam.name : oc.cam.model);
			if (m == nullptr) { m = findMasterCamera(oc.cam.name); }
			if (m != nullptr && m->sensorPixelV != 0) { oc.cam.sensorPixelV = m->sensorPixelV; filled = true; }
		}
		if (filled) { saveOwnedCameras(); }
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

	// --- 撮影場所(/asset/places.json。§5.1/§7.9) ---
	std::vector<hgc::place> g_places;
	bool                    g_placesLoaded = false;

	// 全体設定(settings.json)。実体の初期化/保存は下の設定セクションで定義するが、場所の読み込み時に
	// 「自動挿入する場所」(項目10)の移行を行うため、変数と関数をここで先に宣言する。
	extern json g_settings;
	void ensureSettings(void);
	bool saveSettings(void);

	std::string placesPath(void)
	{
		std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/places.json");
	}
	bool savePlaces(void);	// 初回の種を書き出すため前方宣言(定義は下)
	void ensurePlaces(void)
	{
		if (g_placesLoaded) { return; }
		g_placesLoaded = true;
		std::string body;
		std::string p = placesPath();
		// 【初回起動だけ種を置く(2026-09-03)】撮影場所リストが空だと「登録済みの場所から選択」が
		//  何も出さず、固定計画の場所とも食い違う。ファイルが無いとき=一度も起動していないときだけ
		//  1件作る。**「読めたが空」は作らない**(ユーザーが全部消した状態を勝手に復活させない)。
		//  読めたファイルが壊れていたときも作らない(種で上書きしてしまう方が危ない)。
		//  保存先がまだ使えない(placesPath が空。エッジのSD未マウント等)ときも作らない。
		const bool existed = (!p.empty() && osfile::readAll(p, body));
		if (existed) { csjson::placesFromJson(body, g_places); }
		else if (!p.empty()) { g_places.push_back(factoryPlace()); savePlaces(); }
		// 項目10の移行: 旧形式(places.json の autoInsert が場所ごと。複数trueになり得た)から、
		// 全体設定(settings.json の "autoInsertPlace" に名称1つ)へ一度だけ移す。設定が未設定のときのみ、
		// 最初に autoInsert=true だった場所を採用する(複数あっても1つに収束させる)。
		ensureSettings();
		if (!g_settings.contains("autoInsertPlace"))
		{
			std::string first;
			for (const auto& q : g_places) { if (q.autoInsert) { first = q.name; break; } }
			g_settings["autoInsertPlace"] = first;	// 該当なしなら空
			saveSettings();
		}
	}
	bool savePlaces(void)
	{
		std::string p = placesPath();
		if (p.empty()) { return false; }
		std::string s = csjson::placesToJson(g_places);
		return osfile::writeAll(p, s.data(), s.size());
	}
	// 重複しない場所名を作る(「新しい場所」「新しい場所2」…)。
	std::string uniquePlaceName(const std::string& base)
	{
		bool taken = false;
		for (const auto& p : g_places) { if (p.name == base) { taken = true; break; } }
		if (!taken) { return base; }
		for (int i = 2; i < 1000; ++i)
		{
			std::string cand = base + std::to_string(i);
			bool used = false;
			for (const auto& p : g_places) { if (p.name == cand) { used = true; break; } }
			if (!used) { return cand; }
		}
		return base;
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
			return m;
		}
		// メーカー名が model 先頭と完全一致しない場合(例 UPnPは"Canon"だがCCAPI deviceinformation は
		// "Canon.Inc")、双方の先頭英数字ラン(区切り文字直前まで)が一致すれば、その1語を型番から除く。
		auto alnumRun = [](const std::string& s) -> size_t {
			size_t i = 0;
			while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') ||
			                        (s[i] >= '0' && s[i] <= '9'))) { ++i; }
			return i;
		};
		if (!maker.empty())
		{
			size_t mk = alnumRun(maker);
			size_t md = alnumRun(m);
			if (mk > 0 && mk == md && m.compare(0, md, maker, 0, mk) == 0)
			{
				m.erase(0, md);
				while (!m.empty() && (m.front() == ' ' || m.front() == '\t')) { m.erase(0, 1); }
			}
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

		// カメラが申告する設定可能値(ISO/SS)を所持カメラへ入れる。true=1つでも取れた。
	//
	// 【なぜカメラから取るか(2026-08-19)】マスタに無い機種は、これまで似た機種のマスタから
	//  ISO/SSを借りていた。借り物は当たっている保証が無いのに、正しい値のように見える。
	//  ISOとSSはカメラ自身が申告するので、借りずに本人へ聞けばよい。
	//  (センサー寸法と画素数はカメラから取れない。そちらは空のままにして「無い」と分かるようにする)
	bool fillListsFromCamera(const device& dev, hgc::camera& cam)
	{
		if (dev.apiBase == nullptr) { return false; }
		cmdt::shotRange r;
		if (dev.apiBase->getSettings(r) != ERR_HGC_OK) { return false; }
		// "auto" は露出計算に使えないので落とす(Bulb は末尾の特別値として残す)。
		auto pick = [](const std::vector<std::string>& src, std::vector<std::string>& dst)
		{
			dst.clear();
			for (const auto& v : src) { if (!v.empty() && v != "auto" && v != "Auto") { dst.push_back(v); } }
		};
		if (!r.iso.empty()) { pick(r.iso, cam.isoList); }
		if (!r.ss.empty())  { pick(r.ss,  cam.ssList);  }
		return !cam.isoList.empty() || !cam.ssList.empty();
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
		std::string b = base.empty() ? std::string("Camera") : base;
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

// 一覧を読み直す。取り込んだ直後に呼ばれ、次の要求で /master から読み直す。
void dataManager::reloadMaster(void)
{
	g_masterLoaded = false;
	g_masterCameras.clear();
	g_masterLenses.clear();
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

void dataManager::preloadOwned(void)
{
	ensureOwned();
}

std::string dataManager::ownedCameraAuthPass(const std::string& name)
{
	ensureOwned();
	for (const auto& oc : g_ownedCameras) { if (oc.cam.name == name) { return oc.cam.authPass; } }
	return std::string();
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
	// §4a: 同じ機種で「未識別(シリアルも設定名も空)」の所持カメラが既にあれば、もう一台の未識別は
	//      区別できないため追加しない。識別済み(シリアル or 設定名あり)なら2台目の登録を許可する。
	for (const auto& oc : g_ownedCameras)
	{
		bool sameModel = (!oc.cam.model.empty() && !m->model.empty() && oc.cam.model == m->model)
		              || (!oc.cam.name.empty()  && (oc.cam.name == m->name || oc.cam.name == name));
		if (sameModel && oc.cam.serial.empty() && oc.cam.assignedName.empty()) { return false; }
	}
	hgc::ownedCamera oc;
	oc.cam = *m;
	if (oc.cam.model.empty()) { oc.cam.model = m->name; }	// 機種照合の基準
	oc.cam.name = uniqueOwnedName(oc.cam.name);	// 2台目以降は名称を一意化(リストのキー)
	// 項目D: 愛称(assignedName=カメラ本体で付けたニックネーム)とシリアルは勝手に入れない。カメラがオンラインになりSSDPで取得できてから
	//  設定する(それまでは空=UIでは「未定義」と表示)。マスタに値があっても登録時は空にする。
	oc.cam.assignedName.clear();
	oc.cam.serial.clear();
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

// マスタに無いレンズを所持レンズへ足す。スマホ内蔵カメラのようにレンズが交換できず、
//  機材マスタにも載らない機材のために使う(諸元は端末が答える)。
//  同じ名前が既にあれば何もしない(利用者が値を直していることがある)。
bool dataManager::addOwnedLens(const hgc::lens& lens)
{
	if (lens.name.empty()) { return false; }
	ensureOwned();
	for (const auto& l : g_ownedLenses) { if (l.name == lens.name) { return false; } }
	g_ownedLenses.push_back(lens);
	return saveOwnedLenses();
}

// 所持カメラの「組み合わせるレンズ」へ1本割り当てる。先頭が初期値になる。
//  レンズを交換できない機材(スマホ内蔵カメラ)で使う。既に入っていれば触らない
//  (利用者が並べ替えたり別のレンズを足していることがある)。
bool dataManager::setOwnedCameraLens(const std::string& camName, const std::string& lensName)
{
	ensureOwned();
	hgc::lens l;
	if (!findOwnedLens(lensName, l)) { return false; }
	for (auto& oc : g_ownedCameras)
	{
		if (oc.cam.name != camName) { continue; }
		for (const auto& e : oc.lensList) { if (e.name == lensName) { return false; } }	// 既にある
		oc.lensList.insert(oc.lensList.begin(), l);	// 先頭=初期値
		return saveOwnedCameras();
	}
	return false;
}

// 所持カメラに割り当てられたレンズの先頭(=初期値)。
bool dataManager::findOwnedCameraDefaultLens(const std::string& camName, hgc::lens& out)
{
	ensureOwned();
	for (const auto& oc : g_ownedCameras)
	{
		if (oc.cam.name == camName && !oc.lensList.empty()) { out = oc.lensList.front(); return true; }
	}
	return false;
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

// ========================================================================
//  撮影場所(§5.1/§7.9)。登録した場所を撮影計画で選択する。
// ========================================================================
// 項目10: 自動挿入は全体設定(名称1つ)が真。places.json の autoInsert には依存せず、UI へ返す JSON では
// 「その場所が指定されているか」を設定から導出して載せる(=チェックが同時に複数入る状態を作れない)。
std::string dataManager::placesJson(void)
{
	ensurePlaces();
	std::vector<hgc::place> v = g_places;
	const std::string want = autoInsertPlaceName();
	for (auto& p : v) { p.autoInsert = (!want.empty() && p.name == want); }
	return csjson::placesToJson(v);
}

bool dataManager::addPlace(const std::string& name)
{
	ensurePlaces();
	// 【新しい場所の既定は出荷時の場所(2026-09-04 UI依頼)】座標 0,0(ギニア湾)から始めると
	//  スケジュールが作れず、標高も 0 のまま。出荷時の場所を丸ごと既定にする。
	//  タイムゾーンも factoryPlace() が**端末の現在値**を入れる(既定の 0=UTC のままにしない)。
	//  スマホは、この後に現在地が取れれば座標と標高を上書きする(取れなければこの値が残る)。
	hgc::place p = factoryPlace();
	p.name = uniquePlaceName(name.empty() ? std::string("New place") : name);
	g_places.push_back(std::move(p));
	return savePlaces();
}

bool dataManager::removePlace(const std::string& name)
{
	ensurePlaces();
	auto it = std::remove_if(g_places.begin(), g_places.end(),
	                         [&](const hgc::place& p) { return p.name == name; });
	if (it == g_places.end()) { return false; }
	g_places.erase(it, g_places.end());
	// 項目10: 自動挿入に指定されていた場所を消したら、全体設定の指定も外す(存在しない名前を残さない)。
	ensureSettings();
	if (g_settings.value("autoInsertPlace", std::string()) == name)
	{
		g_settings["autoInsertPlace"] = "";
		saveSettings();
	}
	return savePlaces();
}

// 項目10: 「撮影計画に自動的に挿入する」場所は全体でただ1つ。場所ごとのデータ(places.json)ではなく
// 全体設定(settings.json の "autoInsertPlace")に「場所の名称」を1つだけ保存する。これにより複数の
// 場所にチェックが入る状態を構造的に作れなくする(以前は places.json の autoInsert が複数trueになれた)。
// autoInsert=false かつ現在の指定がその場所なら解除する。他の場所が指定されている場合は触らない。
bool dataManager::setPlaceAutoInsert(const std::string& name, bool autoInsert)
{
	ensurePlaces();
	ensureSettings();
	bool exists = false;
	for (const auto& p : g_places) { if (p.name == name) { exists = true; break; } }
	if (!exists) { return false; }
	const std::string cur = g_settings.value("autoInsertPlace", std::string());
	if (autoInsert)      { g_settings["autoInsertPlace"] = name; }	// 1つだけ: 上書きで他は自動的に外れる
	else if (cur == name){ g_settings["autoInsertPlace"] = ""; }		// 自分の指定を解除
	else                 { return true; }							// 他の場所が指定中 → 変更なし
	return saveSettings();
}

// 「自動挿入する場所」の名称(未設定なら空)。UI のチェック状態はこれと突き合わせて決める。
std::string dataManager::autoInsertPlaceName(void)
{
	ensureSettings();
	return g_settings.value("autoInsertPlace", std::string());
}

// 場所の詳細(name/memo/latitude/longitude/altitude/autoInsert)を JSON で更新/新規作成する。
// origName 一致を置換、無ければ新規追加。改名時は json の "name" を新名にする。
bool dataManager::setPlaceDetailJson(const std::string& origName, const std::string& jsonStr)
{
	ensurePlaces();
	ensureSettings();
	std::vector<hgc::place> parsed;	// 1件の場所オブジェクトを配列にくるんで既存パーサで復元
	if (!csjson::placesFromJson(std::string("[") + jsonStr + "]", parsed) || parsed.empty()) { return false; }
	hgc::place* dst = nullptr;
	for (auto& p : g_places) { if (p.name == origName) { dst = &p; break; } }
	// 【無い名前へは書かない(2026-09-04 UI依頼)】以前はここで新しい場所を作っていた。
	//  非同期の書き込み(標高・現在地)が改名の後に届くと、宛先が古い名前のままなので
	//  同じ場所がもう1件増え、一覧の選択が別の項目へ移っていた。
	//  この関数は「その名前の場所を直す」用途しかないので、見つからなければ失敗を返す。
	if (!dst) { return false; }
	const bool wantAuto = parsed.front().autoInsert;	// 受け取ったチェック状態(項目10: 保存先は全体設定)
	*dst = parsed.front();
	dst->autoInsert = false;	// places.json 側は真値として使わない(全体設定が真)
	const std::string newName = dst->name;
	if (!savePlaces()) { return false; }
	// 項目10: 自動挿入は全体設定へ。改名時は指定も追従させる(旧名で指定されていたら新名へ付け替え)。
	const std::string cur = g_settings.value("autoInsertPlace", std::string());
	if (wantAuto)                          { g_settings["autoInsertPlace"] = newName; return saveSettings(); }
	if (cur == origName || cur == newName) { g_settings["autoInsertPlace"] = ""; return saveSettings(); }
	return true;
}

bool dataManager::findPlace(const std::string& name, hgc::place& out)
{
	ensurePlaces();
	for (const auto& p : g_places) { if (p.name == name) { out = p; return true; } }
	return false;
}

// 項目10: 自動挿入する場所は全体設定(名称1つ)で決まる。名称が空/該当場所が消えていれば無し。
bool dataManager::autoInsertPlace(hgc::place& out)
{
	ensurePlaces();
	const std::string want = autoInsertPlaceName();
	if (want.empty()) { return false; }
	for (const auto& p : g_places) { if (p.name == want) { out = p; return true; } }
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
	cam.assignedName    = j.value("assignedName", cam.assignedName);
	cam.serial      = j.value("serial", cam.serial);
	cam.sensorSize  = j.value("sensorSize", cam.sensorSize);
	cam.sensorSizeV = j.value("sensorSizeV", cam.sensorSizeV);
	cam.sensorPixel = j.value("sensorPixel", cam.sensorPixel);
	cam.sensorPixelV = j.value("sensorPixelV", cam.sensorPixelV);
	cam.meterLv     = j.value("meterLv", cam.meterLv);	// ライブビューで測光する(機体ごとの上書き)
	cam.authUser    = j.value("authUser", cam.authUser);	// ダイジェスト認証(空=認証なしの機体)
	// UI からは平文で届く。空文字なら「変更なし」ではなく「消した」なので、キーの有無で見る。
	if (j.contains("authPass")) { cam.authPass = secret::decrypt(j.value("authPass", std::string())); }
	httpAuth::addCandidate(cam.authUser, cam.authPass);	// 編集直後から 401 に対応できるように

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
	json g_settings;	// 前方宣言(場所セクション)に対応する実体
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
// 走行前に確保しておきたい空き容量[バイト]。
// 【2026-08-17 改定 768KB -> 1152KB】旧値の根拠「222バイト/コマ」は測光するコマが全体の
//  1/4程度という前提で、今の構成に合っていなかった。実測しなおすと
//    SHOT 171.7B/行(毎コマ) / LVHIST 213.6B/行(測光したコマ) / BATT 67.6B/行
//  で、17:00-06:00(13時間・15秒周期=3120コマ・うち測光1096コマ)だと **約800KB** になる。
//  768KB では足りず、翌日の明け方に書けなくなる(2026-08-08 に 03:45 で止まった件と同じ形)。
//  StickS3 の spiffs 区画は 1536KB なので、1152KB を確保しても古いログ用に約300KB残る。
//  ログはスマホへ回収する運用なので、古い世代より走行中の記録を優先する。
//  SD 採用機(CoreS3, GB級)はこの条件を常に満たすので挙動は変わらない。
static constexpr unsigned long long kLogKeepFreeBytes = 1152ULL * 1024ULL;

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
	std::sort(others.begin(), others.end());	// 名前=日付昇順 → 先頭が最古
	int    removed = 0;
	size_t i       = 0;

	// ① 件数の上限(従来): 当日以外は最新4件まで残す。
	const size_t keep = 4;
	for (; i + keep < others.size(); ++i)
	{
		if (osfile::removeLog(others[i])) { ++removed; }
	}

	// ② 容量の下限(2026-08-08 追加): 空きが足りなければ、足りるまで古い順に消す。
	//
	// 【なぜ件数だけでは足りないか】M5StickS3 は microSD が無く内蔵フラッシュ(LittleFS)に
	//  書く。spiffs パーティションは default_8MB.csv で 0x180000 = 1536KB しかない。
	//  一方、当日以外を4世代残す①の方針では、最近のログが1日400〜660KBあるため
	//  4世代だけで 1130KB を占め、当日ぶんに 400KB 弱しか残らない。
	//  実測(2026-08-08 朝R100): 03:45 に書き込みが止まり、05:30 までの 1時間45分ぶんの
	//  ログ(SHOT/LVHIST/BATT すべて)が失われた。撮影自体は840枚すべて正常だったので、
	//  記録だけが黙って消えた。件数は上限内(4件)だったため①では一度も削除されていない。
	//
	// 【どれだけ空けるか】実測 222バイト/コマ(SHOT+LVHIST)。15秒周期の一晩12時間 = 2880コマ
	//  で約640KB。これに余裕を見て 768KB を「走行前に確保しておきたい空き」とする。
	//  SD 採用時(GB級)はこの条件を常に満たすので①だけが効き、従来と挙動は変わらない。
	//
	// 新しいログのほうが価値が高いので、消す順は必ず古い側からにする。当日のログは
	// 走行中の記録そのものなので対象にしない(消しても空けたいのは当日ぶんの書き込み先)。
	unsigned long long total = 0, used = 0;
	if (osfile::spaceInfo(total, used))
	{
		for (; i < others.size(); ++i)
		{
			const unsigned long long freeB = (total > used) ? (total - used) : 0ULL;
			if (freeB >= kLogKeepFreeBytes) { break; }		// 十分空いた
			if (osfile::removeLog(others[i])) { ++removed; }
			if (!osfile::spaceInfo(total, used)) { break; }	// 削除のたび測り直す
		}
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
	const char* kPresetTypes[4] = { "night", "sunrise", "sunset", "day" };
	// 種まきプリセットの名前(型ごと)。全型「標準」だと重複名回避で「標準1」等になり、
	// スケジュールやログ(CCMSW)でどの制御方法か分からなかった(2026-07-26 ユーザー指示で型別名へ)。
	const char* presetSeedName(const std::string& t)
	{
		if (t == "night")   { return "Nightscape"; }
		if (t == "sunrise") { return "Sunrise"; }
		if (t == "sunset")  { return "Sunset"; }
		if (t == "day")     { return "Daylight"; }
		return "Standard";
	}

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

		// 種まき: 型ごとに最低1件。撮影制御方法の初期値(ccmDefaults)から型別名(星景/朝日/夕日/日中/月)で作る。
		json def = json::parse(dataManager::ccmDefaultsJson(), nullptr, false);
		ensureSettings();
		bool changed = false, settingsChanged = false;
		if (!g_settings.contains("preferredCcm") || !g_settings["preferredCcm"].is_object())
		{
			g_settings["preferredCcm"] = json::object();
		}
		for (const char* t : kPresetTypes)
		{
			const char* seedName = presetSeedName(t);
			if (!g_presets.contains(t) || !g_presets[t].is_array() || g_presets[t].empty())
			{
				json one = (def.is_object() && def.contains(t)) ? def[t] : json::object();
				one["name"] = seedName;
				g_presets[t] = json::array({ one });
				changed = true;
				if (!g_settings["preferredCcm"].contains(t)) { g_settings["preferredCcm"][t] = seedName; settingsChanged = true; }
			}
			// 既に1件でもあるなら触らない。旧バージョンの名前を改名する移行処理は廃止した
			//  (2026-08-22。リリース前なので過去データの互換は考えない。日本語の照合値を
			//   コードに残さないため)。
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

// 撮影計画の新規作成に使う撮影制御方法一式(項目14)。型ごとに「優先的な初期値にする」で指定された
// プリセットを採用し、指定が無い/そのプリセットが見つからない型はソフトウェア内蔵の初期値
// (factoryCcmSet)で埋める(初回インストール直後などプリセット未整備でも必ず成立する)。
std::string dataManager::preferredCcmSetJson(void)
{
	ensurePresets();	// 種まき(「標準」+ preferredCcm)もここで済む
	json def = json::parse(ccmDefaultsJson(), nullptr, false);
	json out = json::object();
	for (const char* t : kPresetTypes)
	{
		json chosen;
		const std::string want = preferredCcmName(t);	// 優先指定の名前(空=未指定)
		if (!want.empty() && g_presets.contains(t) && g_presets[t].is_array())
		{
			for (const auto& e : g_presets[t])
			{
				if (e.is_object() && e.value("name", std::string()) == want) { chosen = e; break; }
			}
		}
		if (!chosen.is_object() && def.is_object() && def.contains(t)) { chosen = def[t]; }	// 内蔵初期値へフォールバック
		if (chosen.is_object()) { out[t] = chosen; }
	}
	return out.dump();
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
	// 【無い名前へは書かない(2026-09-04 UI依頼)】撮影場所と同じ理由。改名の後に届いた
	//  書き込みが古い名前を宛先にすると、同じレンズがもう1件増えて一覧の選択が動く。
	//  追加は addOwnedLens が行うので、ここで作る必要はない。
	//  ※所持カメラの方は手入力の追加でこの経路を使っているので、あちらは作れるままにする。
	if (!lp) { return false; }

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
	return recordConnectedCameraStatus(dev, true) >= 0;	// 従来互換: 新規は自動追加
}

int dataManager::recordConnectedCameraStatus(const device& dev, bool allowAdd)
{
	ensureOwned();
	// device.model 例 "Canon EOS R10"。所持/マスタは型番のみ("EOS R10")なのでメーカー名を除いて照合。
	std::string key = stripMaker(dev.model, dev.manufacturer);

	// 1) シリアル一致の所持カメラ → 同一個体。設定名(assignedName)のみ更新する(serialは識別子なので変えない)。
	if (!dev.serialno.empty())
	{
		for (auto& oc : g_ownedCameras)
		{
			if (!oc.cam.serial.empty() && oc.cam.serial == dev.serialno)
			{
				bool changed = false;
				if (!dev.assignedName.empty() && oc.cam.assignedName != dev.assignedName)
				{ oc.cam.assignedName = dev.assignedName; changed = true; }
				// 【型番のずれを直す(2026-08-19)】シリアルが同じなら同一個体なので、
				//  型番はカメラの申告が正しい。過去に部分一致で別機種として登録された個体
				//  ("EOS R50 V" が "EOS R50" になった)を、次に見つけたときに正す。
				//  名前はリストのキーでユーザーが変えられるため、自動で付いたまま
				//  (=旧型番と同じ)のときだけ合わせる。
				if (!key.empty() && oc.cam.model != key)
				{
					const bool autoName = (oc.cam.name == oc.cam.model);
					logEvent("GEAR", (oc.cam.model + " renamed to " + key + " (S/N " + dev.serialno + ")").c_str());
					oc.cam.model = key;
					if (autoName) { oc.cam.name = uniqueOwnedName(key); }
					changed = true;
				}
				// 【ISO/SSが空なら、いま繋がっているカメラから埋める(2026-08-19)】
				//  登録の瞬間には取れないことがある。認証が要る機体は、ユーザーが
				//  ユーザーID/パスワードを入れるまでCCAPIを読めないため(実測 EOS R50 V の1台)。
				//  撮影で繋いだときは認証を通っているので、そこで埋まる。
				if ((oc.cam.isoList.empty() || oc.cam.ssList.empty()) && fillListsFromCamera(dev, oc.cam))
				{
					logEvent("GEAR", (oc.cam.model + " iso/ss taken from camera (S/N " + dev.serialno + ")").c_str());
					changed = true;
				}
				// 【内蔵カメラは並びを毎回引き直す(2026-09-06)】並びは端末の実力から合成するもので、
				//  版で変わる(加算で 48 秒まで伸びた。以前の登録は 8.3 秒止まり)。ユーザーが直す欄でも
				//  ないので、カメラの申告を常に正とする。外付けカメラは従来どおり空のときだけ。
				if (dev.manufacturer == "builtin")
				{
					hgc::camera fresh = oc.cam;
					if (fillListsFromCamera(dev, fresh) &&
					    (fresh.isoList != oc.cam.isoList || fresh.ssList != oc.cam.ssList))
					{
						oc.cam.isoList = fresh.isoList; oc.cam.ssList = fresh.ssList; changed = true;
						logEvent("GEAR", (oc.cam.model + " iso/ss refreshed from device (S/N " + dev.serialno + ")").c_str());
					}
				}
				if (changed) { saveOwnedCameras(); }
				return static_cast<int>(camApply::updated);
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
			if (!dev.assignedName.empty()) { oc.cam.assignedName = dev.assignedName; }
			if (oc.cam.isoList.empty() || oc.cam.ssList.empty()) { fillListsFromCamera(dev, oc.cam); }
			saveOwnedCameras();
			return static_cast<int>(camApply::filled);
		}
	}

	// 3) 同機種は全て別シリアルで確定済み(またはモデル不一致) → 新規の別個体。
	//    allowAdd=false(裏の発見)なら追加せず「新規」だけ返し、登録可否を呼び手(UI)に委ねる。
	if (!allowAdd) { return static_cast<int>(camApply::isNew); }

	//    §4a: 既存が識別済みなら2台目の登録は可。device は実機なのでシリアルを持ち、未識別の重複は作らない。
	hgc::ownedCamera oc;
	// 【マスタに無い機種は借り物をしない(2026-08-19)】以前はマスタ照合が完全一致に外れると
	//  「最長部分一致」に落ち、別機種のマスタを丸ごと使っていた。実測: "EOS R50 V" は
	//  マスタに無く "EOS R50" を掴み、型番も仕様も EOS R50 になった。型番が違うので
	//  撮影開始時の機種照合に通らず、撮影が始まらなかった。
	//  借り物は当たっている保証が無いのに正しい値のように見えるので、やめる。
	//   ・型番/ISO/SS → カメラ本人から取る
	//   ・センサー寸法/画素数 → カメラからは取れないので**空のまま**にする。
	//     間違った値を出すより「無い」と分かるほうがよい(NPFと撮影シミュレーションは
	//     その旨を表示して止まる)。
	const hgc::camera* m = findMasterCamera(key);	// 完全一致のみ
	if (m) { oc.cam = *m; }
	else
	{
		oc.cam.model       = key.empty() ? dev.model : key;
		oc.cam.name        = oc.cam.model;
		oc.cam.maker       = dev.manufacturer;
		oc.cam.sensorSize  = 0.0;	// 未登録(マスタに無い)
		oc.cam.sensorSizeV = 0.0;
		oc.cam.sensorPixel = 0;
		oc.cam.sensorPixelV = 0;
		const bool got = fillListsFromCamera(dev, oc.cam);
		logEvent("GEAR", (key + " not in gear master: sensor size unknown, iso/ss " +
		                  (got ? "from camera" : "unavailable")).c_str());
	}
	if (oc.cam.model.empty()) { oc.cam.model = key.empty() ? dev.model : key; }	// 機種照合の基準(名称は一意化で変わるため model を確実に持たせる)
	if (oc.cam.name.empty()) { oc.cam.name = dev.assignedName.empty() ? (key.empty() ? dev.model : key) : dev.assignedName; }
	oc.cam.name     = uniqueOwnedName(oc.cam.name);	// 同機種2台目以降は名称を一意化(リストのキー)
	oc.cam.serial   = dev.serialno;
	oc.cam.assignedName = dev.assignedName;
	g_ownedCameras.push_back(std::move(oc));
	saveOwnedCameras();
	return static_cast<int>(camApply::isNew);
}

// §4b: 計画カメラの assignedName から所持リストを引き、実シリアルを解決する(接続済みなら serial が入っている)。
bool dataManager::serialForAssignedName(const std::string& assignedName, std::string& outSerial)
{
	if (assignedName.empty()) { return false; }
	ensureOwned();
	for (const auto& oc : g_ownedCameras)
	{
		if (oc.cam.assignedName == assignedName && !oc.cam.serial.empty()) { outSerial = oc.cam.serial; return true; }
	}
	return false;
}

// §4b: 所持カメラに登録済みのシリアル一覧(空は除く)。「それ以外のカメラ」判定に使う。
void dataManager::ownedCameraSerials(std::vector<std::string>& out)
{
	ensureOwned();
	for (const auto& oc : g_ownedCameras) { if (!oc.cam.serial.empty()) { out.push_back(oc.cam.serial); } }
}

// 撮影画像から読めたセンサー諸元を所持カメラへ入れる。既に値があるものは触らない。
//  機材マスターに無い機種はセンサー寸法/画素数が空のままで、NPFも撮影シミュレーションも
//  出せない。カメラのAPIからは取れないが撮影画像のEXIFには入っているので、撮り始めたら埋める。
bool dataManager::fillOwnedCameraSensor(const std::string& serial, double sensorWmm, double sensorHmm,
                                        uint32_t pixelW, uint32_t pixelH)
{
	if (serial.empty() || sensorWmm <= 0.0 || pixelW == 0) { return false; }
	ensureOwned();
	for (auto& oc : g_ownedCameras)
	{
		if (oc.cam.serial != serial) { continue; }
		bool changed = false;
		if (oc.cam.sensorSize  <= 0.0 && sensorWmm > 0.0) { oc.cam.sensorSize  = sensorWmm; changed = true; }
		if (oc.cam.sensorSizeV <= 0.0 && sensorHmm > 0.0) { oc.cam.sensorSizeV = sensorHmm; changed = true; }
		if (oc.cam.sensorPixel == 0   && pixelW    > 0)   { oc.cam.sensorPixel = pixelW;    changed = true; }
		if (oc.cam.sensorPixelV == 0  && pixelH    > 0)   { oc.cam.sensorPixelV = pixelH;   changed = true; }
		if (!changed) { return false; }
		saveOwnedCameras();
		char msg[160];
		std::snprintf(msg, sizeof(msg), "%s sensor from captured image %.2f x %.2f mm / %u x %u px (S/N %s)",
		              oc.cam.model.c_str(), oc.cam.sensorSize, oc.cam.sensorSizeV,
		              static_cast<unsigned>(oc.cam.sensorPixel), static_cast<unsigned>(oc.cam.sensorPixelV), serial.c_str());
		logEvent("GEAR", msg);
		return true;
	}
	return false;
}

// §4b: 同じ機種として登録されている所持カメラの台数(個体が確定しているものだけ数える)。
//  2台以上あると「同機種の空き1台」で代替できない(どちらのつもりか決められない)。
int dataManager::ownedCountForModel(const hgc::camera& cam)
{
	ensureOwned();
	int n = 0;
	for (const auto& oc : g_ownedCameras)
	{
		if (oc.cam.serial.empty()) { continue; }		// 未確定は個体として数えない
		if (!cam.model.empty() && oc.cam.model == cam.model) { ++n; }
	}
	return n;
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

// デバッグログの取捨。説明はヘッダ。**既定は両方とも採らない**。
namespace { bool g_logShot = false; bool g_logBatt = false; bool g_logSys = false; }

void dataManager::setLogOptions(bool shot, bool batt, bool sys)
{ g_logShot = shot; g_logBatt = batt; g_logSys = sys; }
bool dataManager::logShotEnabled(void) { return g_logShot; }
bool dataManager::logBattEnabled(void) { return g_logBatt; }
bool dataManager::logSysEnabled(void)  { return g_logSys; }

void dataManager::setLogOffset(int utcOffsetMin)
{
	g_logOff = utcOffsetMin;
}

void dataManager::logEvent(const char* event, const char* detail, bool error)
{
	writeRecord(error ? "ERR" : "INF", event ? event : "", detail ? detail : "");
}

// 撮影結果レポートをログと同じディレクトリへ JSON で書く(1撮影=1ファイル)。
//
// JSON にしたのは、UI(撮影レポート画面)がそのまま読んで表示するため。所見は文言ではなく
// noteCode の数値で残す(表示する日本語は UI 側が持つ)。ファイル形式を文言変更から切り離す。
//
// この撮影で「機材/設定に無理が無かったか」を判断する材料を残すことが目的で、要は
//  ・撮れなかった/露出を当てられなかったコマがどれだけあったか
//  ・撮影周期をどこまでシャッター速度へ近づけられるか(busy と準備の実測)
// の2点である。
std::string dataManager::writeCaptureReport(const captureReport& r, const hgc::cs& plan, const char* planId)
{
	std::string dir = osfile::logDir();
	if (dir.empty()) { return ""; }
	char timeStr[20], dateStr[11];
	nowLocal(timeStr, dateStr);	// timeStr は "YYYY-MM-DD HH:MM:SS"(日付込み19文字)、dateStr は "YYYY-MM-DD"
	// report_<planId>_<日付>_<HHMMSS>.json。同じ計画を撮り直しても上書きしないよう時刻まで入れる。
	// ファイル名に空白や ':' を入れない(取り出し・スクリプトでの扱いを楽にする)。
	char hhmmss[7] = {0};
	std::snprintf(hhmmss, sizeof(hhmmss), "%c%c%c%c%c%c",
	              timeStr[11], timeStr[12], timeStr[14], timeStr[15], timeStr[17], timeStr[18]);
	std::string path = dir + "/report_" + (planId ? planId : "plan") + "_" + dateStr + "_" + hhmmss + ".json";

	auto pct = [](long n, long d) { return (d > 0) ? (100.0 * static_cast<double>(n) / static_cast<double>(d)) : 0.0; };
	auto avg = [](long sum, int cnt) { return (cnt > 0) ? (static_cast<double>(sum) / cnt) : 0.0; };
	// 実周期 = 最初〜最後のシャッター間隔 ÷ (コマ数-1)。設定周期と比べれば伸びたかが分かる。
	double actual = 0.0;
	if (r.frames > 1 && r.lastShutterMs > r.firstShutterMs)
	{ actual = static_cast<double>(r.lastShutterMs - r.firstShutterMs) / 1000.0 / static_cast<double>(r.frames - 1); }

	// 目安の最短周期 = 最長ss + 準備の最大 + 余裕1秒。
	// 1コマは「シャッター→露光→記録が明ける→測光→露出設定→次のシャッター」の順で進む。
	// 準備(prep)は「記録が明けるのを待つ+測光+露出設定」を丸ごと含むので、露光と足せば
	// 1周に要る時間になる(busy は準備の内数なので二重に足さない)。最大値で見るのは安全側へ倒すため。
	// 準備を1コマも測れていないときは目安を出さない(-1)。憶測の数字を出さない。
	char timeBuf[24];
	std::snprintf(timeBuf, sizeof(timeBuf), "%s", timeStr);
	double minInterval = -1.0;
	if (r.frames > 0 && r.prepMax > 0)
	{ minInterval = r.maxSsSec + (r.prepMax / 1000.0) + 1.0; }

	json j;
	j["version"] = 1;
	j["plan"]    = plan.name;
	j["planId"]  = (planId ? planId : "");
	j["camera"]  = plan.camera.maker + " " + plan.camera.model;
	j["lens"]    = plan.lens.name;
	{
		char ws[20], we[20];
		std::snprintf(ws, sizeof(ws), "%04d-%02d-%02d %02d:%02d",
		              plan.start.year, plan.start.month, plan.start.day, plan.start.hour, plan.start.min);
		std::snprintf(we, sizeof(we), "%04d-%02d-%02d %02d:%02d",
		              plan.end.year, plan.end.month, plan.end.day, plan.end.hour, plan.end.min);
		j["window"] = { { "start", ws }, { "end", we } };
	}
	j["shotAt"] = timeBuf;	// レポートを書いた(=撮影を終えた)日時

	j["capture"] = { { "frames", r.frames }, { "shootFail", r.shootFail },
	                 { "shootFailPct", pct(r.shootFail, r.frames) } };

	// 測光の分母は「測光を試みたコマ」。夜間の固定露出は測光しないので混ぜると失敗率が過大に見える。
	j["exposure"] = { { "meterTried", r.meterTried },
	                  { "meterFail", r.meterFail }, { "meterFailPct", pct(r.meterFail, r.meterTried) },
	                  { "setFail", r.setFail },     { "setFailPct", pct(r.setFail, r.frames) },
	                  { "meterRetryFrames", r.meterRetryFrames },
	                  { "thumbFrames", r.thumbFrames },
	                  { "lvFrames", r.lvFrames },
	                  { "heldFrames", r.heldFrames },
	                  { "applyRetryFrames", r.applyRetryFrames } };

	j["interval"] = { { "setSec", plan.interval }, { "actualSec", actual } };
	// lateFrames/lateOverSumMs = 撮影周期に間に合わず遅れて切ったコマの回数と、その遅れの合計。
	j["timing"]   = { { "lateOk", r.lateOk }, { "lateCnt", r.lateCnt }, { "lateOkPct", pct(r.lateOk, r.lateCnt) },
	                  { "lateAvgMs", avg(r.lateSum, r.lateCnt) }, { "lateMaxMs", r.lateMax },
	                  { "lateFrames", r.lateOverCnt }, { "lateOverSumMs", r.lateOverSumMs },
	                  { "lateOverAvgMs", avg(r.lateOverSumMs, r.lateOverCnt) },
	                  { "prepAvgMs", avg(r.prepSum, r.frames) },  { "prepMaxMs", r.prepMax },
	                  { "prepOver", r.prepOver }, { "leadMs", r.leadMs } };

	// busy = 露光終了からカメラが測光を受け付けるまで(サムネイル測光では登録通知が来るまで)。
	// 周期の下限を決めている本体。
	j["busy"]  = { { "cnt", r.busyCnt }, { "avgMs", avg(r.busySumMs, r.busyCnt) },
	               { "maxMs", r.busyMaxMs }, { "stuck", r.busyStuck } };
	j["meter"] = { { "cnt", r.meterCnt }, { "avgMs", avg(r.meterSumMs, r.meterCnt) }, { "maxMs", r.meterMaxMs } };
	j["apply"] = { { "cnt", r.applyCnt }, { "avgMs", avg(r.applySumMs, r.applyCnt) }, { "maxMs", r.applyMaxMs } };
	// 撮影開始前の初期収束が、うまくいったのか失敗したのか。ここが済んでいないと1枚目から露出が外れる。
	j["converge"] = { { "outcome", r.cvOutcome }, { "steps", r.cvSteps },
	                  { "applyNg", r.cvApplyNg }, { "meterNg", r.cvMeterNg },
	                  { "shots", r.cvShots } };
	j["liveview"] = { { "staleFrames", r.staleFrames }, { "staleTotal", r.staleTotal } };
	j["limit"] = { { "maxSsSec", r.maxSsSec }, { "minIntervalSec", minInterval },
	               { "marginSec", (minInterval >= 0.0) ? (plan.interval - minInterval) : 0.0 } };

	// 所見はコード(noteCode)で残す。閾値を超えたものだけ入れる。表示文言は UI が持つ。
	json notes = json::array();
	if (r.setFail > 0)                                            { notes.push_back(static_cast<int>(NOTE_SET_FAIL)); }
	if (r.staleFrames > 0 && pct(r.staleFrames, r.frames) > 10.0) { notes.push_back(static_cast<int>(NOTE_STALE_MANY)); }
	if (r.lateCnt > 0 && pct(r.lateOk, r.lateCnt) < 90.0)         { notes.push_back(static_cast<int>(NOTE_LATE_MANY)); }
	if (r.meterFail > 0 && pct(r.meterFail, r.meterTried) > 5.0)  { notes.push_back(static_cast<int>(NOTE_METER_FAIL)); }
	// cvOutcome==3 は「収束不要」(固定露出で始まった/直前の撮影露出を引き継いだ)。所見にしない。
	if (r.cvOutcome == 2)                                         { notes.push_back(static_cast<int>(NOTE_CONVERGE_NONE)); }
	else if (r.cvOutcome == 1)                                    { notes.push_back(static_cast<int>(NOTE_CONVERGE_PART)); }
	if (r.busyCnt == 0)                                           { notes.push_back(static_cast<int>(NOTE_BUSY_NO_DATA)); }
	if (minInterval >= 0.0)
	{
		// 余裕が1秒未満なら詰まりすぎ、5秒以上あるならまだ詰められる、という目安。
		const double margin = plan.interval - minInterval;
		if (margin < 1.0)      { notes.push_back(static_cast<int>(NOTE_INTERVAL_TIGHT)); }
		else if (margin > 5.0) { notes.push_back(static_cast<int>(NOTE_INTERVAL_ROOM)); }
	}
	j["notes"] = notes;

	const std::string out = j.dump(1, '\t');
	if (out.empty()) { return ""; }
	if (!osfile::writeAll(path, out.c_str(), out.size())) { return ""; }
	return path;
}

// 撮影レポートのファイル名一覧(中身は読まない)。件数を返すだけの用途でも使うので軽く保つ。
std::vector<std::string> dataManager::reportNames(void)
{
	return osfile::listFiles("log", "report_", ".json");
}

// 撮影レポートの一覧。並びは撮影日時(shotAt)の降順=本当に新しい順。
// ファイル名は report_<planId>_<日付>_<時刻>.json なので、名前で並べると planId が先に効いてしまい
// 計画をまたぐと日時順にならない。中身の shotAt で並べる。
std::string dataManager::reportListJson(void)
{
	std::vector<std::string> names = reportNames();
	std::string dir = osfile::logDir();
	json arr = json::array();
	for (const auto& nm : names)
	{
		// 一覧に出す最小限だけ中身から拾う。壊れたファイルは名前だけで出す(消せるように)。
		json e;
		e["name"] = nm;
		std::string body;
		if (!dir.empty() && osfile::readAll(dir + "/" + nm, body))
		{
			json f = json::parse(body, nullptr, false);
			if (!f.is_discarded() && f.is_object())
			{
				e["plan"]      = f.value("plan", std::string());
				e["camera"]    = f.value("camera", std::string());
				e["edge"]      = f.value("edge", std::string());	// エッジから回収したものだけ入る(空=スマホ直結)
				e["shotAt"]    = f.value("shotAt", std::string());
				e["frames"]    = f.contains("capture") ? f["capture"].value("frames", 0) : 0;
				e["noteCount"] = f.contains("notes") && f["notes"].is_array() ? static_cast<int>(f["notes"].size()) : 0;
			}
		}
		arr.push_back(e);
	}
	// shotAt は "YYYY-MM-DD HH:MM:SS" の固定書式なので文字列比較でそのまま時系列になる。
	// 読めなかったファイル(shotAt 無し)は末尾へ落とす。
	std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
		return a.value("shotAt", std::string()) > b.value("shotAt", std::string());
	});
	return arr.dump();
}

std::string dataManager::reportJson(const std::string& name)
{
	// 名前はファイル名のみを受け取る(パス区切りを含むものは弾く=ディレクトリ外へ出さない)。
	if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) { return ""; }
	std::string dir = osfile::logDir();
	if (dir.empty()) { return ""; }
	std::string body;
	if (!osfile::readAll(dir + "/" + name, body)) { return ""; }
	return body;
}

bool dataManager::removeReport(const std::string& name)
{
	if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) { return false; }
	return osfile::removeFile("log", name);
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
                          double meteredLinear, int rdyMeteringMs, int rdyShutterMs, int prepMs,
                          int lateMs, bool rdyOk, bool setOk, int meterTry, int applyTry,
                          uint32_t histSum, uint64_t lvTimeMs, int staleSkip, uint64_t shutterEpochMs,
                          int busyMs, int firstApplyTries)
{
	if (!g_logShot) { return; }	// 採らない設定(既定)。整形も文字列化もしない
	char lumStr[12];
	std::snprintf(lumStr, sizeof(lumStr), "%+.3f", lumStops);
	// detail = ccm名 + 測光輝度(自動補正時のみ)。Y=リニア輝度, ev=中庸グレー(0.18)基準の段差。
	const char* nm = ccmName ? ccmName : "";
	// hs=/lv=/stale= の追加でdetailが伸びた。128では末尾の sh=(ミリ秒)が切れる実害が出た
	// (2026-07-18: 通し5993行中256行=遅延が大きい行ほど溢れて切れ、解析で遅延を過小評価した)。
	char detail[192];
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
	// タイマ方式の計測(リード調整・カメラ性能判断用)。>=0 のときだけ付与。
	//  rdy=測光時間(OK/NG,試行回数), set=露出設定時間(OK/NG,試行回数),
	//  prep=準備(測光→計算→設定)の合計, late=シャッターの周期からの遅れ(0=ぴったり)。
	if (dn > 0 && dn < static_cast<int>(sizeof(detail)))
	{
		if (rdyMeteringMs >= 0 && dn < static_cast<int>(sizeof(detail)))
		{	dn += std::snprintf(detail + dn, sizeof(detail) - dn, " rdy=%dms(%s,try%d)", rdyMeteringMs, rdyOk ? "OK" : "NG", meterTry); }
		if (rdyShutterMs  >= 0 && dn < static_cast<int>(sizeof(detail)))
		{	dn += std::snprintf(detail + dn, sizeof(detail) - dn, " set=%dms(%s,try%d)", rdyShutterMs, setOk ? "OK" : "NG", applyTry); }
		if (prepMs >= 0 && dn < static_cast<int>(sizeof(detail)))  { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " prep=%dms", prepMs); }
		if (lateMs >= 0 && dn < static_cast<int>(sizeof(detail)))  { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " late=%dms", lateMs); }
		// busy=露光終了からカメラが測光を受け付けるまで[ms]。撮影周期をどこまでSSへ詰められるかの実測値。
		//  stuck = 準備開始までの空白のあいだ明けなかった(下限しか分からない)。
		if (busyMs >= 0 && dn < static_cast<int>(sizeof(detail)))   { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " busy=%dms", busyMs); }
		else if (busyMs == -2 && dn < static_cast<int>(sizeof(detail))) { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " busy=stuck"); }
		// fa=1枚目の露出を何回目でカメラへ適用できたか。1回で通ったときは出さない(常時付くと冗長)。
		// >1 = 適用が一度失敗したが待って乗った(=放置後の初回に出る事象を吸収できた証拠)。
		if (firstApplyTries > 1 && dn < static_cast<int>(sizeof(detail))) { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " fa=%d", firstApplyTries); }
		// hs=測光ヒストグラムの内容チェックサム。前コマと同値なら「カメラが古いフレームを返した
		// (=測光値が1コマ古い)」疑い。alzMetering は中身の鮮度を判別できないのでログで突き合わせる。
		if (histSum != 0 && dn < static_cast<int>(sizeof(detail))) { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " hs=%08x", histSum); }
		// lv=測光したライブビューフレームを「カメラが取得した時刻」(カメラ内蔵時計。sh= とは一定のTZ差)。
		// sh= と突き合わせれば、露光後に撮られた新鮮なフレームか、露光前の古いフレームかが一意に分かる。
		if (lvTimeMs > 0 && dn < static_cast<int>(sizeof(detail)))
		{
			std::time_t lt = static_cast<std::time_t>(lvTimeMs / 1000ULL);
			unsigned    lms = static_cast<unsigned>(lvTimeMs % 1000ULL);
			std::tm     lg{};
#if defined(_WIN32)
			gmtime_s(&lg, &lt);
#else
			gmtime_r(&lt, &lg);
#endif
			dn += std::snprintf(detail + dn, sizeof(detail) - dn, " lv=%02d:%02d:%02d.%03u", lg.tm_hour, lg.tm_min, lg.tm_sec, lms);
		}
		// stale=このコマで「ライブビューが前回の測光から更新されていない(=古い映像)」として捨てた回数。
		// >0 が続く=撮影周期がカメラのライブビュー更新周期に対して短すぎる(周期を伸ばすべき)。
		if (staleSkip > 0 && dn < static_cast<int>(sizeof(detail))) { dn += std::snprintf(detail + dn, sizeof(detail) - dn, " stale=%d", staleSkip); }
		// シャッター投下の実時刻(ms精度)。ログのタイムスタンプ用オフセットでローカル化して sh=HH:MM:SS.mmm。
		if (shutterEpochMs > 0 && dn < static_cast<int>(sizeof(detail)))
		{
			std::time_t st = static_cast<std::time_t>(shutterEpochMs / 1000ULL) + static_cast<std::time_t>(g_logOff) * 60;
			unsigned    sms = static_cast<unsigned>(shutterEpochMs % 1000ULL);
			std::tm     sg{};
#if defined(_WIN32)
			gmtime_s(&sg, &st);
#else
			gmtime_r(&st, &sg);
#endif
			std::snprintf(detail + dn, sizeof(detail) - dn, " sh=%02d:%02d:%02d.%03u", sg.tm_hour, sg.tm_min, sg.tm_sec, sms);
		}
	}
	// SHOT のみ frame〜lum を使う。body を列整形(frame|iso|ss|fn|lum|detail)。露出はカメラ設定値の文字列。
	char body[272];	// 先頭列(約40字) + detail(最大192)
	std::snprintf(body, sizeof(body), "%5d|%5s|%-11s|%-6s|%8s|%s",
	              frame, e.iso.c_str(), e.ss.c_str(), e.fn.c_str(), lumStr, detail);
	writeRecord("INF", "SHOT", body);
}
