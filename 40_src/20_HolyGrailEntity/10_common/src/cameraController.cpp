#include "common.h"
#include "cameraController.h"
#include "detectCanonCCapi.h"
#include "netThread.h"
#include <algorithm>
#include <mutex>
#include <ctime>

// URL("http://host:port/..")からホスト部を取り出す(AP列挙のdedup用)。
static std::string hostOfUrl(const std::string& url)
{
	auto p = url.find("://");
	if (p == std::string::npos) { return std::string(); }
	size_t s = p + 3, e = s;
	while (e < url.size() && url[e] != ':' && url[e] != '/') { ++e; }
	return url.substr(s, e - s);
}

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
// 身元だけを確かめる検出。CCAPI を叩かないので、認証の有無に関係なく安全に呼べる。
//  detectTarget と違い、発見キャッシュも apiBase も作らない(撮影の経路には一切影響しない)。
size_t cameraController::identifyTargets(std::vector<class device>& devices)
{
	static std::mutex identifyMutex;
	std::lock_guard<std::mutex> lock(identifyMutex);	// M-SEARCH の 1900 bind 競合を避ける
	for (auto& be : backends()) { be->identify(devices); }
	return devices.size();
}

size_t cameraController::detectTarget(std::vector<class device> & devices)
{
	// Phase4(netThread並行化との両立): 複数セッションが同時に発見(SSDP M-SEARCH)を走らせると
	// ローカルポート1900のマルチキャストbindが競合し片方が「0台」になって取得リトライを繰り返す。
	// 発見は取得/再接続時の短時間のみなので直列化する(撮影本体の測光/シャッターHTTPは並行のまま)。
	static std::mutex discoverMutex;
	std::lock_guard<std::mutex> lock(discoverMutex);

	// A-2: 発見の短命共有キャッシュ。近接して起動する2セッション目(discoverMutex 直列化で待たされた側)が
	//  再び 3×M-SEARCH を回すのは無駄なので、直近発見ホストへ connectManual で再構成して SSDP を省く。
	//  ・キャッシュは「ホスト一覧+時刻」のみ(apiBase は保持しない)。各セッションは connectManual で
	//    自前の apiBase を得るため、ポインタ共有/二重解放は起きない(device の所有契約と整合)。
	//  ・TTL 内でもホストが落ちていれば connectManual が失敗し不採用=自己修復(runner が後で再探索)。
	static std::vector<std::string> s_cacheHosts;
	static long long                s_cacheAtSec = 0;
	constexpr long long kDiscoverCacheTtlSec = 10;
	const long long nowSec = static_cast<long long>(std::time(nullptr));
	if (!s_cacheHosts.empty() && (nowSec - s_cacheAtSec) <= kDiscoverCacheTtlSec)
	{
		for (const auto& host : s_cacheHosts)
		{
			for (auto& be : backends())
			{
				class device dev;
				if (be->makeManualDevice(host, dev)) { devices.push_back(dev); break; }
			}
		}
		if (!devices.empty()) { return devices.size(); }	// 全滅なら通常SSDPへフォールスルー(下へ)
	}

	for (auto& be : backends())
	{	// 各種別バックエンドで検出(統合・apiBase 初期化はバックエンド内で完結)。
		be->detect(devices);
	}
	// APモード: エッジがSoftAPのDHCP元締めなので、SSDPに頼らず接続局IPを列挙し各IPをprobeする。
	// STAモード/非対象プラットフォームでは apClientIps() が空=この枝は無影響(挙動不変)。
	std::vector<std::string> apIps = netThread::apClientIps();
	for (const auto& host : apIps)
	{
		bool known = false;
		for (const auto& d : devices) { if (hostOfUrl(d.urlAccess) == host) { known = true; break; } }
		if (known) { continue; }
		for (auto& be : backends())
		{	// この種別として host に接続できれば(=/ccapi 応答があれば)カメラとして採用。
			class device dev;
			if (be->makeManualDevice(host, dev)) { devices.push_back(dev); break; }
		}
	}
	// A-2: 発見できたホスト一覧を短命キャッシュへ記録(次の近接セッションが再構成に使う)。
	if (!devices.empty())
	{
		s_cacheHosts.clear();
		for (const auto& d : devices) { std::string h = hostOfUrl(d.urlAccess); if (!h.empty()) { s_cacheHosts.push_back(h); } }
		s_cacheAtSec = static_cast<long long>(std::time(nullptr));
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

// 露出を1項目ずつ設定(タイマ方式で変更のあった項目だけ適用するため)。null は無害にスキップ。
errCode cameraController::setFNumber(const class device& device, const std::string& fNumber)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->setFNumber(fNumber);
}

errCode cameraController::setSS(const class device& device, const std::string& ss)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->setSS(ss);
}

errCode cameraController::setIso(const class device& device, const std::string& iso)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->setIso(iso);
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

// 測光(場面のリニア輝度の取得)。実装はカメラ依存(apiBase実装側)。
errCode cameraController::meterScene(const class device& device, const hgc::exposure& shotExp,
                                     apiBase::meterResult& out, const std::function<bool()>& keepGoing)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->meterScene(shotExp, out, keepGoing);
}

errCode cameraController::meterHere(const class device& device, apiBase::meterResult& out,
                                    const std::function<bool()>& keepGoing)
{
	if (device.apiBase == nullptr) { return ERR_HGC_READY; }
	return device.apiBase->meterHere(out, keepGoing);
}

apiBase::meterTiming cameraController::meterTimingHint(const class device& device)
{
	if (device.apiBase == nullptr) { return apiBase::meterTiming{}; }	// 既定=シャッター5秒前
	return device.apiBase->meterTimingHint();
}

void cameraController::setMeterLv(const class device& device, bool useLv)
{
	if (device.apiBase != nullptr) { device.apiBase->setMeterLv(useLv); }
}

void cameraController::meterReset(const class device& device)
{
	if (device.apiBase != nullptr) { device.apiBase->meterReset(); }
}

void cameraController::meterArm(const class device& device)
{
	if (device.apiBase != nullptr) { device.apiBase->meterArm(); }
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

// ライブビューを止める(撮影ループ中は掴まない。2026-07-30)。
errCode cameraController::stopLiveView(const class device& device)
{
	if (device.apiBase == nullptr) { return ERR_HGC_NOT_SUPPORTED; }
	return device.apiBase->stopLiveView();
}

// 撮影ループ中にライブビューが必要か(カメラ実装の申告に従う)。
bool cameraController::liveViewNeededWhileCapturing(const class device& device)
{
	if (device.apiBase == nullptr) { return true; }
	return device.apiBase->liveViewNeededWhileCapturing();
}
