// 標準ひな形の組み立て(stdTemplates.h)。
#include "stdTemplates.h"
#include "dataManager.h"
#include "astroSched.h"
#include "csJson.h"
#include "holyGrailEntity.h"
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <ctime>

namespace stdtpl
{
	namespace
	{
		const char* const kKeys[static_cast<int>(kind::count)] = {
			"star_sunrise", "night_sunrise", "star_sunrise_sunstar", "night_sunrise_sunstar",
			"star_sunset",  "night_sunset",  "star_sunset_sunstar",  "night_sunset_sunstar" };

		// 境目の太陽高度(ユーザー指示 2026-09-21)。既定の帯(夜間<-12°/移行/朝日夕日 0〜+3°/日中)から
		//  動かすものだけ boundaries に入れる(-12° と +3° は既定のまま)。
		constexpr double kSunToTwilight = -3.0;	// 移行↔朝日(夕日)・移行↔日中(太陽を撮らない側)
		constexpr double kPreNightEv  = -0.667;	// 夜間前移行の露出補正(1/6 段の目盛り。-0.7 の指定を目盛りへ)
		constexpr double kPostNightEv = -1.5;	// 夜間後移行の露出補正
		constexpr double kSunEv       = -3.0;	// 朝日/夕日の露出補正
		constexpr double kSunHyst     = 0.3;	// 朝日/夕日のヒステリシス[段]
		constexpr const char* kSunstarFnDark   = "11";	// 光条: 暗所限界の F
		constexpr const char* kSunstarFnBright = "16";	// 光条: 明所限界の F

		void addBoundary(hgc::cs& cs, hgc::ccmType before, hgc::ccmType after, double alt, bool rising)
		{
			hgc::boundaryOverride bo;
			bo.before = before; bo.after = after; bo.occ = 0;
			bo.altDeg = alt; bo.rising = rising;
			cs.boundaries.push_back(bo);
		}
	}

	const char* key(kind k) { return kKeys[static_cast<int>(k)]; }
	bool isSunstar(kind k) { return k == kind::starSunriseSunstar || k == kind::nightSunriseSunstar ||
	                                k == kind::starSunsetSunstar  || k == kind::nightSunsetSunstar; }
	bool isSunrise(kind k) { return static_cast<int>(k) < static_cast<int>(kind::starSunset); }
	bool isStar(kind k)    { return k == kind::starSunrise || k == kind::starSunriseSunstar ||
	                                k == kind::starSunset  || k == kind::starSunsetSunstar; }

	names parseNames(const std::string& namesJson, const char* ccmKey)
	{
		names n;
		nlohmann::json j = nlohmann::json::parse(namesJson, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return n; }
		if (j.contains("tpl") && j["tpl"].is_object())
		{
			for (auto it = j["tpl"].begin(); it != j["tpl"].end(); ++it)
			{
				if (it.value().is_string()) { n.tpl[it.key()] = it.value().get<std::string>(); }
			}
		}
		const nlohmann::json* c = &j;
		if (ccmKey != nullptr && ccmKey[0] != '\0')
		{
			if (!j.contains(ccmKey) || !j[ccmKey].is_object()) { return n; }
			c = &j[ccmKey];
		}
		auto pick = [&](const char* k, std::string& dst) {
			if (c->contains(k) && (*c)[k].is_string()) { dst = (*c)[k].get<std::string>(); }
		};
		pick("night", n.night); pick("sunrise", n.sunrise); pick("sunset", n.sunset); pick("day", n.day);
		return n;
	}

