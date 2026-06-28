// 撮影計画(cs)・撮影制御方法(ccm)の JSON 相互変換(データ構造仕様書43)。
#include "csJson.h"
#include <json/nlohmann/json.hpp>

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
			return json{ {"iso", e.iso}, {"ss", e.ss}, {"fn", e.fn} };
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
			return e;
		}

		json placeToJson(const hgc::place& p)
		{
			return json{ {"name", p.name}, {"latitude", p.latitude}, {"longitude", p.longitude},
			             {"altitude", p.altitude}, {"autoInsert", p.autoInsert} };
		}
		hgc::place placeFromJson(const json& j)
		{
			hgc::place p;
			p.name       = j.value("name", std::string());
			p.latitude   = j.value("latitude", 0.0);
			p.longitude  = j.value("longitude", 0.0);
			p.altitude   = j.value("altitude", 0.0);
			p.autoInsert = j.value("autoInsert", false);
			return p;
		}

		json cameraToJson(const hgc::camera& c)
		{
			return json{ {"maker", c.maker}, {"model", c.model}, {"name", c.name},
			             {"serial", c.serial}, {"friendly", c.friendly},
			             {"sensorSize", c.sensorSize}, {"sensorSizeV", c.sensorSizeV}, {"sensorPixel", c.sensorPixel},
			             {"isoList", c.isoList}, {"ssList", c.ssList} };
		}
		hgc::camera cameraFromJson(const json& j)
		{
			hgc::camera c;
			c.maker       = j.value("maker", std::string());
			c.model       = j.value("model", std::string());
			c.name        = j.value("name", std::string());
			c.serial      = getStr(j, "serial");
			c.friendly    = getStr(j, "friendly");
			c.sensorSize  = j.value("sensorSize", 0.0);
			c.sensorSizeV = j.value("sensorSizeV", 0.0);
			c.sensorPixel = j.value("sensorPixel", 0u);
			if (j.contains("isoList")) { c.isoList = j["isoList"].get<std::vector<std::string>>(); }
			if (j.contains("ssList"))  { c.ssList  = j["ssList"].get<std::vector<std::string>>(); }
			return c;
		}

		json lensToJson(const hgc::lens& l)
		{
			return json{ {"maker", l.maker}, {"name", l.name}, {"focalLength", l.focalLength},
			             {"fn", l.fn}, {"fnMax", l.fnMax}, {"hasContact", l.hasContact} };
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
			return l;
		}

		// --- 撮影制御方法(多態) ---
		void baseToJson(json& j, const hgc::ccmBase& c)
		{
			j["type"]  = static_cast<int>(c.type);
			j["name"]  = c.name;
			j["color"] = c.color;
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
			c.color = j.value("color", 0u);
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
			case hgc::ccmType::moon:
			{
				const auto& m = static_cast<const hgc::ccmMoon&>(c);
				j["mode"] = static_cast<int>(m.mode);
				j["startLuminance"] = m.startLuminance;
				j["ev"] = m.ev;
				j["initialExposure"] = expToJson(m.initialExposure);
				j["atmosphericExtinction"] = m.atmosphericExtinction;
				j["extinctionCoef"] = m.extinctionCoef;
				j["geocentricCorrection"] = m.geocentricCorrection;
				j["skyBrightnessCoef"] = m.skyBrightnessCoef;
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
			case hgc::ccmType::moon:
			{
				auto m = std::make_shared<hgc::ccmMoon>();
				m->mode = static_cast<hgc::moonMode>(j.value("mode", 0));
				m->startLuminance = j.value("startLuminance", 0.0);
				m->ev = j.value("ev", 0.0);
				if (j.contains("initialExposure")) { m->initialExposure = expFromJson(j["initialExposure"]); }
				m->atmosphericExtinction = j.value("atmosphericExtinction", false);
				m->extinctionCoef = j.value("extinctionCoef", 0.2);
				m->geocentricCorrection = j.value("geocentricCorrection", false);
				m->skyBrightnessCoef = j.value("skyBrightnessCoef", 100.0);
				c = m; break;
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
		j["name"]      = plan.name;
		j["start"]     = dtToJson(plan.start);
		j["end"]       = dtToJson(plan.end);
		j["place"]     = placeToJson(plan.place);
		j["camera"]    = cameraToJson(plan.camera);
		j["lens"]      = lensToJson(plan.lens);
		j["interval"]  = plan.interval;
		j["azimuth"]   = plan.azimuth;
		j["elevation"] = plan.elevation;
		j["landscape"] = plan.landscape;
		// スケジュール手動編集(7.3.2)
		j["sunriseMode"] = static_cast<int>(plan.sunriseMode);
		j["sunsetMode"]  = static_cast<int>(plan.sunsetMode);
		json bl = json::array();
		for (const auto& b : plan.boundaries)
		{
			bl.push_back(json{ {"before", static_cast<int>(b.before)}, {"after", static_cast<int>(b.after)},
			                   {"occ", b.occ}, {"when", dtToJson(b.when)} });
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
			if (w.ccm) { wj["ccm"] = ccmToJsonObj(*w.ccm); }
			wl.push_back(wj);
		}
		j["ccmList"] = wl;
		if (plan.startLeadCcm) { j["startLeadCcm"] = ccmToJsonObj(*plan.startLeadCcm); }
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
		plan.name      = j.value("name", std::string());
		if (j.contains("start")) { plan.start = dtFromJson(j["start"]); }
		if (j.contains("end"))   { plan.end   = dtFromJson(j["end"]); }
		if (j.contains("place"))  { plan.place  = placeFromJson(j["place"]); }
		if (j.contains("camera")) { plan.camera = cameraFromJson(j["camera"]); }
		if (j.contains("lens"))   { plan.lens   = lensFromJson(j["lens"]); }
		plan.interval  = j.value("interval", 0.0);
		plan.azimuth   = j.value("azimuth", 0.0);
		plan.elevation = j.value("elevation", 0.0);
		plan.landscape = j.value("landscape", true);
		// スケジュール手動編集(7.3.2)
		plan.sunriseMode = static_cast<hgc::bandMode>(j.value("sunriseMode", 0));
		plan.sunsetMode  = static_cast<hgc::bandMode>(j.value("sunsetMode", 0));
		if (j.contains("boundaries") && j["boundaries"].is_array())
		{
			for (const auto& b : j["boundaries"])
			{
				hgc::boundaryOverride bo;
				bo.before = static_cast<hgc::ccmType>(b.value("before", 0));
				bo.after  = static_cast<hgc::ccmType>(b.value("after", 0));
				bo.occ    = b.value("occ", 0);
				if (b.contains("when")) { bo.when = dtFromJson(b["when"]); }
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
		if (j.contains("ccmList") && j["ccmList"].is_array())
		{
			for (const auto& w : j["ccmList"])
			{
				hgc::ccmWindow win;
				if (w.contains("start")) { win.start = dtFromJson(w["start"]); }
				if (w.contains("end"))   { win.end   = dtFromJson(w["end"]); }
				if (w.contains("ccm"))   { win.ccm   = ccmFromJsonObj(w["ccm"]); }
				plan.ccmList.push_back(std::move(win));
			}
		}
		if (j.contains("startLeadCcm")) { plan.startLeadCcm = ccmFromJsonObj(j["startLeadCcm"]); }
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
			out.push_back(std::move(l));
		}
		return true;
	}
}
