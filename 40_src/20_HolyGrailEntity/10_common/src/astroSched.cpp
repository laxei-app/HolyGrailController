#include "astroSched.h"
#include <astronomy/astronomy.h>
#include <algorithm>
#include <cmath>

namespace astro
{
	namespace
	{
		constexpr double PI = 3.14159265358979323846;
		constexpr double DEG = 180.0 / PI;

		// ローカル日時 → Astronomy Engine の時刻(真のUTC瞬時)
		astro_time_t toAstro(const hgc::dateTime& d, int offMin)
		{
			astro_time_t t = Astronomy_MakeTime(d.year, d.month, d.day, d.hour, d.min, static_cast<double>(d.sec));
			return Astronomy_AddDays(t, -static_cast<double>(offMin) / 1440.0);
		}

		// Astronomy Engine の時刻 → ローカル日時
		hgc::dateTime fromAstro(astro_time_t t, int offMin)
		{
			astro_time_t loc = Astronomy_AddDays(t, static_cast<double>(offMin) / 1440.0);
			astro_utc_t u = Astronomy_UtcFromTime(loc);
			hgc::dateTime d;
			d.year  = static_cast<uint16_t>(u.year);
			d.month = static_cast<uint16_t>(u.month);
			d.day   = static_cast<uint16_t>(u.day);
			d.hour  = static_cast<uint16_t>(u.hour);
			d.min   = static_cast<uint16_t>(u.minute);
			d.sec   = static_cast<uint16_t>(std::lround(u.second));
			if (d.sec >= 60) { d.sec = 59; }	// 丸め桁あふれの抑止
			return d;
		}

		// 太陽の地平座標(高度/方位)
		horiz sunHorizAt(astro_time_t t, astro_observer_t obs)
		{
			astro_equatorial_t eq = Astronomy_Equator(BODY_SUN, &t, obs, EQUATOR_OF_DATE, ABERRATION);
			astro_horizon_t hz = Astronomy_Horizon(&t, obs, eq.ra, eq.dec, REFRACTION_NORMAL);
			return { hz.azimuth, hz.altitude };
		}

		// 太陽が画角に入っているか
		bool inFrame(const horiz& sun, double shootAz, double shootAlt, const fov& f)
		{
			double d = sun.azimuth - shootAz;
			while (d > 180.0)  { d -= 360.0; }
			while (d < -180.0) { d += 360.0; }
			return (std::fabs(d) <= f.h / 2.0) && (std::fabs(sun.altitude - shootAlt) <= f.v / 2.0);
		}

		// 1サンプルの撮影制御方法を分類する。
		hgc::ccmType classify(double h, bool rising, bool sunInFrame, double nightAlt, double twiAlt)
		{
			constexpr double sunDirectMaxAlt = 12.0;	// これ以上高い太陽は直接撮影ccm扱いにしない
			if (sunInFrame && h >= twiAlt && h <= sunDirectMaxAlt)
			{
				return rising ? hgc::ccmType::sunrise : hgc::ccmType::sunset;
			}
			if (h < nightAlt) { return hgc::ccmType::night; }
			if (h >= twiAlt)  { return hgc::ccmType::day; }
			// 薄明帯(nightAlt..twiAlt)で太陽が画角外:
			//  朝(上昇)はまだ暗いので固定(夜間)、夕(下降)は夜間へリニア移行。
			return rising ? hgc::ccmType::night : hgc::ccmType::linear;
		}

		// 種別から区間用の撮影制御方法を生成する(プロトタイプを深いコピー)。
		std::shared_ptr<hgc::ccmBase> makeCcm(const ccmSet& set, hgc::ccmType ct)
		{
			switch (ct)
			{
			case hgc::ccmType::night:
				if (set.night)   { return set.night->clone(); }
				break;
			case hgc::ccmType::sunrise:
				if (set.sunrise) { return set.sunrise->clone(); }
				break;
			case hgc::ccmType::sunset:
				if (set.sunset)  { return set.sunset->clone(); }
				break;
			case hgc::ccmType::day:
				if (set.day)     { return set.day->clone(); }
				break;
			case hgc::ccmType::linear:
			{
				auto c = std::make_shared<hgc::ccmBase>(hgc::ccmType::linear);
				c->name = "linear";
				return c;
			}
			default: break;
			}
			// フォールバック
			return std::make_shared<hgc::ccmBase>(ct);
		}

