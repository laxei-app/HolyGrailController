#include "presenceMonitor.h"
#include "cameraController.h"
#include "netThread.h"
#include "osSystemCall.h"
#include <json/nlohmann/json.hpp>
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <ctime>
#include <thread>
#include <chrono>

// カメラ在否モニタ(共通実装)。旧 phonePresence.cpp のループをそのまま共通化し、canScan 述語を追加した。
//  注: detectTarget が確保する device.apiBase は shared_ptr 所有。フル探索結果 found は merge 後に破棄され
//      最後の参照が消えて解放されるため、常駐フル探索でもリークしない。
namespace {

	struct pcam { std::string serial; std::string model; std::string friendly; std::string ip; bool online = false; long long lastSeen = 0;
	              bool verify = false; };	// #1: 次tickで即疎通確認し、失敗ならTTLを待たずオフラインへ
	std::vector<pcam>       g_map;
	std::mutex              g_mapMutex;
	std::function<void()>   g_onChange;
	std::function<bool()>   g_canScan;		// この周期にネットワーク探索してよいか(エッジ=撮影中は false)
	std::atomic<bool>       g_running{false};
	std::atomic<bool>       g_scanned{false};	// 一度でもフル探索(M-SEARCH)を終えたか。未探索なら在否は「不明」
	std::atomic<bool>       g_wake{false};		// NOTIFY等で即フル探索を促す
	bool                    g_useNotify = true;	// 受動NOTIFY(SSDP watchStart)を使うか(エッジ=false)
	void*                   g_thread = nullptr;
	std::string             g_lastSig;			// 直近スナップショット署名(変化検知用)

	constexpr int  kFullEverySec = 60;			// フル M-SEARCH の間隔(オンライン検知/取りこぼし保険)
	constexpr int  kTickSec      = 6;			// ループ周期(この単位で疎通確認・wake確認)
	constexpr long kTtlSec       = 150;			// この秒数見えないオンライン個体はオフライン扱い

	std::string hostOf(const std::string& url)
	{
		auto p = url.find("://");
		if (p == std::string::npos) { return std::string(); }
		size_t s = p + 3, e = s;
		while (e < url.size() && url[e] != ':' && url[e] != '/') { ++e; }
		return url.substr(s, e - s);
	}

	std::string signatureLocked()
	{
		std::vector<std::string> items;
		for (const auto& c : g_map) { if (c.online) { items.push_back(c.serial + ":" + c.ip); } }
		std::sort(items.begin(), items.end());
		std::string s; for (auto& i : items) { s += i; s += ","; }
		return s;
	}

	void mergeFullLocked(long long now)
	{
		std::vector<class device> found;
		cameraController::detectTarget(found);	// ※apiBaseリーク許容(頻度を抑えて使用)
		for (auto& d : found)
		{
			if (!d.apiBase) { continue; }
			std::string ip = hostOf(d.urlAccess);
			if (d.serialno.empty() || ip.empty()) { continue; }
			bool merged = false;
			for (auto& c : g_map) { if (c.serial == d.serialno) { c.model = d.model; c.friendly = d.friendName; c.ip = ip; c.online = true; c.lastSeen = now; merged = true; break; } }
			if (!merged) { g_map.push_back(pcam{ d.serialno, d.model, d.friendName, ip, true, now }); }
		}
	}

