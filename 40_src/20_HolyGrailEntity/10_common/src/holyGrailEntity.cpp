#include "common.h"
#include "holyGrailEntity.h"
#include "captureRunner.h"
#include "astroSched.h"
#include "cameraController.h"
#include "dataManager.h"
#include "csJson.h"
#include "netThread.h"
#include "osSystemCall.h"
#include "cs.h"
#include "ccm.h"
#include <atomic>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

// ============================================================================
//  グローバル状態(MVP: 単一インスタンス)
// ============================================================================
namespace
{
	std::mutex            g_mutex;
	hgeNotifyCb           g_cb   = nullptr;
	void*                 g_user = nullptr;
	std::atomic<int>      g_state{ HGE_ST_IDLE };

	hgc::cs               g_plan;
	bool                  g_planReady = false;
	int                   g_offMin = 0;
	std::string           g_schedJson;

	std::vector<device>   g_devices;
	captureRunner         g_runner;
	void*                 g_startThread = nullptr;
	void*                 g_searchThread = nullptr;	// カメラ自動検索ワーカー
	bool                  g_inited = false;
	bool                  g_logCapturing = false;	// ログ用: 撮影中か(START/STOP検出)
	std::string           g_lastCcm;				// ログ用: 直近の撮影制御方法名(CCMSW検出)

	// 進捗スナップショット(progress(get) 応答・エッジ端末用)
	int                   g_pgFrame = 0, g_pgTotal = 0, g_pgRemain = 0, g_pgElapsed = 0;
	hgc::exposure         g_pgExp{};

	const char* const     VERSION = "HolyGrailEntity 0.1 (MVP step2.1)";

	// --- 通知 ---
	void notify(int32_t ev, const std::string& json)
	{
		hgeNotifyCb cb; void* user;
		{
			std::lock_guard<std::mutex> lk(g_mutex);
			cb = g_cb; user = g_user;
		}
		if (cb) { cb(ev, json.c_str(), static_cast<int32_t>(json.size()), user); }
	}

	void setState(int s)
	{
		g_state = s;
		char buf[32];
		std::snprintf(buf, sizeof(buf), "{\"state\":%d}", s);
		notify(HGE_EV_STATE, buf);
	}

	void notifyError(errCode code, const char* msg)
	{
		std::string j = "{\"code\":" + std::to_string(static_cast<unsigned>(code)) +
		                ",\"msg\":\"" + (msg ? msg : "") + "\"}";
		notify(HGE_EV_ERROR, j);
		std::string d = "code=" + std::to_string(static_cast<unsigned>(code)) +
		                " " + (msg ? msg : "");
		dataManager::logEvent("ERR", d.c_str(), true);
	}