		// 順序比較用キー(同一オフセットなら ut で大小比較できる)
		double sortKey(const hgc::dateTime& d)
		{
			astro_time_t t = Astronomy_MakeTime(d.year, d.month, d.day, d.hour, d.min, static_cast<double>(d.sec));
			return t.ut;
		}

		// 指定高度に達する時刻を範囲内で(複数回)探してイベント追加する。
		void addAltitudeEvents(hgc::cs& plan, astro_observer_t obs, hgc::csEvent ev,
		                       astro_direction_t dir, double altitude,
		                       astro_time_t tStart, astro_time_t tEnd, int off)
		{
			astro_time_t cursor = tStart;
			double limit = (tEnd.ut - tStart.ut) + 1e-6;
			for (int guard = 0; guard < 32 && limit > 0.0; ++guard)
			{
				astro_search_result_t r = Astronomy_SearchAltitude(BODY_SUN, obs, dir, cursor, limit, altitude);
				if (r.status != ASTRO_SUCCESS || r.time.ut > tEnd.ut + 1e-9) { break; }
				plan.events.push_back({ ev, fromAstro(r.time, off) });
				cursor = Astronomy_AddDays(r.time, 1.0 / 1440.0);	// 1分進めて次を探す
				limit = (tEnd.ut - cursor.ut) + 1e-6;
			}
		}

		// 天体の出/没(高度0,大気差込み)を範囲内で探してイベント追加する。
		void addRiseSetEvents(hgc::cs& plan, astro_observer_t obs, astro_body_t body, hgc::csEvent ev,
		                      astro_direction_t dir, astro_time_t tStart, astro_time_t tEnd, int off)
		{
			astro_time_t cursor = tStart;
			double limit = (tEnd.ut - tStart.ut) + 1e-6;
			for (int guard = 0; guard < 32 && limit > 0.0; ++guard)
			{
				astro_search_result_t r = Astronomy_SearchRiseSet(body, obs, dir, cursor, limit);
				if (r.status != ASTRO_SUCCESS || r.time.ut > tEnd.ut + 1e-9) { break; }
				plan.events.push_back({ ev, fromAstro(r.time, off) });
				cursor = Astronomy_AddDays(r.time, 1.0 / 1440.0);
				limit = (tEnd.ut - cursor.ut) + 1e-6;
			}
		}
	} // anonymous namespace

	int defaultUtcOffsetMin(double longitude)
	{
		return static_cast<int>(std::lround(longitude / 15.0)) * 60;
	}

	horiz sunHoriz(const hgc::dateTime& localTime, int utcOffsetMin, const hgc::place& p)
	{
		astro_observer_t obs = Astronomy_MakeObserver(p.latitude, p.longitude, p.altitude);
		return sunHorizAt(toAstro(localTime, utcOffsetMin), obs);
	}

	altTime sunAltitudeTime(const hgc::place& p, const hgc::dateTime& baseDate,
	                        double altitude, bool rising, int utcOffsetMin)
	{
		astro_observer_t obs = Astronomy_MakeObserver(p.latitude, p.longitude, p.altitude);
		hgc::dateTime noon = baseDate;	// 基準日の正午(ローカル)を起点に探索
		noon.hour = 12; noon.min = 0; noon.sec = 0;
		astro_time_t start = toAstro(noon, utcOffsetMin);
		astro_direction_t dir = rising ? DIRECTION_RISE : DIRECTION_SET;
		astro_search_result_t r = Astronomy_SearchAltitude(BODY_SUN, obs, dir, start, 2.0, altitude);
		if (r.status != ASTRO_SUCCESS) { return { false, hgc::dateTime{} }; }
		return { true, fromAstro(r.time, utcOffsetMin) };
	}

	fov calcFov(const hgc::camera& cam, const hgc::lens& lens, bool landscape)
	{
		double sw = cam.sensorSize;		// センサー横[mm]
		double fl = lens.focalLength;	// 焦点距離[mm]
		if (sw <= 0.0 || fl <= 0.0) { return { 60.0, 40.0 }; }	// フォールバック
		double sh = sw * 2.0 / 3.0;		// 高さは未取得のため 3:2 を仮定
		double fovW = 2.0 * std::atan2(sw / 2.0, fl) * DEG;
		double fovH = 2.0 * std::atan2(sh / 2.0, fl) * DEG;
		if (landscape) { return { fovW, fovH }; }
		return { fovH, fovW };			// 縦位置は長辺・短辺が入れ替わる
	}

