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
			             {"sensorSize", c.sensorSize}, {"sensorSizeV", c.sensorSizeV}, {"sensorPixel", c.sensorPixel},
			             {"isoList", c.isoList}, {"ssList", c.ssList} };
		}
		hgc::camera cameraFromJson(const json& j)
		{
			hgc::camera c;
			c.maker       = j.value("maker", std::string());
			c.model       = j.value("model", std::string());
			c.name        = j.value("name", std::string());
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
			             {"fn", l.fn}, {"hasContact", l.hasContact} };
		}
		hgc::lens lensFromJson(const json& j)
		{
			hgc::lens l;
			l.maker       = j.value("maker", std::string());
			l.name        = j.value("name", std::string());
			l.focalLength = j.value("focalLength", 0.0);
			l.fn          = j.value("fn", 0.0);
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
			j["initialBright"] = c.initialBright;
			json pr = json::array();
			for (int i = 0; i < hgc::exposureTypeNum; ++i) { pr.push_back(static_cast<int>(c.priority[i])); }
			j["priority"] = pr;
		}
		void baseFromJson(const json& j, hgc::ccmBase& c)
		{
			c.name  = j.value("name", std::string());
			c.color = j.value("color", 0u);
			if (j.contains("limitBright")) { c.limitBright = expFromJson(j["limitBright"]); }
			if (j.contains("limitDark"))   { c.limitDark   = expFromJson(j["limitDark"]); }
			c.initialBright = j.value("initialBright", true);
			if (j.contains("priority") && j["priority"].is_array())
			{
				const auto& pr = j["priority"];
				for (int i = 0; i < hgc::exposureTypeNum && i < static_cast<int>(pr.size()); ++i)
				{
					c.priority[i] = static_cast<hgc::exposureType>(pr[i].get<int>());
				}
			}
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
				j["sunAltitude"] = n.sunAltitude; j["autoEdge"] = n.autoEdge;
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
				n->autoEdge    = j.value("autoEdge", true);
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
			case hgc::ccmType::linear:
				c = std::make_shared<hgc::ccmBase>(hgc::ccmType::linear); break;
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
}
