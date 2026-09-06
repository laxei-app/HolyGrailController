// スマホ内蔵カメラを所持カメラへ自動で登録する(2026-09-05)。
//
// 【なぜ自動なのか】外付けのカメラは「その個体を持っているか」が分からないので、
//  登録してよいか人に聞く。内蔵カメラは端末そのものなので聞く意味が無い。複数のカメラを
//  持つ端末では、それぞれを別のカメラとして並べる(ユーザー指示 2026-09-05)。
//
// 【在否監視には乗せない】内蔵カメラはネットワークの向こうに居ないので、
//  presenceMonitor は "builtin" を弾く。そのため未登録カメラの登録プロンプト
//  (reconcileDiscoveredCameras)にも乗らない。ここが唯一の登録経路になる。
//
// 【スマホ用の撮影制御方法の初期値もここで作る(2026-09-06 仕様)】
//  初期値(プリセット)は「外部カメラ用」(出荷時のコード生成)と「スマホ用」の2組を用意する。
//  スマホ用はカメラの実力(設定可能な iso/ss/F の並び・NPF)から組み立てるので、
//  端末に依る=ここ(内蔵カメラの登録)でしか作れない。名前は UI から受け取る
//  (将来の言語対応を UI 側だけで済ませるため)。
#include "detectBuiltin.h"
#include "apiBuiltin.h"
#include "device.h"
#include "dataManager.h"
#include "holyGrailEntity.h"
#include "csJson.h"
#include "exposureMath.h"
#include <json/nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace
{
	using json = nlohmann::json;

	// 露出をカメラの設定可能値へ吸着させる。ひな形の初期値や限界が、そのカメラに
	//  存在しない値のままだと、撮影開始時にいちばん近い値へ飛んで意図とずれる。
	void snapExposure(hgc::exposure& e, const expo::expoTables& t)
	{
		if (e.iso.empty() && e.ss.empty() && e.fn.empty()) { return; }
		expo::exposureCtl ctl;
		const hgc::exposure noLim{};
		const hgc::exposureType pri[hgc::exposureTypeNum] =
			{ hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		ctl.init(t, noLim, noLim, pri);
		ctl.setCurrent(e);	// いちばん近い目盛りへ吸着する
		e = ctl.current();
	}

	// ── 設定可能値の並び(カメラが答えた文字列)から値を選ぶ ──────────────
	// 段の差(log2 比)が最小のもの。空なら "" 。
	std::string nearestIn(const std::vector<std::string>& list, double target, expo::expoKind k)
	{
		const std::string* best = nullptr; double bestDiff = 1e300;
		for (const auto& s : list)
		{
			const double r = expo::parseValue(s, k);
			if (r <= 0.0 || target <= 0.0) { continue; }
			const double d = std::fabs(std::log2(r / target));
			if (d < bestDiff) { bestDiff = d; best = &s; }
		}
		return best ? *best : std::string();
	}
	// limit 未満で最大のもの。無ければ最小のもの。
	std::string maxBelow(const std::vector<std::string>& list, double limit, expo::expoKind k)
	{
		const std::string* best = nullptr; double bestR = -1.0;
		const std::string* lo = nullptr;   double loR = 1e300;
		for (const auto& s : list)
		{
			const double r = expo::parseValue(s, k);
			if (r <= 0.0) { continue; }
			if (r < loR) { loR = r; lo = &s; }
			if (r < limit && r > bestR) { bestR = r; best = &s; }
		}
		if (best) { return *best; }
		return lo ? *lo : std::string();
	}
	std::string minOf(const std::vector<std::string>& list, expo::expoKind k)
	{
		const std::string* lo = nullptr; double loR = 1e300;
		for (const auto& s : list) { const double r = expo::parseValue(s, k); if (r > 0.0 && r < loR) { loR = r; lo = &s; } }
		return lo ? *lo : std::string();
	}
	std::string maxOf(const std::vector<std::string>& list, expo::expoKind k)
	{
		const std::string* hi = nullptr; double hiR = -1.0;
		for (const auto& s : list) { const double r = expo::parseValue(s, k); if (r > 0.0 && r > hiR) { hiR = r; hi = &s; } }
		return hi ? *hi : std::string();
	}

	// UI から受け取る名前(型ごと)。無ければ英語の既定。
	struct phoneNames
	{
		std::string night = "Night phone", sunrise = "Sunrise phone";
		std::string sunset = "Sunset phone", day = "Daylight phone";
	};
	phoneNames parseNames(const std::string& namesJson)
	{
		phoneNames n;
		json j = json::parse(namesJson, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return n; }
		if (j.value("night",   std::string()).size()) { n.night   = j["night"]; }
		if (j.value("sunrise", std::string()).size()) { n.sunrise = j["sunrise"]; }
		if (j.value("sunset",  std::string()).size()) { n.sunset  = j["sunset"]; }
		if (j.value("day",     std::string()).size()) { n.day     = j["day"]; }
		return n;
	}

	// ── スマホ用の撮影制御方法一式を、そのカメラの実力から組み立てる(2026-09-06 仕様) ──
	//  夜間      : ss=算出した NPF 未満の最大値 / F=設定できる最小値 / ISO=1600(に最寄り)
	//  朝日・夕日: 暗所限界=夜間の値、基準=暗所限界、明所限界= ISO100 / 1/16000(カメラがそこまで
	//              速くなければ最速) / 最大F。順は iso→ss→F。ev -3.0、平滑化 0.5ev・3コマ
	//  日中      : 朝日と同じ限界、ev 0.0
	//  ss の暗所限界は api の上限に関わらず 48 秒まで(apiBuiltin の並びが加算で 48 秒まで持つ)。
	hgc::exposure nightExposureFor(const apiBuiltin& api, double sensorWmm, uint32_t pixelW)
	{
		hgc::exposure e;
		const double npf = expo::npfShutterSec(sensorWmm, static_cast<double>(pixelW), api.focalMm(), api.aperture());
		e.ss  = (npf > 0.0) ? maxBelow(api.ssList(), npf, expo::expoKind::ss)
		                    : nearestIn(api.ssList(), 24.0, expo::expoKind::ss);
		e.fn  = minOf(api.fnList(), expo::expoKind::fn);
		e.iso = nearestIn(api.isoList(), 1600.0, expo::expoKind::iso);
		return e;
	}
	hgc::exposure brightLimitFor(const apiBuiltin& api)
	{
		hgc::exposure e;
		e.iso = nearestIn(api.isoList(), 100.0, expo::expoKind::iso);
		const std::string fastest = minOf(api.ssList(), expo::expoKind::ss);
		const double camMin = expo::parseValue(fastest, expo::expoKind::ss);
		const double want   = 1.0 / 16000.0;
		e.ss = (camMin > want) ? fastest : nearestIn(api.ssList(), want, expo::expoKind::ss);
		e.fn = maxOf(api.fnList(), expo::expoKind::fn);
		return e;
	}
	void buildPhoneSet(const apiBuiltin& api, double sensorWmm, uint32_t pixelW,
	                   const phoneNames& nm, astro::ccmSet& set)
	{
		const hgc::exposure dark   = nightExposureFor(api, sensorWmm, pixelW);
		const hgc::exposure bright = brightLimitFor(api);

		auto night = std::make_shared<hgc::ccmNight>();
		night->name = nm.night; night->forPhone = true;
		night->limitBright = night->limitDark = night->initial = dark;
		set.night = night;

		auto sunrise = std::make_shared<hgc::ccmSunrise>();
		sunrise->name = nm.sunrise; sunrise->forPhone = true;
		sunrise->limitBright = dark; sunrise->limitDark = bright; sunrise->initial = dark;
		sunrise->ev = -3.0; sunrise->hysteresis = 0.5; sunrise->movingAverage = 3;
		set.sunrise = sunrise;

		auto sunset = std::make_shared<hgc::ccmSunset>();
		sunset->name = nm.sunset; sunset->forPhone = true;
		sunset->limitBright = dark; sunset->limitDark = bright; sunset->initial = dark;
		sunset->ev = -3.0; sunset->hysteresis = 0.5; sunset->movingAverage = 3;
		set.sunset = sunset;

		auto day = std::make_shared<hgc::ccmDay>();
		day->name = nm.day; day->forPhone = true;
		day->limitBright = dark; day->limitDark = bright; day->initial = dark;
		day->ev = 0.0;
		set.day = day;
	}

	// 見つかったカメラのうち、焦点距離がいちばん短いもの(スマホ用初期値の元にする)。
	const apiBuiltin* shortestLens(const std::vector<class device>& cams, const class device** dev)
	{
		const apiBuiltin* best = nullptr;
		for (const auto& d : cams)
		{
			const apiBuiltin* api = dynamic_cast<const apiBuiltin*>(d.apiBase.get());
			if (api == nullptr || api->focalMm() <= 0.0) { continue; }
			if (best == nullptr || api->focalMm() < best->focalMm()) { best = api; *dev = &d; }
		}
		return best;
	}

	// スマホ用の初期値(プリセット)を型ごとに1件ずつ作り、すべて「優先的な初期値」にする。
	void makePhonePresets(const std::vector<class device>& cams, const phoneNames& nm)
	{
		const class device* dev = nullptr;
		const apiBuiltin* api = shortestLens(cams, &dev);
		if (api == nullptr || dev == nullptr) { return; }
		double wmm = 0.0, hmm = 0.0; uint32_t px = 0, py = 0;
		dev->apiBase->readSensorSpec(wmm, hmm, px, py);

		astro::ccmSet set;
		buildPhoneSet(*api, wmm, px, nm, set);
		struct one { const char* key; std::shared_ptr<hgc::ccmBase> c; } list[4] = {
			{ "night", set.night }, { "sunrise", set.sunrise }, { "sunset", set.sunset }, { "day", set.day } };
		for (const auto& o : list)
		{
			if (!o.c) { continue; }
			dataManager::setCcmPresetJson(o.key, o.c->name, csjson::ccmToJson(*o.c));
			dataManager::setPreferredCcm(o.key, o.c->name);
		}
		char b[224];
		std::snprintf(b, sizeof(b), "phone presets ready from %s (%.1fmm): night %s %s %s / bright %s %s %s",
		              dev->model.c_str(), api->focalMm(),
		              set.night->limitBright.iso.c_str(), set.night->limitBright.ss.c_str(), set.night->limitBright.fn.c_str(),
		              set.day->limitDark.iso.c_str(), set.day->limitDark.ss.c_str(), set.day->limitDark.fn.c_str());
		dataManager::logEvent("GEAR", b);
	}
}

namespace builtinCam
{
	// スマホ用の撮影計画ひな形を、内蔵カメラ1台につき1つ用意する(2026-09-05)。
	//
	// 【なぜ要るか】スマホの撮影周期は熱の都合で30秒以上にしたい。レンズや露出の値も
	//  そのカメラのものでないと NPF も撮影シミュレーションも出せない。出荷時の固定計画は
	//  キヤノン機を想定した値なので、そのままでは使えない。
	//
	// 【カメラごとに作る】広角と超広角では画角も集光力も最長露光も違う。1つにまとめると
	//  どちらかに合わない値になるので、**持っているカメラの数だけ**作る(2026-09-05 ユーザー指示)。
	//
	// 【端末ごとに中身が変わる】名前も焦点距離も露出の実力も端末で違うので、資産として
	//  同梱できない。登録の直後にその端末の実力から組み立てる。
	//  同じ名前が既にあれば何もしない(消したものを起動のたびに作り直さない)。
	//
	// 【撮影制御方法はスマホ用の初期値と同じ作りで、そのカメラに最適化する(2026-09-06 仕様)】
	//  NPF・設定可能範囲・刻みはカメラごとに違うので、初期値をそのまま写さず同じ規則で
	//  そのカメラの実力から組み立てる(=初期値を取り込んでカメラに応じて最適化したもの)。
	void makeTemplate(const std::vector<class device>& cams, const phoneNames& nm)
	{
		for (const auto& d : cams)
		{
			const apiBuiltin* api = dynamic_cast<const apiBuiltin*>(d.apiBase.get());
			if (api == nullptr) { continue; }

			hgc::cs cs;
			dataManager::factoryFixedPlan(cs);
			// 【頭は英語(2026-08-22 の決まり)】Entity と通信路に日本語を置かない。
			//  後ろに付くカメラ名は端末が答えた「持ち物の名前」なので、そのまま使う。
			cs.name     = "Phone night sky - " + d.model;
			// 【窓を持たせる(2026-09-06)】出荷時の固定計画は開始/終了が 0 で、そのまま保存すると
			//  ひな形を選んだときのスケジュール生成が失敗し(終了≦開始)、選べない。
			//  夜空のひな形なので今日 20:00 〜 翌 04:00 にする(計画を作るときは日付だけ今日へ寄る)。
			{
				const int off = cs.place.tzOffMin;
				hgc::dateTime st = hgc::fromUnixUtc(static_cast<long long>(std::time(nullptr)), off);
				st.hour = 20; st.min = 0; st.sec = 0;
				cs.start = st;
				cs.end   = hgc::fromUnixUtc(hgc::toUnixUtc(st, off) + 8 * 3600, off);
			}
			// 端末の割り当ては撮影計画(cs)ではなくスマホ側が計画ごとに持つ。ひな形から作った
			//  計画は割り当てが無い=スマホになるので、ここで指定するものは無い。

			// カメラは所持カメラから引く(iso/ss の並びもそこに入っている)。
			hgc::camera oc;
			if (dataManager::findOwnedCamera(d.model, oc)) { cs.camera = oc; }
			else { cs.camera.name = d.model; cs.camera.model = d.model; }

			// 【レンズも所持レンズとして登録する(2026-09-05 ユーザー指示)】
			//  内蔵カメラのレンズは交換できず機材マスタにも載らないが、諸元は端末が答える。
			//  計画に値を埋めるだけだと、所持レンズの一覧には別機種のレンズしか無く、
			//  画面で選び直したときに合わない値になる。**一覧にも実体を置いて割り当てる**。
			hgc::lens ln{};
			ln.maker       = "builtin";
			ln.name        = d.model;	// カメラと1対1なので同じ名前でよい
			ln.focalLength = api->focalMm();
			ln.fn          = api->aperture();
			ln.fnMax       = api->aperture();	// 絞りは固定
			ln.hasContact  = false;
			ln.readOnly    = true;				// 端末が答えた値。直す余地が無い(削除は可。2026-09-06 ユーザー指示)
			if (dataManager::addOwnedLens(ln))
			{
				char lb[160];
				std::snprintf(lb, sizeof(lb), "builtin lens registered: %s (%.2fmm F%.1f)",
				              ln.name.c_str(), ln.focalLength, ln.fn);
				dataManager::logEvent("GEAR", lb);
			}
			// 所持カメラの「組み合わせるレンズ」へ割り当てる。これで、計画でこのカメラを
			//  選んだときにレンズも一緒に付いてくる(hge_setPlanCamera)。
			dataManager::setOwnedCameraLens(cs.camera.name, ln.name);
			hgc::lens ol;
			cs.lens = dataManager::findOwnedCameraDefaultLens(cs.camera.name, ol) ? ol : ln;

			// 撮影制御方法: このカメラの実力(NPF・並び)で組み立てる。
			double wmm = 0.0, hmm = 0.0; uint32_t px = 0, py = 0;
			d.apiBase->readSensorSpec(wmm, hmm, px, py);
			astro::ccmSet set;
			buildPhoneSet(*api, wmm, px, nm, set);
			cs.ccm.night = set.night; cs.ccm.sunrise = set.sunrise;
			cs.ccm.sunset = set.sunset; cs.ccm.day = set.day;

			// 値は並びから選んでいるので目盛りに乗っているが、念のため吸着させておく。
			expo::expoTables t;
			t.iso = expo::buildTable(api->isoList(), expo::expoKind::iso);
			t.ss  = expo::buildTable(api->ssList(),  expo::expoKind::ss);
			t.fn  = expo::buildTable(api->fnList(),  expo::expoKind::fn);
			for (const hgc::ccmType ty : { hgc::ccmType::night, hgc::ccmType::sunrise,
			                               hgc::ccmType::sunset, hgc::ccmType::day })
			{
				std::shared_ptr<hgc::ccmBase> c = cs.ccm.get(ty);
				if (!c) { continue; }
				snapExposure(c->initial,     t);
				snapExposure(c->limitBright, t);
				snapExposure(c->limitDark,   t);
			}
			cs.nightFixedExposure = set.night->limitBright;

			// 撮影周期: 熱の都合で 30 秒以上(2026-09-05 ユーザー判断)。加算で長い ss を使うときは
			//  カメラの規則(最長 ss × 係数 + 余裕)がそれを超えるので、大きいほうを採る。
			{
				const double maxSs  = expo::parseValue(cs.nightFixedExposure.ss, expo::expoKind::ss);
				const double factor = (cs.camera.intervalFactor > 0.0) ? cs.camera.intervalFactor : 1.0;
				const double margin = (cs.camera.intervalFactor > 0.0) ? cs.camera.intervalMargin : 2.0;
				const double need   = (maxSs > 0.0) ? std::ceil(maxSs * factor + margin) : 0.0;
				cs.interval = (need > 30.0) ? need : 30.0;
			}

			if (hge_saveTemplateJsonIfAbsent(csjson::toJson(cs).c_str()) == ERR_HGC_OK)
			{
				char b[224];
				std::snprintf(b, sizeof(b),
				              "phone template ready: %s (%.1fmm F%.1f / %.0fs cycle / night %s %s %s)",
				              cs.name.c_str(), cs.lens.focalLength, cs.lens.fn, cs.interval,
				              cs.nightFixedExposure.iso.c_str(), cs.nightFixedExposure.ss.c_str(),
				              cs.nightFixedExposure.fn.c_str());
				dataManager::logEvent("GEAR", b);
			}
		}
	}

	// 端末のカメラを一通り見て、所持カメラ・所持レンズ・スマホ用初期値・ひな形を用意する。
	//  **戻り=見つかったカメラの台数**(足した数ではない)。呼ぶ側はこれで
	//  「用意し終えたか(=もう二度としなくてよいか)」を判断する。0 のときは
	//  カメラの権限がまだ無いなどの理由で列挙できていないので、次の起動でやり直す。
	//  既にあるものは触らない(名前や値をユーザーが変えていることがある)。
	//  namesJson: 初期値の名前 {"night":..,"sunrise":..,"sunset":..,"day":..}(UI の言語で)。
	int registerAll(const std::string& namesJson)
	{
		detectBuiltin det;
		std::vector<class device> found;
		det.detect(found);	// apiBase まで作る。設定可能値はそこから採る

		int added = 0;
		for (auto& d : found)
		{
			if (d.serialno.empty()) { continue; }
			const int r = dataManager::recordConnectedCameraStatus(d, true);
			if (r == static_cast<int>(dataManager::camApply::isNew)) { ++added; }

			// センサーの寸法と画素数は端末が答える。マスタに無い機種は空のままになるので、
			//  ここで埋めておく(NPF と撮影シミュレーションがそのまま使える)。
			if (d.apiBase)
			{
				double wmm = 0.0, hmm = 0.0; uint32_t px = 0, py = 0;
				if (d.apiBase->readSensorSpec(wmm, hmm, px, py) == ERR_HGC_OK)
				{
					dataManager::fillOwnedCameraSensor(d.serialno, wmm, hmm, px, py);
				}
			}
		}
		if (added > 0)
		{
			dataManager::logEvent("GEAR",
				("builtin cameras registered: " + std::to_string(added)).c_str());
		}
		const phoneNames nm = parseNames(namesJson);
		makePhonePresets(found, nm);
		makeTemplate(found, nm);
		// 【新規計画の初期カメラ(2026-09-06 ユーザー指示)】スマホ用初期値の元にした(焦点距離が最短の)
		//  内蔵カメラに「撮影計画の初期値にする」を入れる。利用者が既に別のカメラを選んでいれば触らない。
		{
			hgc::camera cur;
			const class device* dev = nullptr;
			if (!dataManager::autoInsertCamera(cur) && shortestLens(found, &dev) != nullptr && dev != nullptr)
			{
				dataManager::setOwnedCameraAutoInsert(dev->model, true);
				dataManager::logEvent("GEAR", ("plan default camera: " + dev->model).c_str());
			}
		}
		return static_cast<int>(found.size());
	}
}
