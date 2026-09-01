// 撮影計画(cs)・撮影制御方法(ccm)の JSON 相互変換(データ構造仕様書43)。
#include "csJson.h"
#include "secret.h"		// パスワードはファイル・通信ともに暗号化して載せる
#include "httpAuth.h"		// 読み込んだ資格情報はそのまま 401 の候補にする
#include <json/nlohmann/json.hpp>
#include <cctype>

using json = nlohmann::json;

namespace csjson
{
	namespace
	{
		// --- 基本型 ---
		json dtToJson(const hgc::dateTime& d)
		{
			return json{ {"year", d.year}, {"month", d.month}, {"day", d.day},
			             {"hour", d.hour}, {"min", d.min}, {"sec", d.sec} };
		}
		hgc::dateTime dtFromJson(const json& j)
		{
			hgc::dateTime d;
			d.year  = j.value("year", 0);
			d.month = j.value("month", 0);
			d.day   = j.value("day", 0);
			d.hour  = j.value("hour", 0);
			d.min   = j.value("min", 0);
			d.sec   = j.value("sec", 0);
			return d;
		}

		json expToJson(const hgc::exposure& e)
		{
			json j{ {"iso", e.iso}, {"ss", e.ss}, {"fn", e.fn} };
			if (!e.fnWish.empty()) { j["fnWish"] = e.fnWish; }	// 丸めているときだけ出す
			return j;
		}
		// 文字列フィールドを安全に取り出す(型不一致や旧形式の数値でも例外を投げない)。
		std::string getStr(const json& j, const char* key)
		{
			auto it = j.find(key);
			if (it == j.end()) { return std::string(); }
			if (it->is_string()) { return it->get<std::string>(); }
			return std::string();	// 数値等(旧形式)は非互換として無視
		}

		hgc::exposure expFromJson(const json& j)
		{
			hgc::exposure e;
			e.iso = getStr(j, "iso");
			e.ss  = getStr(j, "ss");
			e.fn  = getStr(j, "fn");
			e.fnWish = getStr(j, "fnWish");
			return e;
		}

		json placeToJson(const hgc::place& p)
		{
			return json{ {"name", p.name}, {"memo", p.memo}, {"latitude", p.latitude}, {"longitude", p.longitude},
			             {"altitude", p.altitude}, {"autoInsert", p.autoInsert} };
		}
		hgc::place placeFromJson(const json& j)
		{
			hgc::place p;
			p.name       = j.value("name", std::string());
			p.memo       = j.value("memo", std::string());
			p.latitude   = j.value("latitude", 0.0);
			p.longitude  = j.value("longitude", 0.0);
			p.altitude   = j.value("altitude", 0.0);
			p.autoInsert = j.value("autoInsert", false);
			return p;
		}

		json cameraToJson(const hgc::camera& c)
		{
			return json{ {"maker", c.maker}, {"model", c.model}, {"name", c.name},
			             {"serial", c.serial}, {"assignedName", c.assignedName},
			             {"sensorSize", c.sensorSize}, {"sensorSizeV", c.sensorSizeV},
			             {"sensorPixel", c.sensorPixel}, {"sensorPixelV", c.sensorPixelV},
			             {"isoList", c.isoList}, {"ssList", c.ssList},
			             {"meterLv", c.meterLv},
			             {"authUser", c.authUser},
			             // パスワードは暗号文で載せる。ファイルにも ETP にも平文は出さない
			             //  (エッジも同じ固定鍵を持っているので、そのまま復号できる)。
			             {"authPass", secret::encrypt(c.authPass)} };
		}
		hgc::camera cameraFromJson(const json& j)
		{
			hgc::camera c;
			c.maker       = j.value("maker", std::string());
			c.model       = j.value("model", std::string());
			c.name        = j.value("name", std::string());
			c.serial      = getStr(j, "serial");
			c.assignedName    = getStr(j, "assignedName");
			c.sensorSize  = j.value("sensorSize", 0.0);
			c.sensorSizeV = j.value("sensorSizeV", 0.0);
			c.sensorPixel = j.value("sensorPixel", 0u);
			c.sensorPixelV = j.value("sensorPixelV", 0u);
			if (j.contains("isoList")) { c.isoList = j["isoList"].get<std::vector<std::string>>(); }
			if (j.contains("ssList"))  { c.ssList  = j["ssList"].get<std::vector<std::string>>(); }
			c.meterLv     = j.value("meterLv", false);	// 無い=サムネイルだけ(既定)
			c.authUser    = getStr(j, "authUser");
			c.authPass    = secret::decrypt(getStr(j, "authPass"));	// 平文で手書きされていてもそのまま通る
			// カメラ情報を読む道はここ1本(所持カメラ・撮影計画ファイル・ETP受信のすべて)。
			//  発見の段階ではどの IP がどの機体か分からないので、候補として登録しておく。
			httpAuth::addCandidate(c.authUser, c.authPass);
			return c;
		}

