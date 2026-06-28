#include "common.h"
#include "cameraController.h"
#include "deviceDiscovery.h"
#include <algorithm>

// ネットワークに接続されているカメラを検出する
// devices : 検出したカメラの情報
// return  : 検出したカメラの数
size_t cameraController::detectTarget(std::vector<class device> & devices)
{
	std::vector<class device> devicesIn;						// 呼び出し元の領域はまだ触らない
	auto num = deviceDiscovery::search(devices);				// ネットワークに接続されているカメラを探す
	if (num == 0) { return 0; }									// 見つからない

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

	for (auto& device : devices)
	{	// api の初期化をおこなう。
		class apiBase* apiBase = nullptr;
		if (device.apiClass == device::apiClass::CANON_CCAPI) { apiBase = new apiCanonCCAPI(); }
		else if (device.apiClass == device::apiClass::SONY_DI) { apiBase = new apiCanonCCAPI(); }   // sony の api 作ったら入れ替え
		else { continue; }          // 該当なし

		// api の初期化をおこなう
		if ( apiBase->init(device) != ERR_HGC_OK )		
		{	// 初期化失敗
			delete apiBase; 
			device.apiBase = nullptr;
			continue;
		}
		device.apiBase = apiBase;
	}
	return devices.size();
}

// IP直指定でカメラに接続する(SSDP不使用)。
size_t cameraController::connectManual(std::vector<class device>& devices, const std::string& host)
{
	devices.clear();
	class device dev;
	dev.apiClass = device::apiClass::CANON_CCAPI;
	dev.location = host;
	dev.urlAccess = "http://" + host + ":8080/ccapi";	// CCAPI のアクセスURL
	dev.model = "Canon CCAPI (manual)";
	devices.push_back(dev);								// 先に格納してから apiBase を設定する

	auto* api = new apiCanonCCAPI();
	if (api->initManual(devices.back()) != ERR_HGC_OK)
	{
		delete api;
		devices.pop_back();
		return 0;
	}
	devices.back().apiBase = api;
	return devices.size();
}

// シャッター準備
// shotSet : シャッター設定値
// return  : ERR_HGC_OK:成功
errCode cameraController::rdyShutter(const class device& device, const cmdt::shotSet& shotSet)
{
	return device.apiBase->rdyShutter(shotSet);
}

// シャッターを切る
// device : 対象デバイス
// return : ERR_HGC_OK:成功、それ以外はエラー
errCode cameraController::actShutter(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->actShutter();
}

// 設定を取得する
// device   :対象デバイス
// settings :取得した設定
// return 　:ERR_HGC_OK:成功
errCode cameraController::getSettings(const class device& device, cmdt::shotRange& settings)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->getSettings(settings);
}

// 測光の準備をする
// device :対象デバイス
// return : ERR_HGC_OK: 成功
errCode cameraController::rdyMetering(const class device& device)
{
	return device.apiBase->rdyMetering();
	return ERR_HGC_OK;
}

// 測光結果を取得する
// device :対象デバイス
// hist   :ヒストグラムの領域
// return : ERR_HGC_OK: 成功
errCode cameraController::alzMetering(const class device& device, cmdt::HISTOGRAM& hist)
{
	return device.apiBase->alzMetering(hist);
}

errCode cameraController::setupShootingModeManual(const class device& device)
{
	return device.apiBase->setupShootingModeManual();
}

errCode cameraController::restoreShootingMode(const class device& device)
{
	return device.apiBase->restoreShootingMode();
}

// 撮影開始
// device :対象デバイス
// return : ERR_HGC_OK: 成功
errCode cameraController::startShooting(const class device& device)
{
	return device.apiBase->startShooting();
}
