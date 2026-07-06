#include "common.h"
#include "roleDiscovery.h"
#include "cameraController.h"
#include "dataManager.h"
#include <json/nlohmann/json.hpp>
#include <vector>
#include <mutex>
#include <string>

// エッジ役の発見: スマホから push された既知カメラ(serial→ip)を保持し、撮影開始時に IP直結 +
// (model, serial) 本人確認まで済ませる。IPはヒント、採用可否は接続後の deviceinformation で確定する。
// このファイルはエッジ成果物(M5Stack)だけがリンクする(スマホ成果物は 10_phone のスタブ)。

namespace {

	// --- 既知カメラテーブル(スマホからの cameraInfo プッシュで更新。43 §6 C_CAMERA_INFO) ---
	struct knownCam { std::string serial; std::string model; std::string ip; bool online = false; };
	std::vector<knownCam> g_knownCams;
	std::mutex            g_knownMutex;

	// 既知カメラの device.model 全体("Canon EOS R100")が計画カメラ(型番のみ "EOS R100" か全体)と同機種か。
	//  メーカー接頭辞差を吸収する(known 側は maker を持たないため接頭辞+空白を許容)。
	//  "EOS R10" が "Canon EOS R100" に誤一致しないよう、接尾一致は語境界(直前が空白)を要求する。
	bool knownModelMatch(const std::string& kModel, const hgc::camera& cam)
	{
		auto eq = [&](const std::string& c) -> bool {
			if (c.empty()) { return false; }
			if (kModel == c) { return true; }										// 完全一致(全体 or 既に型番のみ)
			if (kModel.size() > c.size() + 1 &&
			    kModel.compare(kModel.size() - c.size(), c.size(), c) == 0 &&
			    kModel[kModel.size() - c.size() - 1] == ' ') { return true; }	// "メーカー<空白>型番"
			return false;
		};
		return eq(cam.name) || eq(cam.model);
	}

	// 計画カメラにマッチするオンライン既知IPを返す(serial優先)。無ければ空文字。
	std::string knownOnlineIp(const std::string& wantSerial, const hgc::camera& cam)
	{
		std::lock_guard<std::mutex> lk(g_knownMutex);
		for (auto& k : g_knownCams)
		{
			if (!k.online || k.ip.empty()) { continue; }
			if (!wantSerial.empty()) { if (k.serial == wantSerial) { return k.ip; } continue; }
			if (knownModelMatch(k.model, cam)) { return k.ip; }	// serial未解決は機種一致(接続後にserial+modelで再確認)
		}
		return std::string();
	}

	// 機種一致でオンラインな既知カメラが「ちょうど1台」ならそのIPを返す(曖昧さ無し)。
	// 同機種が複数オンライン(選別が必要)や0台なら空を返し、SSDPに委ねる。
	std::string knownUniqueModelIp(const hgc::camera& cam)
	{
		std::lock_guard<std::mutex> lk(g_knownMutex);
		std::string ip; int cnt = 0;
		for (auto& k : g_knownCams)
		{
			if (!k.online || k.ip.empty()) { continue; }
			if (knownModelMatch(k.model, cam)) { ++cnt; ip = k.ip; }
		}
		return (cnt == 1) ? ip : std::string();
	}

}	// anonymous namespace

namespace hge { namespace role {

bool tryIpDirect(const std::string& wantSerial, const hgc::camera& cam, bool hasModel,
                 const std::function<bool(const std::string&)>& serialBusy,
                 device& out)
{
	if (!wantSerial.empty())
	{	// §3.3.1 serial確定 → serial一致IPへ直結し (model, serial) 両方一致で採用(最速)。
		std::string kip = knownOnlineIp(wantSerial, cam);
		std::vector<class device> known;
		//  本人確認: (model, serial) 両方一致。CCAPIのmanufacturerは"Canon.Inc"でstripMaker不可のため、
		//  接続後model全体("Canon EOS R100")を計画型番("EOS R100")と knownModelMatch で照合する。
		if (!kip.empty() && cameraController::connectManual(known, kip) > 0 && known[0].apiBase
		    && known[0].serialno == wantSerial && knownModelMatch(known[0].model, cam))
		{
			out = known[0];
			dataManager::logEvent("NET", (std::string("カメラ接続 IP直結+本人確認 ip=") + kip + " serial=" + wantSerial).c_str());
			return true;
		}
	}
	else if (hasModel)
	{	// serial未指定でも、機種一致のオンライン既知カメラがちょうど1台なら曖昧さ無し→IP直結。
		std::string kip = knownUniqueModelIp(cam);
		std::vector<class device> known;
		if (!kip.empty() && cameraController::connectManual(known, kip) > 0 && known[0].apiBase
		    && knownModelMatch(known[0].model, cam) && !serialBusy(known[0].serialno))
		{
			out = known[0];
			dataManager::logEvent("NET", (std::string("カメラ接続 IP直結(機種一意) ip=") + kip + " serial=" + known[0].serialno).c_str());
			return true;
		}
	}
	return false;
}

int setKnownCameras(const char* json, int len)
{
	if (json == nullptr) { return ERR_HGC_INVALID_ARG; }
	std::string s = (len > 0) ? std::string(json, static_cast<size_t>(len)) : std::string(json);
	try {
		auto arr = nlohmann::json::parse(s);
		if (!arr.is_array()) { return ERR_HGC_INVALID_ARG; }
		std::lock_guard<std::mutex> lk(g_knownMutex);
		for (auto& e : arr) {
			knownCam k;
			k.serial = e.value("serial", std::string());
			k.model  = e.value("model",  std::string());
			k.ip     = e.value("ip",     std::string());
			k.online = e.value("online", false);
			if (k.serial.empty() && k.ip.empty()) { continue; }
			bool merged = false;	// serial一致は更新、無ければ追加。online=false も保持(在/不在の判断は上位)。
			for (auto& x : g_knownCams) { if (!k.serial.empty() && x.serial == k.serial) { x = k; merged = true; break; } }
			if (!merged) { g_knownCams.push_back(k); }
		}
	} catch (const std::exception&) { return ERR_HGC_INVALID_ARG; }
	return ERR_HGC_OK;
}

}}	// namespace hge::role