		// 魚眼判定のフォールバック: マスタ/計画に "fisheye" が無い場合はレンズ名で判定する。
		//  データ側(lenses_list.json の "fisheye")が真の情報源。名前判定は未設定時のみ。
		bool isFisheyeName(const std::string& name)
		{
			std::string lo = name;
			for (auto& c : lo) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
			return lo.find("fisheye") != std::string::npos;
		}
		json lensToJson(const hgc::lens& l)
		{
			return json{ {"maker", l.maker}, {"name", l.name}, {"focalLength", l.focalLength},
			             {"fn", l.fn}, {"fnMax", l.fnMax}, {"hasContact", l.hasContact},
			             {"fisheye", l.fisheye} };
		}
		hgc::lens lensFromJson(const json& j)
		{
			hgc::lens l;
			l.maker       = j.value("maker", std::string());
			l.name        = j.value("name", std::string());
			l.focalLength = j.value("focalLength", 0.0);
			l.fn          = j.value("fn", 0.0);
			l.fnMax       = j.value("fnMax", 0.0);
			l.hasContact  = j.value("hasContact", true);
			l.fisheye     = j.value("fisheye", isFisheyeName(l.name));	// フィールド優先・無ければ名前判定
			return l;
		}

		// --- 撮影制御方法(多態) ---
		void baseToJson(json& j, const hgc::ccmBase& c)
		{
			j["type"]  = static_cast<int>(c.type);
			j["name"]  = c.name;
			j["limitBright"] = expToJson(c.limitBright);
			j["limitDark"]   = expToJson(c.limitDark);
			j["initial"] = expToJson(c.initial);
			json pr = json::array();
			for (int i = 0; i < hgc::exposureTypeNum; ++i) { pr.push_back(static_cast<int>(c.priority[i])); }
			j["priority"] = pr;
			j["hysteresis"]    = c.hysteresis;		// 個別露出平滑化(0=全体設定)
			j["movingAverage"] = c.movingAverage;
		}
		void baseFromJson(const json& j, hgc::ccmBase& c)
		{
			c.name  = j.value("name", std::string());
			if (j.contains("limitBright")) { c.limitBright = expFromJson(j["limitBright"]); }
			if (j.contains("limitDark"))   { c.limitDark   = expFromJson(j["limitDark"]); }
			// 基準(iso/ss/fn。JSONキーは initial 維持)。後方互換: 旧 initialBright(bool) しか無ければ限界から派生する
		// (true=明所限界=limitDark / false=暗所限界=limitBright)。limit は上で読み込み済み。
		if (j.contains("initial")) { c.initial = expFromJson(j["initial"]); }
		else { c.initial = j.value("initialBright", true) ? c.limitDark : c.limitBright; }
			if (j.contains("priority") && j["priority"].is_array())
			{
				const auto& pr = j["priority"];
				for (int i = 0; i < hgc::exposureTypeNum && i < static_cast<int>(pr.size()); ++i)
				{
					c.priority[i] = static_cast<hgc::exposureType>(pr[i].get<int>());
				}
			}
			c.hysteresis    = j.value("hysteresis", 0.0);		// 個別露出平滑化(0=全体設定)
			c.movingAverage = j.value("movingAverage", 0u);
		}

