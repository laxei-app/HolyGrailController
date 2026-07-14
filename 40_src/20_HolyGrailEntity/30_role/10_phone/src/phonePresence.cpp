#include "common.h"
#include "roleDiscovery.h"
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

// スマホ役の常駐プレゼンスマップ(§3.2 / §5.4)。
//  ・起動時と定期(既定60s)に M-SEARCH(detectTarget)でオンラインカメラを洗い出し、間の各周期は
//    既知IPへの軽量HTTP疎通(leak-free)でオフライン化を素早く拾う。
//  ・受動NOTIFY(cameraController::watchStart)で新カメラ出現を検知したら即フル探索へ前倒しする。
//  ・マップが変化(オンライン/オフライン/IP変更)した時だけ onChange を呼ぶ(UIが最新IPをエッジへpush)。
//  ・presence(見えているか)のみ保持し CCAPIセッションは張らない(占有しない)。
//  注: detectTarget が確保する device.apiBase は shared_ptr 所有(commit a57d09b)。フル探索結果 found は
//      merge 後に破棄され最後の参照が消えて解放されるため、常駐フル探索でもリークしない。
namespace {

	struct pcam { std::string serial; std::string model; std::string friendly; std::string ip; bool online = false; long long lastSeen = 0; };
	std::vector<pcam>       g_map;
	std::mutex              g_mapMutex;
	std::function<void()>   g_onChange;
	std::atomic<bool>       g_running{false};
	std::atomic<bool>       g_scanned{false};	// 項目8: 一度でもフル探索(M-SEARCH)を終えたか。未探索なら在否は「不明」
	std::atomic<bool>       g_wake{false};		// NOTIFY等で即フル探索を促す
	void*                   g_thread = nullptr;
	std::string             g_lastSig;			// 直近スナップショット署名(変化検知用)

	constexpr int  kFullEverySec = 60;			// フル M-SEARCH の間隔(オンライン検知/取りこぼし保険)
	constexpr int  kTickSec      = 6;			// ループ周期(この単位で疎通確認・wake確認)
	constexpr long kTtlSec       = 150;			// この秒数見えないオンライン個体はオフライン扱い

	// "http://IP:8080/ccapi" 等からホスト部を取り出す。
	std::string hostOf(const std::string& url)
	{
		auto p = url.find("://");
		if (p == std::string::npos) { return std::string(); }
		size_t s = p + 3, e = s;
		while (e < url.size() && url[e] != ':' && url[e] != '/') { ++e; }
		return url.substr(s, e - s);
	}

	// 現在のオンライン集合の署名(serial:ip をソート連結)。変化検知に使う。
	std::string signatureLocked()
	{
		std::vector<std::string> items;
		for (const auto& c : g_map) { if (c.online) { items.push_back(c.serial + ":" + c.ip); } }
		std::sort(items.begin(), items.end());
		std::string s; for (auto& i : items) { s += i; s += ","; }
		return s;
	}

	// フル探索(M-SEARCH+接続+deviceinformation)結果でマップを更新。
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

	// 既知オンライン個体へ軽量HTTP疎通(leak-free)。落ちていれば online=false。TTL超過もオフライン化。
	void refreshLivenessLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (!c.online) { continue; }
			std::string resp;
			if (netThread::httpGet("http://" + c.ip + ":8080/ccapi", resp) && !resp.empty()) { c.lastSeen = now; }
			else if (now - c.lastSeen > kTtlSec) { c.online = false; }
		}
	}

	void presenceLoop()
	{
		long long lastFull = 0;
		while (g_running.load())
		{
			const long long now = static_cast<long long>(std::time(nullptr));
			bool changed = false;
			{
				std::lock_guard<std::mutex> lk(g_mapMutex);
				const bool doFull = g_wake.exchange(false) || (now - lastFull >= kFullEverySec) || lastFull == 0;
				if (doFull) { mergeFullLocked(now); lastFull = now; g_scanned.store(true); }
				else        { refreshLivenessLocked(now); }
				std::string sig = signatureLocked();
				if (sig != g_lastSig) { g_lastSig = sig; changed = true; }
			}
			if (changed && g_onChange) { g_onChange(); }
			// kTickSec を小刻みに寝て、g_running / g_wake に素早く反応する。
			for (int i = 0; i < kTickSec && g_running.load() && !g_wake.load(); ++i)
			{ std::this_thread::sleep_for(std::chrono::seconds(1)); }
		}
	}

}	// anonymous namespace

namespace hge { namespace role {

void presenceStart(std::function<void()> onChange)
{
	if (g_running.exchange(true)) { return; }	// 二重起動防止
	g_onChange = std::move(onChange);
	g_lastSig.clear();
	// 受動NOTIFY: カメラ出現の広告を拾ったら即フル探索へ前倒し。
	cameraController::watchStart([]() { g_wake.store(true); });
	ossc::THREAD_FUNC fn = [](void*) -> errCode { presenceLoop(); return ERR_HGC_OK; };
	g_thread = ossc::threadNet(fn, nullptr);
}

void presenceStop()
{
	if (!g_running.exchange(false)) { return; }
	cameraController::watchStop();
	if (g_thread) { ossc::threadEnd(g_thread); g_thread = nullptr; }
	std::lock_guard<std::mutex> lk(g_mapMutex);
	g_map.clear(); g_lastSig.clear();
}

std::string presenceJson()
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

// 項目8: 計画カメラの在否をプレゼンスマップから判定(占有しない)。1=オンライン/0=オフライン/-1=不明。
//  ・まだ一度もフル探索していない間は「不明」(×を出さず点灯のまま)。
//  ・一度でも探索済みなら、オンライン一致が無いカメラは「オフライン(×)」とする(要件:オンラインでない場合は×)。
//   一度発見した個体は httpGet 疎通で生存確認するため、SSDP広告を止める個体も落とさない(誤×を抑制)。
int cameraPresence(const hgc::camera& cam)
{
	if (!g_scanned.load()) { return -1; }	// 未探索=不明
	std::lock_guard<std::mutex> lk(g_mapMutex);
	// serial 指定があり、その個体を把握していればそれが最優先(厳密)。
	if (!cam.serial.empty())
	{
		for (const auto& c : g_map) { if (c.serial == cam.serial) { return c.online ? 1 : 0; } }
		// serial 指定だが未知(まだ見えたことがない) → 下のモデル照合へ。
	}
	// モデル照合。プレゼンスの model は "Canon EOS R10" 等、計画は "EOS R10"。オンライン一致が1つでもあれば在。
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

}}	// namespace hge::role