	void refreshLivenessLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (!c.online) { c.verify = false; continue; }
			std::string resp;
			const bool alive = netThread::httpGet("http://" + c.ip + ":8080/ccapi", resp) && !resp.empty();
			if (alive) { c.lastSeen = now; c.verify = false; }
			else if (c.verify)  { c.online = false; c.verify = false; }	// #1: 即確認の要求 → TTLを待たずオフライン
			else if (now - c.lastSeen > kTtlSec) { c.online = false; }
		}
	}

	// #1: verify 要求のある個体だけを先に疎通確認する(フル探索の周期を待たない)。
	bool anyVerifyPendingLocked()
	{
		for (const auto& c : g_map) { if (c.online && c.verify) { return true; } }
		return false;
	}

	bool canScanNow() { return !g_canScan || g_canScan(); }

	void presenceLoop()
	{
		long long lastFull = 0;
		while (g_running.load())
		{
			const long long now = static_cast<long long>(std::time(nullptr));
			// wake は「この周回で必ず消費する」。条件式の中で exchange すると、短絡評価で
			// 評価されない経路ができ、wake が立ちっぱなしになって下の待ちが 0 秒になる
			// (=CPUを明け渡さない空回り → IDLEタスクが走れず task_wdt でリセット)。
			// 実際 2026-07-23 に両エッジが約2秒周期でリセットを繰り返した(WDT/ossNet)。
			const bool woke = g_wake.exchange(false);
			bool changed = false;
			if (canScanNow())	// エッジ: 撮影中はネットワークを触らずマップを保持(前回値のまま)
			{
				std::lock_guard<std::mutex> lk(g_mapMutex);
				// #1: 即確認の要求がある間は、まず疎通確認を優先する(オフラインを最短で確定させる)。
				//  その後のフル探索で、IPが変わっただけの個体は拾い直される。
				const bool verifyFirst = anyVerifyPendingLocked();
				const bool doFull = !verifyFirst &&
				                    (woke || (now - lastFull >= kFullEverySec) || lastFull == 0);
				if (doFull) { mergeFullLocked(now); lastFull = now; g_scanned.store(true); }
				else        { refreshLivenessLocked(now); }
				std::string sig = signatureLocked();
				if (sig != g_lastSig) { g_lastSig = sig; changed = true; }
			}
			if (changed && g_onChange) { g_onChange(); }
			// 待ち。wake で早く起きるのは良いが、**1秒は必ず眠る**。そうしないと wake が
			// 立て続けに来たときや、上のブロックを丸ごと飛ばす経路(撮影中=canScanNow false)で
			// 1周が一瞬で終わり、このスレッドがCPUを占有してWDTリセットに至る。
			std::this_thread::sleep_for(std::chrono::seconds(1));
			for (int i = 1; i < kTickSec && g_running.load() && !g_wake.load(); ++i)
			{ std::this_thread::sleep_for(std::chrono::seconds(1)); }
		}
	}

}	// anonymous namespace

namespace presenceMon {

void start(std::function<void()> onChange, std::function<bool()> canScan, bool useNotify)
{
	if (g_running.exchange(true)) { return; }	// 二重起動防止
	g_onChange = std::move(onChange);
	g_canScan  = std::move(canScan);
	g_useNotify = useNotify;
	g_lastSig.clear();
	// 受動NOTIFY: カメラ出現の広告を拾ったら即フル探索へ前倒し(useNotify のときのみ。
	//  エッジは共有SSDPリスナを entity 側=runner poke 用が使うため、ここでは二重登録しない)。
	if (g_useNotify) { cameraController::watchStart([]() { g_wake.store(true); }); }
	ossc::THREAD_FUNC fn = [](void*) -> errCode { presenceLoop(); return ERR_HGC_OK; };
	g_thread = ossc::threadNet(fn, nullptr);
}

void stop(void)
{
	if (!g_running.exchange(false)) { return; }
	if (g_useNotify) { cameraController::watchStop(); }
	if (g_thread) { ossc::threadEnd(g_thread); g_thread = nullptr; }
	std::lock_guard<std::mutex> lk(g_mapMutex);
	g_map.clear(); g_lastSig.clear(); g_scanned.store(false);
}

std::string json(void)
{
	std::lock_guard<std::mutex> lk(g_mapMutex);
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& c : g_map)
	{
		if (c.serial.empty() || c.ip.empty()) { continue; }
		arr.push_back({ {"serial", c.serial}, {"model", c.model}, {"friendly", c.friendly}, {"ip", c.ip}, {"online", c.online} });
	}
	return arr.dump();
}

void verifyNow(const hgc::camera& cam)
{
	{
		std::lock_guard<std::mutex> lk(g_mapMutex);
		const std::string& m = cam.model.empty() ? cam.name : cam.model;
		for (auto& c : g_map)
		{
			if (!c.online) { continue; }
			const bool hit = (!cam.serial.empty() && c.serial == cam.serial) ||
			                 (cam.serial.empty() && !m.empty() &&
			                  (c.model.find(m) != std::string::npos ||
			                   (!c.model.empty() && m.find(c.model) != std::string::npos)));
			if (hit) { c.verify = true; }
		}
	}
	// ループの待ちを打ち切って即座に処理させる(疎通確認→必要ならフル探索の順で回る)。
	g_wake.store(true);
}

int presence(const hgc::camera& cam)
{
	if (!g_scanned.load()) { return -1; }	// 未探索=不明
	std::lock_guard<std::mutex> lk(g_mapMutex);
	if (!cam.serial.empty())
	{
		for (const auto& c : g_map) { if (c.serial == cam.serial) { return c.online ? 1 : 0; } }
		// serial 指定だが未知(まだ見えたことがない) → 下のモデル照合へ。
	}
	const std::string& m = cam.model.empty() ? cam.name : cam.model;
	if (!m.empty())
	{
		for (const auto& c : g_map)
		{
			if (c.online && (c.model.find(m) != std::string::npos || (!c.model.empty() && m.find(c.model) != std::string::npos)))
			{ return 1; }
		}
	}
	return 0;	// 探索済みでオンライン一致が無い=オフライン(×)
}

}	// namespace presenceMon