		json ccmToJsonObj(const hgc::ccmBase& c)
		{
			json j;
			baseToJson(j, c);
			switch (c.type)
			{
			case hgc::ccmType::night:
			{
				const auto& n = static_cast<const hgc::ccmNight&>(c);
				j["sunAltitude"] = n.sunAltitude;
				j["postNightEv"] = n.postNightEv;
				j["preNightEv"]  = n.preNightEv;
				break;
			}
			case hgc::ccmType::sunrise:
			{
				const auto& s = static_cast<const hgc::ccmSunrise&>(c);
				j["sunAltitude"] = s.sunAltitude; j["sunAltitudeEnd"] = s.sunAltitudeEnd; j["ev"] = s.ev;
				break;
			}
			case hgc::ccmType::sunset:
			{
				const auto& s = static_cast<const hgc::ccmSunset&>(c);
				j["sunAltitude"] = s.sunAltitude; j["sunAltitudeEnd"] = s.sunAltitudeEnd; j["ev"] = s.ev;
				break;
			}
			case hgc::ccmType::day:
			{
				const auto& d = static_cast<const hgc::ccmDay&>(c);
				j["ev"] = d.ev;
				break;
			}
			default: break;	// linear/invalid は基本フィールドのみ
			}
			return j;
		}

		std::shared_ptr<hgc::ccmBase> ccmFromJsonObj(const json& j)
		{
			hgc::ccmType t = static_cast<hgc::ccmType>(j.value("type", 0));
			std::shared_ptr<hgc::ccmBase> c;
			switch (t)
			{
			case hgc::ccmType::night:
			{
				auto n = std::make_shared<hgc::ccmNight>();
				n->sunAltitude = j.value("sunAltitude", -18.0);
				n->postNightEv = j.value("postNightEv", 0.0);
				n->preNightEv  = j.value("preNightEv", 0.0);
				c = n; break;
			}
			case hgc::ccmType::sunrise:
			{
				auto s = std::make_shared<hgc::ccmSunrise>();
				s->sunAltitude    = j.value("sunAltitude", -6.0);
				s->sunAltitudeEnd = j.value("sunAltitudeEnd", 0.0);
				s->ev             = j.value("ev", -3.0);
				c = s; break;
			}
			case hgc::ccmType::sunset:
			{
				auto s = std::make_shared<hgc::ccmSunset>();
				s->sunAltitude    = j.value("sunAltitude", 0.0);
				s->sunAltitudeEnd = j.value("sunAltitudeEnd", -6.0);
				s->ev             = j.value("ev", -3.0);
				c = s; break;
			}
			case hgc::ccmType::day:
			{
				auto d = std::make_shared<hgc::ccmDay>();
				d->ev = j.value("ev", 0.0);
				c = d; break;
			}
			case hgc::ccmType::preNight:
				c = std::make_shared<hgc::ccmBase>(hgc::ccmType::preNight); break;
			case hgc::ccmType::postNight:
				c = std::make_shared<hgc::ccmBase>(hgc::ccmType::postNight); break;
			default:
				c = std::make_shared<hgc::ccmBase>(t); break;
			}
			baseFromJson(j, *c);
			return c;
		}
	}

