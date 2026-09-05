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
#include <string>
#include <vector>

namespace builtinCam
{
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
		return added;
	}
}
