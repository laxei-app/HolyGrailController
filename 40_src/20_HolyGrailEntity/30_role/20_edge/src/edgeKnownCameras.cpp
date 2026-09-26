#include "common.h"
#include "roleDiscovery.h"
#include "cameraController.h"
#include "detectCanonCCapi.h"
#include <memory>
#include "dataManager.h"
#include "osFile.h"
#include "net.h"			// §3.3 tier3: 限定サブネットのバッチ探索(net::scanSubnetPort)
#include "presenceMonitor.h"	// 項目1: 在否モニタ(スマホと共通)。エッジ自身が M-SEARCH/NOTIFY で在否を持つ
#include "holyGrailEntity.h"	// 項目1: hge_anyActiveCameraSession(撮影中はスキャンを止める)
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

	// 既知カメラの型番(device.model。探索元が型番だけに揃えている)が計画カメラと同機種か。
	//  2026-09-06 に接頭辞("Canon ")の吸収を外した。古い knownCams.json に全体名が残っていても
	//  一致しないだけで、通常の SSDP 発見に落ちる(データは作り直す方針)。
	bool knownModelMatch(const std::string& kModel, const hgc::camera& cam)
	{
		if (kModel.empty()) { return false; }
		return (!cam.name.empty() && kModel == cam.name) || (!cam.model.empty() && kModel == cam.model);
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

	// 既知テーブルを /asset/knownCams.json へ保存(呼び出し側で g_knownMutex を保持していること)。
	// 復元に使えるのは serial+ip が確定した分のみ。online は保存しない(起動時は前回IPを楽観扱いにする)。
	void saveKnownCamsLocked()
	{
		std::string d = osfile::dir("asset");
		if (d.empty()) { return; }
		nlohmann::json arr = nlohmann::json::array();
		for (const auto& k : g_knownCams)
		{
			if (k.serial.empty() || k.ip.empty()) { continue; }
			arr.push_back({ {"serial", k.serial}, {"model", k.model}, {"ip", k.ip} });
		}
		std::string s = arr.dump();
		osfile::writeAll(d + "/knownCams.json", s.data(), s.size());
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
			dataManager::logEvent("NET", (std::string("camera connected: direct ip + identity check ip=") + kip + " serial=" + wantSerial).c_str());
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
			dataManager::logEvent("NET", (std::string("camera connected: direct ip (single body of the model) ip=") + kip + " serial=" + known[0].serialno).c_str());
			return true;
		}
	}
	return false;
}