	std::string toJson(const hgc::cs& plan)
	{
		json j;
		// 【キー名(2026-09-02)】計画名は "planName"。"name" は機材やエッジ端末でも使う
		//  一般名なので、別種のJSONと取り違えたときに見分けが付かなかった。
		//  **保存ファイルの形式も変わる**(旧い計画ファイルは名前を失う。計画は作り直す)。
		j["planName"]  = plan.name;
		j["start"]     = dtToJson(plan.start);
		j["end"]       = dtToJson(plan.end);
		j["place"]     = placeToJson(plan.place);
		j["camera"]    = cameraToJson(plan.camera);
		// 同期撮影(2026-08-25)。旧データ互換のため fromJson 側に既定値を置く。
		j["syncShot"]  = plan.syncShot;
		{
			json arr = json::array();
			for (const auto& c : plan.subCameras) { arr.push_back(cameraToJson(c)); }
			j["subCameras"] = arr;
		}
		j["lens"]      = lensToJson(plan.lens);
		j["interval"]  = plan.interval;
		j["azimuth"]   = plan.azimuth;
		j["elevation"] = plan.elevation;
		j["landscape"] = plan.landscape;
		// この計画が所有する撮影制御方法一式(2026-08-11 改定。ここだけが権威)。
		// 窓(ccmList)は型と時刻だけを持ち、実体はここから引く。以前は窓ごとに実体を複製して
		// 書き出していたが、実体は毎回 planCcm から作り直されるため二重管理になっていた。
		{
			json jc;
			if (plan.ccm.night)   { jc["night"]   = ccmToJsonObj(*plan.ccm.night); }
			if (plan.ccm.sunrise) { jc["sunrise"] = ccmToJsonObj(*plan.ccm.sunrise); }
			if (plan.ccm.sunset)  { jc["sunset"]  = ccmToJsonObj(*plan.ccm.sunset); }
			if (plan.ccm.day)     { jc["day"]     = ccmToJsonObj(*plan.ccm.day); }
			jc["useNight"]   = plan.ccm.useNight;
			jc["useSunrise"] = plan.ccm.useSunrise;
			jc["useSunset"]  = plan.ccm.useSunset;
			jc["useDay"]     = plan.ccm.useDay;
			j["ccm"] = jc;
		}
		json bl = json::array();
		for (const auto& b : plan.boundaries)
		{
			bl.push_back(json{ {"before", static_cast<int>(b.before)}, {"after", static_cast<int>(b.after)},
			                   {"occ", b.occ}, {"when", dtToJson(b.when)},
			                   {"altDeg", b.altDeg}, {"rising", b.rising} });
		}
		j["boundaries"] = bl;

		json ev = json::array();
		for (const auto& e : plan.events)
		{
			ev.push_back(json{ {"event", static_cast<int>(e.event)}, {"when", dtToJson(e.when)} });
		}
		j["events"] = ev;

		json wl = json::array();
		for (const auto& w : plan.ccmList)
		{
			json wj;
			wj["start"] = dtToJson(w.start);
			wj["end"]   = dtToJson(w.end);
			// 実体は書かない(計画所有の ccm から引くため)。型だけ持つ。
			wj["type"]  = static_cast<int>(w.type != hgc::ccmType::invalid ? w.type
			                               : (w.ccm ? w.ccm->type : hgc::ccmType::invalid));
			wl.push_back(wj);
		}
		j["ccmList"] = wl;
		// 夜間の固定露出と移行目標ev(夜間ウィンドウが無くても移行のクランプ/基準に使う。仕様3.7/3.9)。
		j["nightFixedExposure"] = expToJson(plan.nightFixedExposure);
		j["nightPreNightEv"]    = plan.nightPreNightEv;
		j["nightPostNightEv"]   = plan.nightPostNightEv;

		return j.dump();
	}

	bool fromJson(const std::string& s, hgc::cs& plan)
	{
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return false; }

		plan = hgc::cs{};
		plan.name      = j.value("planName", std::string());
		if (j.contains("start")) { plan.start = dtFromJson(j["start"]); }
		if (j.contains("end"))   { plan.end   = dtFromJson(j["end"]); }
		if (j.contains("place"))  { plan.place  = placeFromJson(j["place"]); }
		if (j.contains("camera")) { plan.camera = cameraFromJson(j["camera"]); }
		plan.syncShot = j.value("syncShot", false);
		plan.subCameras.clear();
		if (j.contains("subCameras") && j["subCameras"].is_array())
		{
			for (const auto& c : j["subCameras"]) { plan.subCameras.push_back(cameraFromJson(c)); }
		}
		if (j.contains("lens"))   { plan.lens   = lensFromJson(j["lens"]); }
		plan.interval  = j.value("interval", 0.0);
		plan.azimuth   = j.value("azimuth", 0.0);
		plan.elevation = j.value("elevation", 0.0);
		plan.landscape = j.value("landscape", true);
		// スケジュール手動編集(7.3.2)
		if (j.contains("boundaries") && j["boundaries"].is_array())
		{
			for (const auto& b : j["boundaries"])
			{
				hgc::boundaryOverride bo;
				bo.before = static_cast<hgc::ccmType>(b.value("before", 0));
				bo.after  = static_cast<hgc::ccmType>(b.value("after", 0));
				bo.occ    = b.value("occ", 0);
				if (b.contains("when")) { bo.when = dtFromJson(b["when"]); }
				bo.altDeg = b.value("altDeg", -100.0);
				bo.rising = b.value("rising", false);
				plan.boundaries.push_back(bo);
			}
		}