	// --- JSON 補助 ---
	std::string dtToStr(const hgc::dateTime& d)
	{
		char b[32];
		std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u",
		              d.year, d.month, d.day, d.hour, d.min, d.sec);
		return b;
	}
	std::string jesc(const std::string& s)
	{
		std::string o;
		for (char c : s)
		{
			if (c == '"' || c == '\\') { o.push_back('\\'); }
			o.push_back(c);
		}
		return o;
	}

	// --- スケジュールの JSON 生成 ---
	void buildScheduleJson(void)
	{
		char num[64];
		std::string j = "{\"name\":\"" + jesc(g_plan.name) + "\"";
		j += ",\"interval\":" + std::to_string(static_cast<int>(g_plan.interval));
		j += ",\"start\":\"" + dtToStr(g_plan.start) + "\"";
		j += ",\"end\":\""   + dtToStr(g_plan.end)   + "\"";
		// 表示用の静的フィールド(場所/機材/方向/仰角/向き)
		j += ",\"place\":\"" + jesc(g_plan.place.name) + "\"";
		std::snprintf(num, sizeof(num), "%.4f,%.4f", g_plan.place.latitude, g_plan.place.longitude);
		j += ",\"latlng\":\"" + std::string(num) + "\"";
		j += ",\"altitude\":" + std::to_string(static_cast<int>(g_plan.place.altitude));
		j += ",\"camera\":\"" + jesc(g_plan.camera.maker + " " + g_plan.camera.model) + "\"";
		j += ",\"lens\":\""   + jesc(g_plan.lens.name) + "\"";
		std::snprintf(num, sizeof(num), "%.1f", g_plan.azimuth);
		j += ",\"azimuth\":" + std::string(num);
		std::snprintf(num, sizeof(num), "%.1f", g_plan.elevation);
		j += ",\"elevation\":" + std::string(num);
		j += ",\"landscape\":" + std::string(g_plan.landscape ? "true" : "false");
		j += ",\"events\":[";
		for (size_t i = 0; i < g_plan.events.size(); ++i)
		{
			if (i) { j += ","; }
			j += "{\"event\":" + std::to_string(static_cast<int>(g_plan.events[i].event)) +
			     ",\"when\":\"" + dtToStr(g_plan.events[i].when) + "\"}";
		}
		j += "],\"windows\":[";
		for (size_t i = 0; i < g_plan.ccmList.size(); ++i)
		{
			if (i) { j += ","; }
			const auto& w = g_plan.ccmList[i];
			int ct = w.ccm ? static_cast<int>(w.ccm->type) : 0;
			std::string nm = w.ccm ? w.ccm->name : "";
			j += "{\"type\":" + std::to_string(ct) +
			     ",\"name\":\"" + jesc(nm) + "\"" +
			     ",\"start\":\"" + dtToStr(w.start) + "\"" +
			     ",\"end\":\"" + dtToStr(w.end) + "\"}";
		}
		j += "]}";
		g_schedJson = j;
	}

	// 検出済みデバイス一覧を JSON 配列にする。
	std::string devicesJson(void)
	{
		std::string dj = "[";
		for (size_t i = 0; i < g_devices.size(); ++i)
		{
			if (i) { dj += ","; }
			const auto& d = g_devices[i];
			dj += "{\"uuid\":\"" + jesc(d.uuid) + "\",\"model\":\"" + jesc(d.model) +
			      "\",\"manufacturer\":\"" + jesc(d.manufacturer) +
			      "\",\"serialno\":\"" + jesc(d.serialno) + "\"}";
		}
		dj += "]";
		return dj;
	}

	// 接続したカメラのIP/機種をログ(NET)に残す。
	void logCameraNet(void)
	{
		if (g_devices.empty()) { return; }
		const auto& d = g_devices[0];
		std::string detail = "camera=" + d.model + " " + d.location;
		dataManager::logEvent("NET", detail.c_str());
	}

	// time_t → ローカル日時 と UTCオフセット[分]
	void localFromTime(time_t t, hgc::dateTime& d, int& offMin)
	{
		std::tm lt{};
#if defined(_WIN32)
		localtime_s(&lt, &t);
#else
		localtime_r(&t, &lt);
#endif
		d.year  = static_cast<uint16_t>(lt.tm_year + 1900);
		d.month = static_cast<uint16_t>(lt.tm_mon + 1);
		d.day   = static_cast<uint16_t>(lt.tm_mday);
		d.hour  = static_cast<uint16_t>(lt.tm_hour);
		d.min   = static_cast<uint16_t>(lt.tm_min);
		d.sec   = static_cast<uint16_t>(lt.tm_sec);
		// ローカルを UTC とみなした時刻と実 UTC の差がオフセット
		long long asIfUtc = hgc::toUnixUtc(d, 0);
		offMin = static_cast<int>((asIfUtc - static_cast<long long>(t)) / 60);
	}

	// 固定データの撮影計画を生成する。開始=現在、終了=2時間後。
	// 機材・場所・撮影制御方法などの出荷時設定は dataManager から取得する。
	errCode loadFixedPlanImpl(void)
	{
		g_plan = hgc::cs{};

		// 出荷時設定部分(name/場所/機材/周期/方位/仰角/向き)を取得
		dataManager::factoryFixedPlan(g_plan);

		// 開始=現在(-60秒)、終了=2時間後
		time_t now = std::time(nullptr);
		hgc::dateTime startDt; int off = 0;
		localFromTime(now - 60, startDt, off);
		hgc::dateTime endDt; int off2 = 0;
		localFromTime(now + 2 * 3600, endDt, off2);
		g_offMin = off;
		dataManager::setLogOffset(g_offMin);	// ログのローカル時刻に反映
		g_plan.start = startDt;
		g_plan.end   = endDt;

		astro::ccmSet set = dataManager::currentCcmSet();
		errCode e = astro::buildSchedule(g_plan, set, g_offMin);
		if (e != ERR_HGC_OK) { return e; }

		buildScheduleJson();
		g_planReady = true;
		return ERR_HGC_OK;
	}

	// カメラを SSDP で検索する(ワーカースレッドで実行)。
	// 成功で g_devices に格納し HGE_EV_DEVICE を通知、状態 READY。
	errCode searchSequence(void)
	{
		g_devices.clear();
		size_t n = cameraController::detectTarget(g_devices);
		if (n == 0 || g_devices.empty() || g_devices[0].apiBase == nullptr)
		{
			notifyError(ERR_HGC_NOT_FOUND, "no camera found");
			setState(HGE_ST_IDLE);	// 再検索できるよう IDLE に戻す
			return ERR_HGC_NOT_FOUND;
		}
		notify(HGE_EV_DEVICE, devicesJson());
		logCameraNet();
		setState(HGE_ST_READY);
		return ERR_HGC_OK;
	}

	// 撮影開始シーケンス(検索→スケジュール→撮影ループ起動)。ワーカースレッドで実行。
	errCode startupSequence(void)
	{
		if (!g_planReady)
		{
			errCode e = loadFixedPlanImpl();
			if (e != ERR_HGC_OK) { notifyError(e, "loadFixedPlan"); setState(HGE_ST_ERROR); return e; }
		}
		notify(HGE_EV_SCHEDULE, g_schedJson);

		// カメラ検索(検出済み=手動IP or 自動検索 のカメラがあれば再検索しない)
		bool haveCamera = !g_devices.empty() && g_devices[0].apiBase != nullptr;
		if (!haveCamera)
		{
			g_devices.clear();
			size_t n = cameraController::detectTarget(g_devices);
			if (n == 0 || g_devices.empty() || g_devices[0].apiBase == nullptr)
			{
				notifyError(ERR_HGC_NOT_FOUND, "no camera found");
				setState(HGE_ST_ERROR);
				return ERR_HGC_NOT_FOUND;
			}
		}

		// デバイス一覧を通知
		notify(HGE_EV_DEVICE, devicesJson());
		logCameraNet();

		// 撮影ループの通知配線
		g_runner.setCallbacks(
			[](int s) {
				// START/STOP をログに残す(状態遷移を監視)
				if (s == HGE_ST_CAPTURING && !g_logCapturing)
				{
					g_logCapturing = true;
					g_lastCcm.clear();
					std::string d = "plan=" + g_plan.name +
					                " " + dtToStr(g_plan.start) + "~" + dtToStr(g_plan.end);
					dataManager::logEvent("START", d.c_str());
				}
				else if ((s == HGE_ST_IDLE || s == HGE_ST_ERROR) && g_logCapturing)
				{
					g_logCapturing = false;
					dataManager::logEvent("STOP", "");
				}
				setState(s);
			},
			[](const captureRunner::progressInfo& p) {
				g_pgFrame = p.frame; g_pgTotal = p.total;
				g_pgRemain = p.remainSec; g_pgElapsed = p.elapsedSec;
				char b[96];
				std::snprintf(b, sizeof(b),
					"{\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d}",
					p.frame, p.total, p.remainSec, p.elapsedSec);
				notify(HGE_EV_PROGRESS, b);
			},
			[](const captureRunner::capturedInfo& c) {
				g_pgExp = c.exp;
				char b[160];
				std::snprintf(b, sizeof(b),
					"{\"frame\":%d,\"iso\":%u,\"ss\":%.6f,\"fn\":%.2f,\"luminance\":%.3f}",
					c.frame, c.exp.iso, c.exp.ss, c.exp.fn, c.luminance);
				notify(HGE_EV_CAPTURED, b);
				// 撮影制御方法が切り替わったらログ(CCMSW)
				if (c.ccm != g_lastCcm)
				{
					std::string d = (g_lastCcm.empty() ? "" : g_lastCcm + " -> ") + c.ccm;
					dataManager::logEvent("CCMSW", d.c_str());
					g_lastCcm = c.ccm;
				}
				dataManager::logShot(c.frame, c.exp, c.luminance, c.ccm.c_str());
			},
			[](errCode e, const std::string& m) { notifyError(e, m.c_str()); });

		hgc::exposureSmoothing smooth = dataManager::factorySmoothing();	// 出荷時設定
		errCode e = g_runner.ready(g_plan, &g_devices[0], smooth, g_offMin);
		if (e != ERR_HGC_OK) { notifyError(e, "ready"); setState(HGE_ST_ERROR); return e; }
		return g_runner.start();	// 撮影ループ(別スレッド)を起動。CAPTURING は runner が通知
	}
}