bool trySubnetSweep(const std::string& wantSerial, const hgc::camera& cam, bool hasModel,
                    const std::function<bool(const std::string&)>& serialBusy,
                    device& out)
{
	// 自サブネットで :8080 が開いているホストを一括抽出(生存かつCCAPIサービス有りの候補)。
	std::vector<std::string> hosts = net::scanSubnetPort(8080, 250, 254);
	if (hosts.empty()) { return false; }
	dataManager::logEvent("NET", (std::string("subnet scan: :8080 replies=") + std::to_string(hosts.size())).c_str());
	// 応答IPのみ connectManual で本接続し (model, serial) 本人確認。一致した最初の1台を採用する。
	for (const auto& ip : hosts)
	{
		std::vector<class device> cand;
		if (cameraController::connectManual(cand, ip) <= 0 || !cand[0].apiBase) { continue; }
		bool ok = false;
		if (!wantSerial.empty())      { ok = (cand[0].serialno == wantSerial) && knownModelMatch(cand[0].model, cam); }
		else if (hasModel)            { ok = knownModelMatch(cand[0].model, cam) && !serialBusy(cand[0].serialno); }
		if (ok)
		{
			out = cand[0];
			dataManager::logEvent("NET", (std::string("camera connected: subnet scan + identity check ip=") + ip + " serial=" + cand[0].serialno).c_str());
			return true;	// 成功IPの永続化(noteConnected)は呼び手側の共通経路が行う
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

void noteConnected(const std::string& serial, const std::string& model, const std::string& ip)
{
	if (serial.empty() || ip.empty()) { return; }	// 本人確認キー(serial)と直結ヒント(ip)が揃った時のみ記録
	std::lock_guard<std::mutex> lk(g_knownMutex);
	bool changed = true, merged = false;
	for (auto& x : g_knownCams)
	{
		if (x.serial == serial)
		{
			changed = (x.ip != ip) || (!model.empty() && x.model != model) || !x.online;
			x.ip = ip; if (!model.empty()) { x.model = model; } x.online = true;
			merged = true; break;
		}
	}
	if (!merged) { g_knownCams.push_back(knownCam{ serial, model, ip, true }); }
	if (changed) { saveKnownCamsLocked(); }	// 変化時のみ不揮発へ(頻繁な再接続でのフラッシュを抑制)
}

void loadPersisted()
{
	std::string d = osfile::dir("asset");
	if (d.empty()) { return; }
	std::string s;
	if (!osfile::readAll(d + "/knownCams.json", s) || s.empty()) { return; }
	try {
		auto arr = nlohmann::json::parse(s);
		if (!arr.is_array()) { return; }
		std::lock_guard<std::mutex> lk(g_knownMutex);
		for (auto& e : arr)
		{
			knownCam k;
			k.serial = e.value("serial", std::string());
			k.model  = e.value("model",  std::string());
			k.ip     = e.value("ip",     std::string());
			k.online = true;	// 起動時は前回IPを楽観的にオンライン扱い(接続時に (model,serial) で本人確認)
			if (k.serial.empty() || k.ip.empty()) { continue; }
			bool merged = false;
			for (auto& x : g_knownCams) { if (x.serial == k.serial) { x = k; merged = true; break; } }
			if (!merged) { g_knownCams.push_back(k); }
		}
	} catch (const std::exception&) { /* 壊れていれば無視(次回の接続成功で上書き保存される) */ }
}

// 項目1: エッジも「スマホと同じ在否モニタ(共通 presenceMonitor)」を自分で動かす。
//  ・M-SEARCH(定期)+ 受動NOTIFY + 既知オンライン個体への軽量疎通で、エッジ自身が在否を持つ
//    (スマホ push 依存を廃止=スマホ非接続の単独運用でも×を出せる)。
//  ・エッジ特有: 撮影中(カメラI/O中)はスキャンしない=時間厳守のシャッターと単一netThreadで競合させない。
//    撮影中の在否はランナー(取得フェーズ/接続断検知)が管理するので二重にならない。
void presenceStart(std::function<void()> onChange)
{
	// useNotify=false: SSDP共有リスナは entity 側(ensureWatching=runner poke 用)が管理するため二重登録しない。
	//  在否は定期M-SEARCH(60秒)+疎通で拾う(起動時から走るので、開始時には既に在否を把握済み)。
	// 第4引数: 撮影に使っている個体か。撮影中も**使っていないカメラだけ**を軽く突いて、
	//  AP の無通信タイマ(既定300秒)で追い出されないようにする(2026-08-23)。
	presenceMon::start(std::move(onChange), []() { return !hge_anyActiveCameraSession(); }, false,
	                   [](const std::string& serial) { return hge_isCameraInUse(serial.c_str()); });
}
void presenceStop() { presenceMon::stop(); }
std::string presenceJson() { return presenceMon::json(); }

void registerBackends()
{
	// エッジ役が扱うカメラ(ネットワーク越しの外付けのみ)。
	cameraController::addBackend(std::make_unique<detectCanonCCapi>());
}

bool ownedCamerasAuthoritative()
{
	// エッジも接続したカメラを所持台帳へ記録する(recordConnectedCamera)。ただしそれは
	// 「どの個体に繋いだか」の控えで、測光方式などの**設定の出所ではない**。設定はスマホが
	// 撮影計画へ載せて送ってくるので、エッジはそれを信用する(自分の控えで上書きしない)。
	return false;
}

// 計画カメラの在否。エッジ自身の在否モニタ(共通)から判定する。1=オンライン/0=オフライン/-1=不明。
int cameraPresence(const hgc::camera& cam) { return presenceMon::presence(cam); }
void cameraPresenceVerify(const hgc::camera& cam) { presenceMon::verifyNow(cam); }

}}	// namespace hge::role
