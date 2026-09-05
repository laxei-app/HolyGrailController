// スマホ内蔵カメラを所持カメラへ自動で登録する(2026-09-05)。
//
// 【なぜ自動なのか】外付けのカメラは「その個体を持っているか」が分からないので、見つけたら
//  登録してよいか人に聞く。内蔵カメラは端末そのものなので聞く意味が無い。複数のカメラを
//  持つ端末では、それぞれを別のカメラとして並べる(ユーザー指示 2026-09-05)。
//
// 【在否監視には乗せない】内蔵カメラはネットワークの向こうに居ないので、
//  presenceMonitor は "builtin" を弾く。そのため未登録カメラの登録プロンプト
//  (reconcileDiscoveredCameras)にも乗らない。ここが唯一の登録経路になる。
#include "detectBuiltin.h"
#include "apiBuiltin.h"
#include "device.h"
#include "dataManager.h"
#include "holyGrailEntity.h"
#include "csJson.h"
#include "exposureMath.h"
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>

namespace
{
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
}

namespace builtinCam
{
	// スマホ用の撮影計画ひな形を用意する(2026-09-05)。
	//
	// 【なぜ要るか】スマホの撮影周期は熱の都合で30秒以上にしたい。露出の限界も内蔵カメラの
	//  実力(最長露光など)に収まっている必要がある。出荷時の固定計画はキヤノン機を想定した
	//  値なので、そのままでは使えない。
	//
	// 【端末ごとに中身が変わる】カメラの名前も露出の実力も端末で違うので、資産として同梱
	//  できない。**登録の直後に、その端末の実力から組み立てる**。
	//  名前が同じひな形が既にあれば何もしない(消したものを起動のたびに作り直さない)。
	void makeTemplate(const std::vector<class device>& cams)
	{
		// 【いちばんセンサーの大きいカメラを選ぶ】星の写りを決めるのは集光力であって、
		//  前面か背面かという名前ではない。機種名にも言語にも依存しない選び方にする。
		//  (実測: Pixel 6 は背面 9.79x7.37mm に対し前面 3.67x2.76mm)
		const class device* pick = nullptr;
		const apiBuiltin*   api  = nullptr;
		double best = -1.0;
		for (const auto& d : cams)
		{
			const apiBuiltin* a = dynamic_cast<const apiBuiltin*>(d.apiBase.get());
			if (a == nullptr) { continue; }
			const double area = a->sensorArea();
			if (area > best) { best = area; pick = &d; api = a; }
		}
		if (pick == nullptr || api == nullptr) { return; }

		hgc::cs cs;
		dataManager::factoryFixedPlan(cs);
		// 【名前は英語(2026-08-22 の決まり)】Entity と通信路に日本語を置かない。
		cs.name     = "Phone night sky";
		// 端末の割り当ては撮影計画(cs)ではなくスマホ側が計画ごとに持つ。ひな形から作った
		//  計画は割り当てが無い=スマホになるので、ここで指定するものは無い。
		cs.interval = 30.0;		// 熱の都合。スマホは30秒以上(2026-09-05 ユーザー判断)

		// カメラは所持カメラから引く(iso/ss の並びもそこに入っている)。
		hgc::camera oc;
		if (dataManager::findOwnedCamera(pick->model, oc)) { cs.camera = oc; }
		else { cs.camera.name = pick->model; cs.camera.model = pick->model; }

		// レンズは交換できないので、カメラの一部として端末の値を入れる。
		//  ここが空だと NPF も撮影シミュレーションも出せない。
		cs.lens = hgc::lens{};
		cs.lens.maker       = "builtin";
		cs.lens.name        = pick->model;
		cs.lens.focalLength = api->focalMm();
		cs.lens.fn          = api->aperture();
		cs.lens.fnMax       = api->aperture();	// 絞りは固定
		cs.lens.hasContact  = false;

		// 露出をこのカメラの目盛りへ吸着させる。存在しない値を持ったひな形にしない。
		expo::expoTables t;
		t.iso = expo::buildTable(api->isoList(), expo::expoKind::iso);
		t.ss  = expo::buildTable(api->ssList(),  expo::expoKind::ss);
		t.fn  = expo::buildTable(api->fnList(),  expo::expoKind::fn);
		//  出荷時の計画は撮影制御方法の実体を持たない(読み込むときに初期値から入る)ので、
		//  ここで吸着できるのは持っているぶんだけ。持っていなくても、撮影開始時に
		//  exposureCtl がいちばん近い目盛りへ寄せるので破綻はしない。
		snapExposure(cs.nightFixedExposure, t);
		for (const hgc::ccmType ty : { hgc::ccmType::night, hgc::ccmType::sunrise,
		                               hgc::ccmType::sunset, hgc::ccmType::day })
		{
			std::shared_ptr<hgc::ccmBase> c = cs.ccm.get(ty);
			if (!c) { continue; }
			snapExposure(c->initial,     t);
			snapExposure(c->limitBright, t);
			snapExposure(c->limitDark,   t);
		}

		const int32_t r = hge_saveTemplateJsonIfAbsent(csjson::toJson(cs).c_str());
		if (r == ERR_HGC_OK)
		{
			char b[192];
			std::snprintf(b, sizeof(b), "phone template ready: %s (%s / %.1fmm F%.1f / %.0fs cycle)",
			              cs.name.c_str(), cs.camera.name.c_str(),
			              cs.lens.focalLength, cs.lens.fn, cs.interval);
			dataManager::logEvent("GEAR", b);
		}
	}

	// 端末のカメラを一通り見て、まだ所持カメラに無いものを足す。戻り=足した台数。
	//  既にあるものは触らない(名前をユーザーが変えていることがある)。
	int registerAll(void)
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
				double wmm = 0.0, hmm = 0.0; uint32_t px = 0;
				if (d.apiBase->readSensorSpec(wmm, hmm, px) == ERR_HGC_OK)
				{
					dataManager::fillOwnedCameraSensor(d.serialno, wmm, hmm, px);
				}
			}
		}
		if (added > 0)
		{
			dataManager::logEvent("GEAR",
				("builtin cameras registered: " + std::to_string(added)).c_str());
		}
		makeTemplate(found);
		return added;
	}
}
