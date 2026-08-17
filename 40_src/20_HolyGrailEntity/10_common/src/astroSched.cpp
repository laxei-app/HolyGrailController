#include "astroSched.h"
#include <astronomy/astronomy.h>
#include <algorithm>
#include <cmath>
#include <vector>

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

		// 天体の地平座標(高度/方位)
		horiz bodyHorizAt(astro_body_t body, astro_time_t t, astro_observer_t obs)
		{
			astro_equatorial_t eq = Astronomy_Equator(body, &t, obs, EQUATOR_OF_DATE, ABERRATION);
			astro_horizon_t hz = Astronomy_Horizon(&t, obs, eq.ra, eq.dec, REFRACTION_NORMAL);
			return { hz.azimuth, hz.altitude };
		}

		// 太陽の地平座標(高度/方位)
		horiz sunHorizAt(astro_time_t t, astro_observer_t obs)
		{
			return bodyHorizAt(BODY_SUN, t, obs);
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
			constexpr double sunDirectMaxAlt = 3.0;	// これ以上高い太陽は日中扱い(朝日/夕日の上限既定+3°)
			if (sunInFrame && h >= twiAlt && h <= sunDirectMaxAlt)
			{
				return rising ? hgc::ccmType::sunrise : hgc::ccmType::sunset;
			}
			if (h < nightAlt) { return hgc::ccmType::night; }
			if (h >= twiAlt)  { return hgc::ccmType::day; }
			// 薄明帯(nightAlt..twiAlt)で太陽が画角外:
			//  朝(上昇)は夜間→次の自動露出への「夜間後移行」、
			//  夕(下降)は自動露出→夜間への「夜間前移行」。
			return rising ? hgc::ccmType::postNight : hgc::ccmType::preNight;
		}

		// 種別から区間に入れる撮影制御方法を返す。
		// 夜間/朝日/夕日/日中は**計画が所有する実体をそのまま指す**(複製しない)。計画の ccm を
		// 編集すれば同じ型の窓すべてに反映される。移行(夜間前/後)はユーザー設定を持たないのでその場で作る。
		std::shared_ptr<hgc::ccmBase> makeCcm(const hgc::ccmOwned& own, hgc::ccmType ct)
		{
			switch (ct)
			{
			case hgc::ccmType::night:
			case hgc::ccmType::sunrise:
			case hgc::ccmType::sunset:
			case hgc::ccmType::day:
				if (auto c = own.get(ct)) { return c; }
				break;
			case hgc::ccmType::preNight:
			{
				auto c = std::make_shared<hgc::ccmBase>(hgc::ccmType::preNight);
				c->name = "preNight";
				return c;
			}
			case hgc::ccmType::postNight:
			{
				auto c = std::make_shared<hgc::ccmBase>(hgc::ccmType::postNight);
				c->name = "postNight";
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

	horiz moonHoriz(const hgc::dateTime& localTime, int utcOffsetMin, const hgc::place& p)
	{
		astro_observer_t obs = Astronomy_MakeObserver(p.latitude, p.longitude, p.altitude);
		return bodyHorizAt(BODY_MOON, toAstro(localTime, utcOffsetMin), obs);
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
		double sh = (cam.sensorSizeV > 0.0) ? cam.sensorSizeV : sw * 2.0 / 3.0;	// 縦が未取得なら 3:2 を仮定
		double fovW = 2.0 * std::atan2(sw / 2.0, fl) * DEG;
		double fovH = 2.0 * std::atan2(sh / 2.0, fl) * DEG;
		if (landscape) { return { fovW, fovH }; }
		return { fovH, fovW };			// 縦位置は長辺・短辺が入れ替わる
	}

	errCode buildSchedule(hgc::cs& plan, int utcOffsetMin)
	{
		const int off = utcOffsetMin;
		astro_observer_t obs = Astronomy_MakeObserver(plan.place.latitude, plan.place.longitude, plan.place.altitude);
		astro_time_t tStart = toAstro(plan.start, off);
		astro_time_t tEnd   = toAstro(plan.end, off);
		if (tEnd.ut <= tStart.ut) { return ERR_HGC_INVALID_ARG; }

		const fov f = calcFov(plan.camera, plan.lens, plan.landscape);
		// 撮影制御方法の開始/終了高度はスケジュール画面の境界で決める(ccm画面のスライダーは廃止)。
		// 既定の高度帯: 夜間 <-12° / 夜間前後移行 -12°〜0° / 朝日・夕日(直接撮影) 0°〜+3° / 日中 >+3°。
		// 各境界はスケジュールで可動(夜間境界 -19〜-6, 移行↔朝日夕日 -1〜+2, 朝日夕日↔日中 +3〜+6)。
		// 夜間境界の既定は -18°(天文薄明)だったが -12°(航海薄明)へ変更した(2026-08-17 ユーザー指示)。
		//  -18°まで粘ると移行区間が長くなりすぎ、夜間の固定露出へ着地するまでに薄明が終わってしまう。
		const double nightAlt   = -12.0;	// 夜間の上限(既定。スケジュールで -19〜-12 に可動)
		const double twiAltRise = 0.0;		// 朝日/夕日帯の下限(既定)
		const double twiAltSet  = 0.0;

		plan.ccmList.clear();
		plan.events.clear();

		// --- 1分刻みでサンプリングして撮影制御方法を分類 → 区間統合 ---
		const double stepDays = 60.0 / 86400.0;
		constexpr double sunDirectMaxAlt = 3.0;	// これ以上高い太陽は日中扱い(朝日/夕日の上限既定+3°)

		// 第1パス: 全サンプルの高度・上昇/下降・画角侵入を収集する。
		// 時刻は ut(double) のみ保持し、必要時に Astronomy_TimeFromDays(ut) で astro_time_t へ復元する。
		// astro_time_t(約56B)をサンプルごとに持つと小RAM機(StickS3=内蔵のみ)で vector が溢れて
		// bad_alloc → abort するため。さらに reserve でリアロケーション(倍々=一時ピーク3倍)を避ける。
		// 項目11: 画角侵入(inFrame)の判定は廃止したので保持しない(帯は太陽高度だけで決める)。
		struct Sample { double ut; double h; bool rising; };
		std::vector<Sample> samples;
		samples.reserve(static_cast<size_t>((tEnd.ut - tStart.ut) / stepDays) + 2);
		double prevAlt = sunHorizAt(Astronomy_AddDays(tStart, -stepDays), obs).altitude;
		for (astro_time_t t = tStart; t.ut <= tEnd.ut + 1e-9; t = Astronomy_AddDays(t, stepDays))
		{
			horiz s = sunHorizAt(t, obs);
			bool rising = (s.altitude - prevAlt) >= 0.0;
			samples.push_back({ t.ut, s.altitude, rising });
			prevAlt = s.altitude;
		}

		// 第2パス: 種別を決める(項目1)。直接撮影候補帯(高度 twiAlt..+12°, 上昇/下降が一定の連続区間)は、
		// その帯のあいだ一度でも太陽が画角に入れば帯まるごと朝日/夕日、一度も入らなければ帯まるごと日中。
		// 帯外(深い薄明 nightAlt..twiAlt は夜間後/前移行、夜間、+12°超は日中)は従来どおり高度で分類する。
		std::vector<hgc::ccmType> types(samples.size(), hgc::ccmType::invalid);
		for (size_t i = 0; i < samples.size(); )
		{
			const double twiAlt = samples[i].rising ? twiAltRise : twiAltSet;
			const bool isBand = (samples[i].h >= twiAlt && samples[i].h <= sunDirectMaxAlt);
			if (!isBand)
			{
				const double h = samples[i].h;
				hgc::ccmType ct;
				// 夜間を「使わない」なら移行として扱う(4種とも使う/使わないを持つため。ただし
				// 夜間/日中を外すと時間帯に穴が空くので UI では切り替えさせない)。
				if (h < nightAlt)             { ct = plan.ccm.used(hgc::ccmType::night)
				                                     ? hgc::ccmType::night
				                                     : (samples[i].rising ? hgc::ccmType::postNight : hgc::ccmType::preNight); }
				else if (h > sunDirectMaxAlt) { ct = hgc::ccmType::day; }
				else /* nightAlt..twiAlt 薄明 */ { ct = samples[i].rising ? hgc::ccmType::postNight : hgc::ccmType::preNight; }
				types[i] = ct;
				++i;
				continue;
			}
			// 上昇/下降が一定の帯を切り出し、帯全体の種別を決める。
			const bool rising = samples[i].rising;
			const double ta = rising ? twiAltRise : twiAltSet;
			size_t j = i;
			while (j < samples.size() && samples[j].rising == rising &&
			       samples[j].h >= ta && samples[j].h <= sunDirectMaxAlt)
			{
				++j;
			}
			// 朝日/夕日を「使う」かは計画が持つ(2026-08-11)。使わないなら日中として扱う。
			// かつては「太陽が画角に入るか」で自動判定していたが、ユーザーが決める方式へ変えたので廃止した。
			// これにより画角の向き次第で朝日帯が日中を分断する既知の副作用も無くなった。
			const hgc::ccmType want = rising ? hgc::ccmType::sunrise : hgc::ccmType::sunset;
			const hgc::ccmType ct   = plan.ccm.used(want) ? want : hgc::ccmType::day;
			for (size_t k = i; k < j; ++k) { types[k] = ct; }
			i = j;
		}

		// 種別の連続区間を窓へ統合する。
		hgc::ccmType runType = hgc::ccmType::invalid;
		astro_time_t runStart = tStart;
		// 種別が変わる境目の時刻を、実際の高度しきい値へ寄せる(2026-08-17)。
		//  サンプルは1分刻みなので、そのまま使うと最大1分ぶん行き過ぎる。太陽高度にして約0.2°で、
		//  夜間の始まりが -12.2°、終わりが -11.8° と表示されていた(既定 -12° のはずが揃わない)。
		//  隣り合う2サンプルの間に入りうるしきい値は高々1つなので(しきい値どうしは3°以上離れており、
		//  1分で太陽が動くのは約0.2°)、その1つを線形補間で求める。
		auto refineUt = [&](size_t i) -> double
		{
			if (i == 0) { return samples[i].ut; }
			const double h0 = samples[i - 1].h, h1 = samples[i].h;
			const double d  = h1 - h0;
			if (std::fabs(d) < 1e-9) { return samples[i].ut; }
			const double lo = (h0 < h1) ? h0 : h1;
			const double hi = (h0 < h1) ? h1 : h0;
			const double cand[3] = { nightAlt, samples[i].rising ? twiAltRise : twiAltSet, sunDirectMaxAlt };
			for (double th : cand)
			{
				if (th <= lo || th >= hi) { continue; }
				const double r = (th - h0) / d;	// 0..1
				return samples[i - 1].ut + (samples[i].ut - samples[i - 1].ut) * r;
			}
			return samples[i].ut;	// しきい値以外の理由で変わった(画角侵入等)ならサンプル位置のまま
		};
		auto flushRun = [&](astro_time_t runEnd)
		{
			if (runType == hgc::ccmType::invalid) { return; }
			hgc::ccmWindow w;
			w.start = fromAstro(runStart, off);
			w.end   = fromAstro(runEnd, off);
			w.type  = runType;
			w.ccm   = makeCcm(plan.ccm, runType);
			plan.ccmList.push_back(std::move(w));
		};
		for (size_t i = 0; i < samples.size(); ++i)
		{
			if (types[i] != runType)
			{
				const astro_time_t bt = Astronomy_TimeFromDays(refineUt(i));
				flushRun(bt);
				runType = types[i];
				runStart = bt;
			}
		}
		flushRun(tEnd);

		// --- 境目の時刻上書き(7.3.2)。隣接窓の (before,after) 型ペアの occ 番目を when へ動かす。---
		auto typeAt = [&](size_t idx) -> hgc::ccmType {
			return plan.ccmList[idx].ccm ? plan.ccmList[idx].ccm->type : hgc::ccmType::invalid;
		};
		for (size_t i = 0; i + 1 < plan.ccmList.size(); ++i)
		{
			const hgc::ccmType bt = typeAt(i);
			const hgc::ccmType at = typeAt(i + 1);
			// この型ペアの出現順(これ以前に同じ (bt,at) 境目がいくつあったか)。
			uint16_t occ = 0;
			for (size_t k = 0; k < i; ++k) { if (typeAt(k) == bt && typeAt(k + 1) == at) { ++occ; } }
			for (const auto& bo : plan.boundaries)
			{
				if (bo.before != bt || bo.after != at || bo.occ != occ) { continue; }
				astro_time_t lo = toAstro(plan.ccmList[i].start, off);
				astro_time_t hi = toAstro(plan.ccmList[i + 1].end, off);
				const double minGap = 60.0 / 86400.0;	// 1分は最低残す
				// 適用時刻の決定。境目は「太陽高度」で保持しているので、現在の隣接窓ペアの区間
				// [窓i開始, 窓i+1終了] の中でその高度になる時刻を探す。撮影日時を変えても正しい高度で
				// 再適用でき、区間外(=別日付/別窓に紐づく古い指定)は適用しない(=自動のまま)ので壊れない。
				double wut = -1.0;
				if (bo.altDeg > -90.0)
				{
					for (size_t s = 1; s < samples.size(); ++s)
					{
						if (samples[s].ut < lo.ut || samples[s].ut > hi.ut) { continue; }
						if (samples[s].rising != bo.rising) { continue; }	// 昇降方向が一致する交差のみ
						if ((samples[s - 1].h - bo.altDeg) * (samples[s].h - bo.altDeg) <= 0.0) { wut = samples[s].ut; break; }
					}
				}
				else
				{
					// 旧データ(高度未保存): when を使う。ただし区間外(古い日付)は無視して自動のまま。
					astro_time_t w = toAstro(bo.when, off);
					if (w.ut >= lo.ut && w.ut <= hi.ut) { wut = w.ut; }
				}
				if (wut < 0.0) { break; }						// 区間内に該当高度なし→自動(壊さない)
				if (wut < lo.ut + minGap) { wut = lo.ut + minGap; }
				if (wut > hi.ut - minGap) { wut = hi.ut - minGap; }
				if (wut <= lo.ut || wut >= hi.ut) { break; }	// 縮退(隣接窓が短すぎ)→無視
				astro_time_t wc = Astronomy_AddDays(lo, wut - lo.ut);
				hgc::dateTime nd = fromAstro(wc, off);
				plan.ccmList[i].end       = nd;
				plan.ccmList[i + 1].start = nd;
				break;
			}
		}

		// 開始(plan.start)直前に効いていたはずの撮影制御方法を求める(移行中開始の1枚目シード用)。
		// 最初の窓と異なる種別が出るまで tStart から後方へ走査する(夜間前→日中/夕日, 夜間後→夜間)。
		plan.startLeadCcm.reset();
		if (!plan.ccmList.empty() && plan.ccmList.front().ccm)
		{
			hgc::ccmType firstType = plan.ccmList.front().ccm->type;
			for (int i = 1; i <= 6 * 60; ++i)	// 最大6時間ぶん遡る
			{
				astro_time_t t  = Astronomy_AddDays(tStart, -stepDays * i);
				astro_time_t tp = Astronomy_AddDays(t, -stepDays);
				horiz s  = sunHorizAt(t,  obs);
				horiz sp = sunHorizAt(tp, obs);
				bool rising = (s.altitude - sp.altitude) >= 0.0;
				double twiAlt = rising ? twiAltRise : twiAltSet;
				hgc::ccmType ct = classify(s.altitude, rising,
				                           inFrame(s, plan.azimuth, plan.elevation, f), nightAlt, twiAlt);
				if (ct != firstType && ct != hgc::ccmType::invalid)
				{
					plan.startLeadCcm = makeCcm(plan.ccm, ct);
					break;
				}
			}
		}

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

		// 夜間撮影の固定露出・移行目標evを、夜間ウィンドウの有無に関わらず常に保持する(仕様3.7/3.9)。
		// 夜間前/後移行のクランプ(暗所限界)・基準(home)・ss上限の基準として captureRunner が使う。
		if (plan.ccm.night)
		{
			plan.nightFixedExposure = plan.ccm.night->limitBright;	// 夜間=固定露出(limitBright==limitDark)
			plan.nightPreNightEv    = plan.ccm.night->preNightEv;
			plan.nightPostNightEv   = plan.ccm.night->postNightEv;
		}

		return ERR_HGC_OK;
	}
}