	void build(const gear& g, kind k, const names& nm, hgc::cs& cs)
	{
		cs = hgc::cs{};
		dataManager::factoryFixedPlan(cs);
		{ hgc::place ap; if (dataManager::autoInsertPlace(ap)) { cs.place = ap; } }
		cs.tplKind = key(k);
		{
			auto it = nm.tpl.find(cs.tplKind);
			const std::string title = (it != nm.tpl.end() && !it->second.empty()) ? it->second : cs.tplKind;
			cs.name = g.camera.name + " " + title;	// 「カメラ名」+ 名称
		}
		cs.camera   = g.camera;
		cs.lens     = g.lens;
		cs.interval = isStar(k) ? g.starInterval : g.cityInterval;

		// 窓: 日の出含む=今日 21:00〜翌 09:00 / 日の入含む=今日 15:00〜翌 03:00(場所の時刻)。
		//  計画を作るときは日付だけ今日へ寄るので、時刻だけが意味を持つ。
		{
			const int off = cs.place.tzOffMin;
			hgc::dateTime st = hgc::fromUnixUtc(static_cast<long long>(std::time(nullptr)), off);
			st.hour = static_cast<uint16_t>(isSunrise(k) ? 21 : 15); st.min = 0; st.sec = 0;
			cs.start = st;
			cs.end   = hgc::fromUnixUtc(hgc::toUnixUtc(st, off) + 12 * 3600, off);
		}

		// 撮影制御方法。夜間=固定露出(星景/夜景で ss が違う)、朝日/夕日/日中=夜間の露出を暗所限界・基準に、
		//  明所限界へ向けて iso→ss→F の順で動く(§4.5 の往復対称で戻るときは逆順)。
		const hgc::exposure dark = isStar(k) ? g.starNight : g.cityNight;
		hgc::exposure darkSun = dark, brightSun = g.bright;
		if (isSunstar(k)) { darkSun.fn = kSunstarFnDark; brightSun.fn = kSunstarFnBright; }

		auto night = std::make_shared<hgc::ccmNight>();
		night->name = nm.night.empty() ? "night" : nm.night;
		night->forPhone = g.forPhone;
		night->sunAltitude = -12.0;			// 既定の帯と同じ値を持たせる(境目の可動範囲の基準に使われる)
		night->limitBright = night->limitDark = night->initial = dark;
		night->preNightEv  = kPreNightEv;
		night->postNightEv = kPostNightEv;
		cs.ccm.night = night;

		auto fillAuto = [&](hgc::ccmBase& c, const std::string& name)
		{
			c.name = name;
			c.forPhone = g.forPhone;
			c.limitBright = darkSun;
			c.limitDark   = brightSun;
			c.initial     = darkSun;		// 基準=暗所限界(基準へ寄る/離れる動きにする。2026-09-21 ユーザー指示)
			c.priority[0] = hgc::exposureType::iso;
			c.priority[1] = hgc::exposureType::ss;
			c.priority[2] = hgc::exposureType::fn;
		};
		auto sunrise = std::make_shared<hgc::ccmSunrise>();
		fillAuto(*sunrise, nm.sunrise.empty() ? "sunrise" : nm.sunrise);
		sunrise->ev = kSunEv; sunrise->hysteresis = kSunHyst; sunrise->smoothMin = 0.0;	// なめらかさは全体設定
		cs.ccm.sunrise = sunrise;

		auto sunset = std::make_shared<hgc::ccmSunset>();
		fillAuto(*sunset, nm.sunset.empty() ? "sunset" : nm.sunset);
		sunset->ev = kSunEv; sunset->hysteresis = kSunHyst; sunset->smoothMin = 0.0;
		cs.ccm.sunset = sunset;

		auto day = std::make_shared<hgc::ccmDay>();
		fillAuto(*day, nm.day.empty() ? "day" : nm.day);
		day->ev = 0.0;
		cs.ccm.day = day;

		cs.ccm.useNight = true; cs.ccm.useDay = true;
		cs.ccm.useSunrise = isSunrise(k);
		cs.ccm.useSunset  = !isSunrise(k);
		cs.nightFixedExposure = dark;
		cs.nightPreNightEv    = kPreNightEv;
		cs.nightPostNightEv   = kPostNightEv;

		// 境目(太陽高度で保持。適用は現在の窓の中で交差を探す)。
		using T = hgc::ccmType;
		if (isSunrise(k))
		{
			addBoundary(cs, T::postNight, T::sunrise,  kSunToTwilight, true);	// 朝: 移行→朝日 -3°
			addBoundary(cs, T::day,       T::preNight, kSunToTwilight, false);	// 夕: 日中→移行 -3°
		}
		else
		{
			addBoundary(cs, T::sunset,    T::preNight, kSunToTwilight, false);	// 夕: 夕日→移行 -3°
			addBoundary(cs, T::postNight, T::day,      kSunToTwilight, true);	// 朝: 移行→日中 -3°
		}
		astro::buildSchedule(cs);
	}

	int seed(const gear& g, const names& nm, bool withSunstar)
	{
		int made = 0;
		for (int i = 0; i < static_cast<int>(kind::count); ++i)
		{
			const kind k = static_cast<kind>(i);
			if (isSunstar(k) && (!withSunstar || g.fnFixed)) { continue; }
			hgc::cs cs;
			build(g, k, nm, cs);
			if (hge_saveStdTemplateJson(csjson::toJson(cs).c_str()) != 1) { continue; }
			++made;
			char b[256];
			std::snprintf(b, sizeof(b), "std template ready: %s (%s / %.0fs / night %s %s %s / bright %s %s %s)",
			              cs.name.c_str(), cs.tplKind.c_str(), cs.interval,
			              cs.nightFixedExposure.iso.c_str(), cs.nightFixedExposure.ss.c_str(), cs.nightFixedExposure.fn.c_str(),
			              cs.ccm.day->limitDark.iso.c_str(), cs.ccm.day->limitDark.ss.c_str(), cs.ccm.day->limitDark.fn.c_str());
			dataManager::logEvent("GEAR", b);
		}
		return made;
	}
}