	errCode buildSchedule(hgc::cs& plan, const ccmSet& set, int utcOffsetMin)
	{
		const int off = utcOffsetMin;
		astro_observer_t obs = Astronomy_MakeObserver(plan.place.latitude, plan.place.longitude, plan.place.altitude);
		astro_time_t tStart = toAstro(plan.start, off);
		astro_time_t tEnd   = toAstro(plan.end, off);
		if (tEnd.ut <= tStart.ut) { return ERR_HGC_INVALID_ARG; }

		const fov f = calcFov(plan.camera, plan.lens, plan.landscape);
		const double nightAlt   = set.night   ? set.night->sunAltitude   : -18.0;
		const double twiAltRise = set.sunrise ? set.sunrise->sunAltitude : -6.0;
		const double twiAltSet  = set.sunset  ? set.sunset->sunAltitude  : -6.0;

		plan.ccmList.clear();
		plan.events.clear();

		// --- 1分刻みでサンプリングして撮影制御方法を分類 → 区間統合 ---
		const double stepDays = 60.0 / 86400.0;
		// 上昇/下降判定の初期値として1ステップ前の高度を用意
		double prevAlt = sunHorizAt(Astronomy_AddDays(tStart, -stepDays), obs).altitude;

		hgc::ccmType runType = hgc::ccmType::invalid;
		astro_time_t runStart = tStart;

		auto flushRun = [&](astro_time_t runEnd)
		{
			if (runType == hgc::ccmType::invalid) { return; }
			hgc::ccmWindow w;
			w.start = fromAstro(runStart, off);
			w.end   = fromAstro(runEnd, off);
			w.ccm   = makeCcm(set, runType);
			plan.ccmList.push_back(std::move(w));
		};

		for (astro_time_t t = tStart; t.ut <= tEnd.ut + 1e-9; t = Astronomy_AddDays(t, stepDays))
		{
			horiz s = sunHorizAt(t, obs);
			bool rising = (s.altitude - prevAlt) >= 0.0;
			double twiAlt = rising ? twiAltRise : twiAltSet;
			hgc::ccmType ct = classify(s.altitude, rising, inFrame(s, plan.azimuth, plan.elevation, f), nightAlt, twiAlt);
			prevAlt = s.altitude;

			if (ct != runType)
			{
				flushRun(t);
				runType = ct;
				runStart = t;
			}
		}
		flushRun(tEnd);

		// --- イベント(時刻)算出 ---
		plan.events.push_back({ hgc::csEvent::start, plan.start });
		addRiseSetEvents(plan, obs, BODY_SUN,  hgc::csEvent::sunset,  DIRECTION_SET,  tStart, tEnd, off);
		addRiseSetEvents(plan, obs, BODY_SUN,  hgc::csEvent::sunrise, DIRECTION_RISE, tStart, tEnd, off);
		addRiseSetEvents(plan, obs, BODY_MOON, hgc::csEvent::moonset, DIRECTION_SET,  tStart, tEnd, off);
		addRiseSetEvents(plan, obs, BODY_MOON, hgc::csEvent::moonrise,DIRECTION_RISE, tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::civilDusk,        DIRECTION_SET,   -6.0,  tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::nauticalDusk,     DIRECTION_SET,  -12.0,  tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::astronomicalDusk, DIRECTION_SET,  -18.0,  tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::astronomicalDawn, DIRECTION_RISE, -18.0,  tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::nauticalDawn,     DIRECTION_RISE, -12.0,  tStart, tEnd, off);
		addAltitudeEvents(plan, obs, hgc::csEvent::civilDawn,        DIRECTION_RISE,  -6.0,  tStart, tEnd, off);
		plan.events.push_back({ hgc::csEvent::end, plan.end });

		std::sort(plan.events.begin(), plan.events.end(),
		          [](const hgc::eventItem& a, const hgc::eventItem& b) { return sortKey(a.when) < sortKey(b.when); });

		return ERR_HGC_OK;
	}
}
