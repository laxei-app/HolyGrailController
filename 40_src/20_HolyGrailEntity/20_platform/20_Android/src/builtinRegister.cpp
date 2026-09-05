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
	void makeTemplate(const std::vector<class device>& cams)
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
			cs.interval = 30.0;	// 熱の都合。スマホは30秒以上(2026-09-05 ユーザー判断)
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
			if (dataManager::addOwnedLens(ln))
			{
				char lb[160];
				std::snprintf(lb, sizeof(lb), "builtin lens registered: %s (%.2fmm F%.1f)",
				              ln.name.c_str(), ln.focalLength, ln.fn);
				dataManager::logEvent("GEAR", lb);
			}
			// 既に一覧にあるなら、そちら(利用者が直した値かもしれない)を使う。
			hgc::lens ol;
			cs.lens = dataManager::findOwnedLens(ln.name, ol) ? ol : ln;

			expo::expoTables t;
			t.iso = expo::buildTable(api->isoList(), expo::expoKind::iso);
			t.ss  = expo::buildTable(api->ssList(),  expo::expoKind::ss);
			t.fn  = expo::buildTable(api->fnList(),  expo::expoKind::fn);

			// 【夜間の固定露出はカメラの実力で決める】ここを空のままにすると撮影制御方法の
			//  初期値(キヤノン機向けの 8秒など)が入る。前面カメラのように最長1秒しか無い
			//  個体では届かないので、**そのカメラで出せる中から選ぶ**。
			//   ・シャッターは出せる中でいちばん長いもの(暗い空にはこれが要る)
			//   ・ISO は 1600 に近いもの(粒状感と明るさの兼ね合いの目安)
			//   ・F値は固定なのでその値
			if (!api->ssList().empty())  { cs.nightFixedExposure.ss  = api->ssList().back(); }
			if (!api->fnList().empty())  { cs.nightFixedExposure.fn  = api->fnList().front(); }
			if (!api->isoList().empty())
			{
				std::string bestIso = api->isoList().front();
				double bestDiff = -1.0;
				for (const auto& v : api->isoList())
				{
					const double r = expo::parseValue(v, expo::expoKind::iso);
					if (r <= 0.0) { continue; }
					const double diff = std::fabs(std::log2(r / 1600.0));
					if (bestDiff < 0.0 || diff < bestDiff) { bestDiff = diff; bestIso = v; }
				}
				cs.nightFixedExposure.iso = bestIso;
			}

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

	// 端末のカメラを一通り見て、所持カメラ・所持レンズ・ひな形を用意する。
	//  **戻り=見つかったカメラの台数**(足した数ではない)。呼ぶ側はこれで
	//  「用意し終えたか(=もう二度としなくてよいか)」を判断する。0 のときは
	//  カメラの権限がまだ無いなどの理由で列挙できていないので、次の起動でやり直す。
	//  既にあるものは触らない(名前や値をユーザーが変えていることがある)。
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
		return static_cast<int>(found.size());
	}
}
