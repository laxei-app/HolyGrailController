#include "common.h"
#include "detectSsdpBase.h"
#include "net.h"
#include "osSystemCall.h"
#include "tool.h"
#include "netThread.h"
#include "dataManager.h"	// 発見できたのに使えない機体を記録する
#include <algorithm>

// SSDP(M-SEARCH)で検出し、同一カメラの統合と apiBase 初期化まで済ませて out に追加する。
// (旧 cameraController::detectTarget の探索/統合/初期化処理をバックエンド側へ移設したもの。)
size_t detectSsdpBase::detect(std::vector<class device>& out)
{
	return discover(out, false);
}

// 身元だけ(デバイス記述まで)。CCAPI を叩かないので認証の影響を受けない。
// 在否監視のように「そこに居るか」だけ知りたい側が使う。
size_t detectSsdpBase::identify(std::vector<class device>& out)
{
	return discover(out, true);
}

size_t detectSsdpBase::discover(std::vector<class device>& out, bool identifyOnly)
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
		std::shared_ptr<class apiBase> api(makeApi());	// makeApi は raw new を返す→shared_ptr が所有
		if (!api) { continue; }							// 生成失敗(このバックエンド非対応)

		if (identifyOnly)
		{	// 身元だけ。デバイス記述(認証不要)で機種名/シリアル/愛称を埋め、apiBase は作らない。
			//  api はここで役目を終える(スコープ離脱で解放)。
			if (api->identify(device) != ERR_HGC_OK) { continue; }
			if (device.serialno.empty()) { continue; }	// 身元が割れないものは在否に使えない
			device.apiBase = nullptr;
			out.push_back(device);
			added++;
			continue;
		}

		// api の初期化をおこなう
		errCode ie = api->init(device);
		if ( ie != ERR_HGC_OK )
		{	// 初期化失敗(api はスコープ離脱で自動解放)
			// 【落とした理由を残す(2026-08-19)】ここで黙って捨てると、SSDPには応えているのに
			//  「カメラが見つからない」としか見えない。実機で起きたのは、CCAPIに認証を設定した
			//  機体で資格情報が未登録のため /ccapi が401になり、この機体だけ発見されなかった例。
			//  身元(記述XML)は認証不要で取れているので、どの機体かは書ける。
			int st = 0; std::string dummy;
			netThread::lastHttpFailure(st, dummy);
			std::string why = (st == 401 || st == 403)
			                  ? "register the camera user/password in owned cameras"
			                  : "";
			std::string d = "found but unusable " + (device.model.empty() ? std::string("?") : device.model)
			              + "/" + (device.serialno.empty() ? std::string("?") : device.serialno)
			              + " HTTP=" + std::to_string(st) + (why.empty() ? "" : (" " + why));
			dataManager::logEvent("NET", d.c_str(), true);
			device.apiBase = nullptr;
			continue;
		}
		device.apiBase = api;			// 共有所有(device コピーで参照共有・最後の参照で解放)
		out.push_back(device);
		added++;
	}
	return added;
}

detectSsdpBase::~detectSsdpBase()
{
	watchStop();
}

// SSDP受動待ち受け開始。0.0.0.0:1900 に bind して 239.255.255.250 の NOTIFY を専用スレッドで待つ。
// 既に稼働中なら何もしない(共有単一リスナ)。net::ssdpListenStart が nullptr(Windows/失敗)なら
// 待ち受け無しで戻る(呼び出し側は60秒ごとの能動再探索で復帰する)。
void detectSsdpBase::watchStart(std::function<void()> onAppear)
{
	if (watchRunning_) { return; }
	onAppear_  = std::move(onAppear);
	watchSock_ = net::ssdpListenStart();
	if (watchSock_ == nullptr) { return; }	// 非対応/失敗 → 待ち受け無し
	watchRunning_ = true;
	ossc::THREAD_FUNC fn = [this](void*) -> errCode { return this->watchLoop(); };
	watchThread_ = ossc::threadNet(fn, nullptr, 4096);	// NOTIFY待ち受けは軽量(内部DRAM節約)
}

void detectSsdpBase::watchStop()
{
	if (!watchRunning_ && watchThread_ == nullptr) { return; }
	watchRunning_ = false;
	if (watchThread_ != nullptr) { ossc::threadEnd(watchThread_); watchThread_ = nullptr; }	// join
	if (watchSock_ != nullptr)   { net::ssdpListenClose(watchSock_); watchSock_ = nullptr; }
}

// 待ち受けスレッド本体: NOTIFY を周期読みし、対象サービス語を含めば onAppear。
// ※プラットフォームで ssdpListenRead のブロッキング挙動が異なる: Android(recvfrom+SO_RCVTIMEO)は
//   最大1秒ブロックするが、M5Stack(WiFiUDP::parsePacket)はノンブロッキングで即戻る。後者では
//   パケット無しで tight loop になり ESP32 のタスクウォッチドッグ(IDLE枯渇)で abort する。
//   これを防ぐため、パケットが無いときは必ず少し眠って CPU を明け渡す(停止応答も約50ms以内)。
errCode detectSsdpBase::watchLoop()
{
	std::string pkt;
	while (watchRunning_)
	{
		pkt.clear();
		if (!net::ssdpListenRead(watchSock_, pkt) || pkt.empty())
		{
			tool::sleep(50);	// パケット無し → CPUを明け渡す(M5Stackのウォッチドッグ回避)
			continue;
		}
		for (const auto& iface : interfaces())
		{	// このバックエンドのカメラが自発広告した(出現した)とみなして通知。
			if (tool::findKvp(pkt, iface.keywords))
			{
				if (onAppear_) { onAppear_(); }
				break;
			}
		}
	}
	return ERR_HGC_OK;
}