		if (j.contains("events") && j["events"].is_array())
		{
			for (const auto& e : j["events"])
			{
				hgc::eventItem it;
				it.event = static_cast<hgc::csEvent>(e.value("event", 0));
				if (e.contains("when")) { it.when = dtFromJson(e["when"]); }
				plan.events.push_back(it);
			}
		}
		// 計画所有の撮影制御方法一式(窓より先に読む。窓はここから実体を引くため)。
		if (j.contains("ccm") && j["ccm"].is_object())
		{
			const auto& jc = j["ccm"];
			auto pick = [&](const char* k) -> std::shared_ptr<hgc::ccmBase>
			{ return jc.contains(k) ? ccmFromJsonObj(jc[k]) : nullptr; };
			plan.ccm.set(hgc::ccmType::night,   pick("night"));
			plan.ccm.set(hgc::ccmType::sunrise, pick("sunrise"));
			plan.ccm.set(hgc::ccmType::sunset,  pick("sunset"));
			plan.ccm.set(hgc::ccmType::day,     pick("day"));
			plan.ccm.useNight   = jc.value("useNight",   true);
			plan.ccm.useSunrise = jc.value("useSunrise", true);
			plan.ccm.useSunset  = jc.value("useSunset",  true);
			plan.ccm.useDay     = jc.value("useDay",     true);
		}
		if (j.contains("ccmList") && j["ccmList"].is_array())
		{
			for (const auto& w : j["ccmList"])
			{
				hgc::ccmWindow win;
				if (w.contains("start")) { win.start = dtFromJson(w["start"]); }
				if (w.contains("end"))   { win.end   = dtFromJson(w["end"]); }
				win.type = static_cast<hgc::ccmType>(w.value("type", 0));
				// 実体は計画所有の一式から引く(複製しない)。移行(夜間前/後)はその場で作る。
				win.ccm  = plan.ccm.get(win.type);
				if (!win.ccm && win.type != hgc::ccmType::invalid)
				{
					win.ccm = std::make_shared<hgc::ccmBase>(win.type);
					win.ccm->name = (win.type == hgc::ccmType::preNight) ? "preNight"
					              : (win.type == hgc::ccmType::postNight) ? "postNight" : "";
				}
				// 実体を解決できない窓は捨てる。撮影ループは窓の ccm を無条件に参照するので、
				// null を混ぜたまま進めると落ちる。型が付いていない古い保存を読んだ場合が該当し、
				// その場合は ccmList を空にして呼び出し側に組み立て直させるのが安全。
				if (!win.ccm) { plan.ccmList.clear(); break; }
				plan.ccmList.push_back(std::move(win));
			}
		}
		if (j.contains("nightFixedExposure")) { plan.nightFixedExposure = expFromJson(j["nightFixedExposure"]); }
		plan.nightPreNightEv  = j.value("nightPreNightEv", 0.0);
		plan.nightPostNightEv = j.value("nightPostNightEv", 0.0);
		return true;
	}

	std::string ccmToJson(const hgc::ccmBase& ccm)
	{
		return ccmToJsonObj(ccm).dump();
	}

	std::shared_ptr<hgc::ccmBase> ccmFromJson(const std::string& s)
	{
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return nullptr; }
		return ccmFromJsonObj(j);
	}

	// ========================================================================
	//  所持機材(§5.5/5.6)・機材マスタ(§5.8/5.9)の JSON 変換
	// ========================================================================

	// --- 所持カメラ/所持レンズ(内部形式。/asset/ownedCameras.json・ownedLenses.json) ---
	std::string ownedCamerasToJson(const std::vector<hgc::ownedCamera>& list)
	{
		json arr = json::array();
		for (const auto& oc : list)
		{
			json o;
			o["camera"]     = cameraToJson(oc.cam);	// JSONキーは "camera"(メンバは cam)
			json ll = json::array();
			for (const auto& l : oc.lensList) { ll.push_back(lensToJson(l)); }
			o["lensList"]   = ll;
			o["autoInsert"] = oc.autoInsert;
			arr.push_back(o);
		}
		return arr.dump();
	}

	bool ownedCamerasFromJson(const std::string& s, std::vector<hgc::ownedCamera>& out)
	{
		out.clear();
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_array()) { return false; }
		for (const auto& o : j)
		{
			if (!o.is_object()) { continue; }
			hgc::ownedCamera oc;
			if (o.contains("camera")) { oc.cam = cameraFromJson(o["camera"]); }
			if (o.contains("lensList") && o["lensList"].is_array())
			{
				for (const auto& l : o["lensList"]) { oc.lensList.push_back(lensFromJson(l)); }
			}
			oc.autoInsert = o.value("autoInsert", false);
			out.push_back(std::move(oc));
		}
		return true;
	}

	std::string ownedLensesToJson(const std::vector<hgc::lens>& list)
	{
		json arr = json::array();
		for (const auto& l : list) { arr.push_back(lensToJson(l)); }
		return arr.dump();
	}

	bool ownedLensesFromJson(const std::string& s, std::vector<hgc::lens>& out)
	{
		out.clear();
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_array()) { return false; }
		for (const auto& l : j) { if (l.is_object()) { out.push_back(lensFromJson(l)); } }
		return true;
	}

	std::string placesToJson(const std::vector<hgc::place>& list)
	{
		json arr = json::array();
		for (const auto& p : list) { arr.push_back(placeToJson(p)); }
		return arr.dump();
	}

	bool placesFromJson(const std::string& s, std::vector<hgc::place>& out)
	{
		out.clear();
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_array()) { return false; }
		for (const auto& p : j) { if (p.is_object()) { out.push_back(placeFromJson(p)); } }
		return true;
	}

	// --- 機材マスタ(30_refer 由来の読取専用形式) ---
	// camera_body_list: manufacture/name/pixel_h/pixel_w/sensor_h/sensor_w/iso(int配列)/ss(string配列)
	bool camerasFromMasterJson(const std::string& s, std::vector<hgc::camera>& out)
	{
		out.clear();
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_array()) { return false; }
		for (const auto& m : j)
		{
			if (!m.is_object()) { continue; }
			hgc::camera c;
			c.maker       = m.value("manufacture", std::string());
			c.name        = m.value("name", std::string());
			c.model       = c.name;	// マスタは型番のみ。model/name 共通とする
			c.sensorSize  = m.value("sensor_w", 0.0);	// 横[mm]
			c.sensorSizeV = m.value("sensor_h", 0.0);	// 縦[mm]
			c.sensorPixel = m.value("pixel_w", 0u);		// 横[pixel]
			c.sensorPixelV = m.value("pixel_h", 0u);	// 縦[pixel]
			c.meterLv     = m.value("meter_lv", false);	// 測光方式(無い=サムネイルだけ)
			if (m.contains("iso") && m["iso"].is_array())
			{
				for (const auto& v : m["iso"])
				{
					if (v.is_number_integer()) { c.isoList.push_back(std::to_string(v.get<long long>())); }
					else if (v.is_string())    { c.isoList.push_back(v.get<std::string>()); }
				}
			}
			if (m.contains("ss") && m["ss"].is_array())
			{
				for (const auto& v : m["ss"]) { if (v.is_string()) { c.ssList.push_back(v.get<std::string>()); } }
			}
			out.push_back(std::move(c));
		}
		return true;
	}

	// lenses_list: manufacture/mount/name/f_min/f_max/fnum_min_wide/fnum_min_tele/fnum_max/electronic_contacts
	// 単一焦点・単一F値モデルへ縮約(焦点=f_min(広角端)、開放F=fnum_min_wide。ズーム/絞り域は後回し)。
	bool lensesFromMasterJson(const std::string& s, std::vector<hgc::lens>& out)
	{
		out.clear();
		json j = json::parse(s, nullptr, false);
		if (j.is_discarded() || !j.is_array()) { return false; }
		for (const auto& m : j)
		{
			if (!m.is_object()) { continue; }
			hgc::lens l;
			l.maker       = m.value("manufacture", std::string());
			l.name        = m.value("name", std::string());
			l.focalLength = m.value("f_min", 0.0);
			l.fn          = m.value("fnum_min_wide", 0.0);
			l.fnMax       = m.value("fnum_max", 0.0);
			l.hasContact  = m.value("electronic_contacts", true);
			l.fisheye     = m.value("fisheye", isFisheyeName(l.name));	// フィールド優先・無ければ名前判定
			out.push_back(std::move(l));
		}
		return true;
	}
}