// ============================================================================
//  extern "C" インターフェース
// ============================================================================
extern "C" {

int32_t hge_init(void)
{
	if (g_inited) { return ERR_HGC_OK; }
	netThread::init();
	g_inited = true;
	setState(HGE_ST_IDLE);
	return ERR_HGC_OK;
}

int32_t hge_term(void)
{
	g_runner.stop();
	if (g_startThread)  { ossc::threadEnd(g_startThread);  g_startThread = nullptr; }
	if (g_searchThread) { ossc::threadEnd(g_searchThread); g_searchThread = nullptr; }
	if (g_inited) { netThread::deInit(); g_inited = false; }
	setState(HGE_ST_IDLE);
	return ERR_HGC_OK;
}

const char* hge_version(void)
{
	return VERSION;
}

int32_t hge_setNotify(hgeNotifyCb cb, void* user)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	g_cb = cb; g_user = user;
	return ERR_HGC_OK;
}

int32_t hge_searchDevices(void)
{
	if (g_runner.isRunning()) { return ERR_HGC_INVALID_STATE; }
	if (g_searchThread) { ossc::threadEnd(g_searchThread); g_searchThread = nullptr; }
	setState(HGE_ST_SEARCHING);
	ossc::THREAD_FUNC fn = [](void*) -> errCode { return searchSequence(); };
	g_searchThread = ossc::threadNet(fn, nullptr);
	return ERR_HGC_OK;
}

