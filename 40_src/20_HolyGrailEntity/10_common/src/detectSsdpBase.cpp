#include "common.h"
#include "detectSsdpBase.h"
#include <algorithm>

// SSDP(M-SEARCH)で検出し、同一カメラの統合と apiBase 初期化まで済ませて out に追加する。
// (旧 cameraController::detectTarget の探索/統合/初期化処理をバックエンド側へ移設したもの。)
size_t detectSsdpBase::detect(std::vector<class device>& out)
{
	std::vector<class device> devices;						// このバックエンド分の検出結果
	auto num = deviceDiscovery::search(devices, interfaces());	// SSDP で探す(このバックエンドの service 定義で分類)
	if (num == 0) { return 0; }								// 見つからない

	// uuid と service でソートする
	std::sort(devices.begin(), devices.end(),
		[](const class device& a, const class device& b)
	{
		// uuid 比較
		if (a.uuid < b.uuid) { return true; }
		if (a.uuid > b.uuid) { return false; }

		// service 比較
		if (a.service < b.service) { return true; }
		return false;

	});

	// 同じカメラの情報を 1 つにまとめる。
	for (int ix = 0; ix < devices.size()-1; ix++)
	{
		auto ixN = ix + 1;						// 次のインデックス
		do
		{
			if ((devices[ix].uuid != devices[ixN].uuid) || (devices[ix].service != devices[ixN].service))
			{	// 違うデバイス
				break;
			}

			// 統合関数
			std::function<void(std::string&, std::string&)> integra = [&](std::string& org, std::string& nxt)->void {
				if (org.length() == 0) { org = nxt; }
			};
			// 同じデバイスの情報を統合する
			integra(devices[ix].uuid, devices[ixN].uuid);
			integra(devices[ix].location, devices[ixN].location);
			integra(devices[ix].urn, devices[ixN].urn);
			integra(devices[ix].service, devices[ixN].service);

			// api を統合
			if ((devices[ix].apiClass == device::apiClass::NON) &&
				(devices[ix].apiClass != devices[ixN].apiClass))
			{	// 入ってなかったら入ってる方を使う
				devices[ix].apiClass = devices[ixN].apiClass;
			}

			// 削除
			devices.erase(devices.begin() + ixN);
			ixN++;
		} while (ixN < devices.size());
	}

	size_t added = 0;
	for (auto& device : devices)
	{	// api の初期化をおこなう。このバックエンドが対応する種別のみ生成できる。
		class apiBase* apiBase = makeApi();
		if (apiBase == nullptr) { continue; }			// 生成失敗(このバックエンド非対応)

		// api の初期化をおこなう
		if ( apiBase->init(device) != ERR_HGC_OK )
		{	// 初期化失敗
			delete apiBase;
			device.apiBase = nullptr;
			continue;
		}
		device.apiBase = apiBase;
		out.push_back(device);			// apiBase はポインタ共有(device は解放しない設計)
		added++;
	}
	return added;
}
