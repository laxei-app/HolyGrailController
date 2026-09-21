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
#include "stdTemplates.h"
#include <json/nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace
{
	using json = nlohmann::json;

	// 【並びから選ぶのをやめた(2026-09-19)】内蔵カメラの ss と ISO は無段になり、
	//  設定できる値の並びは持たない。欲しい値は「段」で決めて、デバイスに解決させる
	//  (apiBase::expoResolve が範囲で止め、その端末が出せる値にする)。
	//  以前の nearestIn / maxBelow / minOf / maxOf / snapExposure は役目を終えたので外した。
	hgc::exposure resolveReal(apiBuiltin& api, double isoWant, double ssWant, double fnWant)
	{
		apiBase::expoPoint want;
		if (isoWant > 0.0) { want.iso = expo::stopsOfReal(isoWant, expo::expoKind::iso); want.hasIso = true; }
		if (ssWant  > 0.0) { want.ss  = expo::stopsOfReal(ssWant,  expo::expoKind::ss);  want.hasSs  = true; }
		if (fnWant  > 0.0) { want.fn  = expo::stopsOfReal(fnWant,  expo::expoKind::fn);  want.hasFn  = true; }
		hgc::exposure out; apiBase::expoPoint got;
		api.expoResolve(want, out, got);
		return out;
	}

	// 撮影制御方法の名前(UI の言語で渡ってくる。Entity は文言を持たない)。
	struct phoneNames
	{
		std::string night, sunrise, sunset, day;
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
	hgc::exposure nightExposureFor(apiBuiltin& api, double sensorWmm, uint32_t pixelW)
	{
		// ss = NPF(点像を保つ目安)。出せなければ 24 秒。F は最も明るい絞り。ISO 1600。
		const double npf = expo::npfShutterSec(sensorWmm, static_cast<double>(pixelW),
		                                       api.focalMm(), api.apertureMin());
		return resolveReal(api, 1600.0, (npf > 0.0) ? npf : 24.0, api.apertureMin());
	}
	hgc::exposure brightLimitFor(apiBuiltin& api)
	{
		// 明所限界。ss は 1/16000 を望み、そこまで速くない端末では expoResolve が端で止める。
		return resolveReal(api, 100.0, 1.0 / 16000.0, api.apertureMax());
	}
	void buildPhoneSet(apiBuiltin& api, double sensorWmm, uint32_t pixelW,
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
	// 戻りが非 const なのは、呼ぶ側が expoResolve(デバイスの遅延構築に触る)を使うため。
	apiBuiltin* shortestLens(const std::vector<class device>& cams, const class device** dev)
	{
		apiBuiltin* best = nullptr;
		for (const auto& d : cams)
		{
			apiBuiltin* api = dynamic_cast<apiBuiltin*>(d.apiBase.get());
			if (api == nullptr || api->focalMm() <= 0.0) { continue; }
			if (best == nullptr || api->focalMm() < best->focalMm()) { best = api; *dev = &d; }
		}
		return best;
	}

	// スマホ用の初期値(プリセット)を型ごとに1件ずつ作り、すべて「優先的な初期値」にする。
	void makePhonePresets(const std::vector<class device>& cams, const phoneNames& nm)
	{
		const class device* dev = nullptr;
		apiBuiltin* api = shortestLens(cams, &dev);
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
	// 【標準ひな形を内蔵カメラ1台につき1組(光条なしの 4 種)作る(2026-09-21 ユーザー指示)】
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
	//  同じカメラ・同じ種類が既にあれば何もしない(消したものを起動のたびに作り直さない)。
	//
	// 【組み立ては共通部分(stdTemplates)】ここが答えるのは、このカメラの実力だけ:
	//  夜間の露出(NPF・開放・ISO1600)、夜景はその ss 半分、明所限界、周期の規則。
	//  光条(F11〜16 に絞る)は内蔵カメラには作らない(絞りが固定。ユーザー指示 2026-09-21)。
	void makeTemplate(const std::vector<class device>& cams, const std::string& namesJson)
	{
		const stdtpl::names nm = stdtpl::parseNames(namesJson, "");	// 撮影制御方法の名前はスマホ用初期値と同じ
		for (const auto& d : cams)
		{
			apiBuiltin* api = dynamic_cast<apiBuiltin*>(d.apiBase.get());
			if (api == nullptr) { continue; }

			stdtpl::gear g;
			// カメラは所持カメラから引く(iso/ss の並び・周期の規則もそこに入っている)。
			if (!dataManager::findOwnedCamera(d.model, g.camera)) { g.camera.name = d.model; g.camera.model = d.model; }

			// 【レンズも所持レンズとして登録する(2026-09-05 ユーザー指示)】
			//  内蔵カメラのレンズは交換できず機材マスタにも載らないが、諸元は端末が答える。
			//  計画に値を埋めるだけだと、所持レンズの一覧には別機種のレンズしか無く、
			//  画面で選び直したときに合わない値になる。**一覧にも実体を置いて割り当てる**。
			hgc::lens ln{};
			ln.maker       = "builtin";
			ln.name        = d.model;	// カメラと1対1なので同じ名前でよい
			ln.focalLength = api->focalMm();
			// 【絞りは固定とは限らない(2026-09-19)】iPhone 13 は可変で、Android にも出てくる。
			//  端末が答える並びの最小(最も明るい)と最大をそのまま入れる。1 点なら同じ値になる。
			ln.fn          = api->apertureMin();
			ln.fnMax       = api->apertureMax();
			ln.fnList      = api->fnList();	// 選べる絞りそのもの(1 点なら 1 つだけ)
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
			dataManager::setOwnedCameraLens(g.camera.name, ln.name);
			hgc::lens ol;
			g.lens = dataManager::findOwnedCameraDefaultLens(g.camera.name, ol) ? ol : ln;

			// 露出: このカメラの実力(NPF・範囲)で組み立てる。値は expoResolve が「その端末が出せる値」にする。
			double wmm = 0.0, hmm = 0.0; uint32_t px = 0, py = 0;
			d.apiBase->readSensorSpec(wmm, hmm, px, py);
			g.starNight = nightExposureFor(*api, wmm, px);
			{
				const double ssStar = expo::parseValue(g.starNight.ss, expo::expoKind::ss);
				g.cityNight = resolveReal(*api, 1600.0, (ssStar > 0.0) ? ssStar * 0.5 : 12.0, api->apertureMin());
			}
			g.bright   = brightLimitFor(*api);
			g.fnFixed  = !(api->apertureMax() > api->apertureMin() + 1e-9);
			g.forPhone = true;

			// 撮影周期: 熱の都合で 30 秒以上(2026-09-05 ユーザー判断)。加算で長い ss を使うときは
			//  カメラの規則(最長 ss × 係数 + 余裕)がそれを超えるので、大きいほうを採る。
			auto intervalFor = [&](const std::string& ss) -> double
			{
				const double maxSs  = expo::parseValue(ss, expo::expoKind::ss);
				const double factor = (g.camera.intervalFactor > 0.0) ? g.camera.intervalFactor : 1.0;
				const double margin = (g.camera.intervalFactor > 0.0) ? g.camera.intervalMargin : 2.0;
				const double need   = (maxSs > 0.0) ? std::ceil(maxSs * factor + margin) : 0.0;
				return (need > 30.0) ? need : 30.0;
			};
			g.starInterval = intervalFor(g.starNight.ss);
			g.cityInterval = intervalFor(g.cityNight.ss);

			const int made = stdtpl::seed(g, nm, false);
			char b[224];
			std::snprintf(b, sizeof(b),
			              "std templates for %s (%.1fmm F%.1f): %d made / night %s %s %s / city ss %s / %.0fs,%.0fs cycle",
			              g.camera.name.c_str(), g.lens.focalLength, g.lens.fn, made,
			              g.starNight.iso.c_str(), g.starNight.ss.c_str(), g.starNight.fn.c_str(),
			              g.cityNight.ss.c_str(), g.starInterval, g.cityInterval);
			dataManager::logEvent("GEAR", b);
		}
	}

	// 端末のカメラを一通り見て、所持カメラ・所持レンズ・スマホ用初期値・ひな形を用意する。
	//  **戻り=見つかったカメラの台数**(足した数ではない)。呼ぶ側はこれで
	//  「用意し終えたか(=もう二度としなくてよいか)」を判断する。0 のときは
	//  カメラの権限がまだ無いなどの理由で列挙できていないので、次の起動でやり直す。
	//  既にあるものは触らない(名前や値をユーザーが変えていることがある)。
	//  namesJson: 初期値の名前 {"night":..,"sunrise":..,"sunset":..,"day":.., "tpl":{標準ひな形の名前}}(UI の言語で)。
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
		makeTemplate(found, namesJson);
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