int32_t hge_connectManual(const char* host)
{
	if (host == nullptr || host[0] == '\0') { return ERR_HGC_INVALID_ARG; }
	size_t n = cameraController::connectManual(g_devices, std::string(host));
	if (n == 0 || g_devices.empty() || g_devices[0].apiBase == nullptr)
	{
		notifyError(ERR_HGC_NOT_FOUND, "manual connect failed");
		return ERR_HGC_NOT_FOUND;
	}
	notify(HGE_EV_DEVICE, devicesJson());
	logCameraNet();
	setState(HGE_ST_READY);
	return ERR_HGC_OK;
}

int32_t hge_loadFixedPlan(void)
{
	return loadFixedPlanImpl();
}

int32_t hge_setUtcOffset(int32_t offMin)
{
	g_offMin = offMin;
	dataManager::setLogOffset(g_offMin);
	return ERR_HGC_OK;
}

int32_t hge_setPlanJson(const char* json, int32_t len)
{
	if (json == nullptr || len <= 0) { return ERR_HGC_INVALID_ARG; }
	hgc::cs plan;
	if (!csjson::fromJson(std::string(json, static_cast<size_t>(len)), plan))
	{
		return ERR_HGC_JSON_PARSE;
	}
	g_plan = plan;
	buildScheduleJson();		// 受信した events/ccmList から表示用JSONを作る
	g_planReady = true;
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanTimes(const char* startIso, const char* endIso, int32_t offMin)
{
	if (startIso == nullptr || endIso == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady)	// 場所/機材など出荷時設定の基礎部を用意する
	{
		errCode e = loadFixedPlanImpl();
		if (e != ERR_HGC_OK) { return e; }
	}
	hgc::dateTime s{}, en{};
	if (std::sscanf(startIso, "%hu-%hu-%huT%hu:%hu:%hu",
	                &s.year, &s.month, &s.day, &s.hour, &s.min, &s.sec) != 6) { return ERR_HGC_INVALID_ARG; }
	if (std::sscanf(endIso, "%hu-%hu-%huT%hu:%hu:%hu",
	                &en.year, &en.month, &en.day, &en.hour, &en.min, &en.sec) != 6) { return ERR_HGC_INVALID_ARG; }

	g_offMin = offMin;
	dataManager::setLogOffset(g_offMin);
	g_plan.start = s;
	g_plan.end   = en;

	astro::ccmSet set = dataManager::factoryCcmSet();
	errCode e = astro::buildSchedule(g_plan, set, g_offMin);	// 開始/終了からスケジュール自動生成
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	g_planReady = true;
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_getScheduleJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady)
	{
		errCode e = loadFixedPlanImpl();
		if (e != ERR_HGC_OK) { return e; }
	}
	int32_t need = static_cast<int32_t>(g_schedJson.size()) + 1;	// 終端含む
	if (buf == nullptr || *inoutLen < need)
	{
		*inoutLen = need;
		return ERR_HGC_BUF_SHORT;
	}
	std::memcpy(buf, g_schedJson.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_getPlanJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady)
	{
		errCode e = loadFixedPlanImpl();
		if (e != ERR_HGC_OK) { return e; }
	}
	std::string s = csjson::toJson(g_plan);
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need)
	{
		*inoutLen = need;
		return ERR_HGC_BUF_SHORT;
	}
	std::memcpy(buf, s.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_getCcmDefaultsJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	std::string s = dataManager::ccmDefaultsJson();
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, s.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_setCcmDefaultsJson(const char* json, int32_t len)
{
	if (json == nullptr || len <= 0) { return ERR_HGC_INVALID_ARG; }
	if (!dataManager::setCcmDefaultsJson(std::string(json, static_cast<size_t>(len))))
	{
		return ERR_HGC_JSON_PARSE;
	}
	// 初期値変更を反映: 計画があればスケジュールを再生成して通知
	if (g_planReady)
	{
		astro::ccmSet set = dataManager::currentCcmSet();
		if (astro::buildSchedule(g_plan, set, g_offMin) == ERR_HGC_OK)
		{
			buildScheduleJson();
			notify(HGE_EV_SCHEDULE, g_schedJson);
		}
	}
	return ERR_HGC_OK;
}

int32_t hge_getProgressJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	char tmp[256];
	std::snprintf(tmp, sizeof(tmp),
		"{\"state\":%d,\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d,"
		"\"ccm\":\"%s\",\"iso\":%u,\"ss\":%.6f,\"fn\":%.2f}",
		g_state.load(), g_pgFrame, g_pgTotal, g_pgRemain, g_pgElapsed,
		jesc(g_lastCcm).c_str(), g_pgExp.iso, g_pgExp.ss, g_pgExp.fn);
	int32_t need = static_cast<int32_t>(std::strlen(tmp)) + 1;
	if (buf == nullptr || *inoutLen < need)
	{
		*inoutLen = need;
		return ERR_HGC_BUF_SHORT;
	}
	std::memcpy(buf, tmp, need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_captureStart(void)
{
	if (g_runner.isRunning()) { return ERR_HGC_INVALID_STATE; }
	if (g_startThread) { ossc::threadEnd(g_startThread); g_startThread = nullptr; }
	setState(HGE_ST_SEARCHING);
	ossc::THREAD_FUNC fn = [](void*) -> errCode { return startupSequence(); };
	g_startThread = ossc::threadNet(fn, nullptr);
	return ERR_HGC_OK;
}

int32_t hge_captureStop(void)
{
	setState(HGE_ST_STOPPING);
	g_runner.stop();
	if (g_startThread) { ossc::threadEnd(g_startThread); g_startThread = nullptr; }
	setState(HGE_ST_IDLE);
	return ERR_HGC_OK;
}

int32_t hge_getState(void)
{
	return g_state.load();
}

} // extern "C"
