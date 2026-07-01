#include "common.h"
#include "cameraController.h"
#include "detectCanonCCapi.h"
#include <algorithm>

// 受信バックエンド群(Meyers シングルトン。初回に生成)。
// 現状は Canon CCAPI のみ。Sony/Nikon/スマホ内蔵は将来ここへ push_back する(構造のみ)。
std::vector<std::unique_ptr<detectBase>>& cameraController::backends()
{
	static std::vector<std::unique_ptr<detectBase>> b;
	if (b.empty())
	{
		b.push_back(std::make_unique<detectCanonCCapi>());
	}
	return b;
}

// ネットワークに接続されているカメラを検出する
// devices : 検出したカメラの情報(呼び出し側でクリア済み。各バックエンドが追加する)
// return  : 検出したカメラの数
size_t cameraController::detectTarget(std::vector<class device> & devices)
{
	for (auto& be : backends())
	{	// 各種別バックエンドで検出(統合・apiBase 初期化はバックエンド内で完結)。
		be->detect(devices);
	}
	return devices.size();
}

// IP直指定でカメラに接続する(SSDP不使用)。対応するバックエンドが接続を試みる。
size_t cameraController::connectManual(std::vector<class device>& devices, const std::string& host)
{
	devices.clear();
	for (auto& be : backends())
	{
		class device dev;
		if (be->makeManualDevice(host, dev))
		{
			devices.push_back(dev);		// apiBase はポインタ共有(device は解放しない設計)
			return devices.size();
		}
	}
	return 0;
}

// SSDP受動待ち受けの開始/停止(3b)。全バックエンドへ委譲(SSDP系のみ実装、他は既定no-op)。
void cameraController::watchStart(std::function<void()> onAppear)
{
	for (auto& be : backends()) { be->watchStart(onAppear); }
}

void cameraController::watchStop()
{
	for (auto& be : backends()) { be->watchStop(); }
}

// シャッター準備
// shotSet : シャッター設定値
// return  : ERR_HGC_OK:成功
errCode cameraController::rdyShutter(const class device& device, const cmdt::shotSet& shotSet)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
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
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->rdyMetering();
}

// 測光結果を取得する
// device :対象デバイス
// hist   :ヒストグラムの領域
// return : ERR_HGC_OK: 成功
errCode cameraController::alzMetering(const class device& device, cmdt::HISTOGRAM& hist)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->alzMetering(hist);
}

errCode cameraController::setupShootingModeManual(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->setupShootingModeManual();
}

errCode cameraController::restoreShootingMode(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }	// 未取得のまま終了(3a)なら無害にスキップ
	return device.apiBase->restoreShootingMode();
}

// 接続維持用の無害なGET(待機中の定期送出・再接続後の到達確認に使う)。
errCode cameraController::keepAlive(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->keepAlive();
}

// 撮影開始
// device :対象デバイス
// return : ERR_HGC_OK: 成功
errCode cameraController::startShooting(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->startShooting();
}
