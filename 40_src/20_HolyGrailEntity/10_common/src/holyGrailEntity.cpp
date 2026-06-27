#include "common.h"
#include "holyGrailEntity.h"
#include "captureRunner.h"
#include "astroSched.h"
#include "osClock.h"
#include "cameraController.h"
#include "dataManager.h"
#include "csJson.h"
#include "netThread.h"
#include "osSystemCall.h"
#include "cs.h"
#include "ccm.h"
#include "exposureMath.h"
#include <json/nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
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
	std::string           g_editId;	// 編集対象の撮影計画 id(plan_<id>.json)。空=未保存
	// 計画固有の撮影制御方法(初期値ccmとは別管理)。計画作成時に初期値をコピーし、以後独立に編集する。
	astro::ccmSet                 g_planCcm;
	std::shared_ptr<hgc::ccmMoon> g_planMoon;

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
	// ログ用の短い日時(MM-DD HH:MM)。固定長レコードの detail(55B) に収めるため。
	std::string dtShort(const hgc::dateTime& d)
	{
		char b[16];
		std::snprintf(b, sizeof(b), "%02u-%02u %02u:%02u", d.month, d.day, d.hour, d.min);
		return b;
	}
	// --- PLANログ補助 ---
	bool isAutoCcm(hgc::ccmType t)
	{
		return t == hgc::ccmType::sunrise || t == hgc::ccmType::sunset || t == hgc::ccmType::day;
	}
	// 自動露出の目標ev(露出補正)。夜間/移行など非自動は 0。
	double ccmTargetEv(const hgc::ccmBase* c)
	{
		switch (c->type)
		{
		case hgc::ccmType::sunrise: return static_cast<const hgc::ccmSunrise*>(c)->ev;
		case hgc::ccmType::sunset:  return static_cast<const hgc::ccmSunset*>(c)->ev;
		case hgc::ccmType::day:     return static_cast<const hgc::ccmDay*>(c)->ev;
		default:                    return 0.0;
		}
	}
	// iso/ss/fn を簡潔表記(iso/ss/fn)にする。
	std::string expoBrief(const hgc::exposure& e)
	{
		return e.iso + "/" + e.ss + "/" + e.fn;
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

	// unix秒 → ローカル日時(g_offMin 基準)。
	hgc::dateTime localFromUnix(long long sec)
	{
		time_t local = static_cast<time_t>(sec + static_cast<long long>(g_offMin) * 60);
		std::tm g{};
#if defined(_WIN32)
		gmtime_s(&g, &local);
#else
		gmtime_r(&local, &g);
#endif
		hgc::dateTime d;
		d.year = static_cast<uint16_t>(g.tm_year + 1900); d.month = static_cast<uint16_t>(g.tm_mon + 1);
		d.day = static_cast<uint16_t>(g.tm_mday); d.hour = static_cast<uint16_t>(g.tm_hour);
		d.min = static_cast<uint16_t>(g.tm_min); d.sec = static_cast<uint16_t>(g.tm_sec);
		return d;
	}
	double sunAltAtUnix(long long sec) { return astro::sunHoriz(localFromUnix(sec), g_offMin, g_plan.place).altitude; }
	const hgc::ccmBase* activeCcmAtUnix(long long sec)
	{
		for (const auto& w : g_plan.ccmList)
		{
			long long s = hgc::toUnixUtc(w.start, g_offMin), e = hgc::toUnixUtc(w.end, g_offMin);
			if (sec >= s && sec < e) { return w.ccm.get(); }
		}
		return nullptr;
	}

	// 仕様 7.3.2 のスケジュール=太陽高度軸(+6°〜-24°)で夕方/朝方を分けたブロック群を JSON 配列で返す。
	// 各ブロック: {title,date,axis(down=夕/up=朝),segments[{type,name,altTop,altBottom,used}],marks[{label,time,alt}]}
	std::string buildBlocksJson(void)
	{
		const double TOP = 6.0, BOT = -24.0;
		auto clampAlt = [&](double a) { return a > TOP ? TOP : (a < BOT ? BOT : a); };
		long long s0 = hgc::toUnixUtc(g_plan.start, g_offMin), s1 = hgc::toUnixUtc(g_plan.end, g_offMin);
		if (s1 <= s0) { return "[]"; }
		hgc::dateTime d0 = localFromUnix(s0), d1 = localFromUnix(s1);
		bool multiDay = !(d0.year == d1.year && d0.month == d1.month && d0.day == d1.day);

		struct Smp { long long t; double alt; };
		std::vector<Smp> sm;
		for (long long t = s0; t <= s1; t += 60) { sm.push_back({ t, sunAltAtUnix(t) }); }
		if (sm.size() < 2) { return "[]"; }

		auto hms = [&](long long sec) { hgc::dateTime d = localFromUnix(sec); char b[16]; std::snprintf(b, sizeof(b), "%02u:%02u:%02u", d.hour, d.min, d.sec); return std::string(b); };
		auto md  = [&](long long sec) { hgc::dateTime d = localFromUnix(sec); char b[12]; std::snprintf(b, sizeof(b), "%u/%u", d.month, d.day); return std::string(b); };

		// 高度が[-24,+6]に入る極大連続区間を抽出し、内部の最小高度(=太陽南中の逆=深夜)で
		// 下降部(夕方)/上昇部(朝方)に分割する。
		struct Blk { size_t a, b; };
		std::vector<Blk> blks;
		size_t i = 0;
		while (i < sm.size())
		{
			if (!(sm[i].alt <= TOP && sm[i].alt >= BOT)) { ++i; continue; }
			size_t j = i;
			while (j < sm.size() && sm[j].alt <= TOP && sm[j].alt >= BOT) { ++j; }
			// 区間 [i, j-1]。最小高度の位置で分割。
			size_t mn = i; for (size_t k = i; k < j; ++k) { if (sm[k].alt < sm[mn].alt) { mn = k; } }
			bool fallsThenRises = (mn > i && mn < j - 1);
			if (fallsThenRises) { blks.push_back({ i, mn }); blks.push_back({ mn, j - 1 }); }
			else { blks.push_back({ i, j - 1 }); }
			i = j;
		}

		std::string out = "[";
		bool firstBlk = true;
		for (const auto& bk : blks)
		{
			size_t a = bk.a, b = bk.b;
			if (b <= a) { continue; }
			bool down = sm[a].alt > sm[b].alt;	// 夕方=下降
			if (!firstBlk) { out += ","; }
			firstBlk = false;
			out += "{\"title\":\"" + std::string(down ? "\\u5915\\u65b9\\u306e\\u8a08\\u753b" : "\\u671d\\u306e\\u8a08\\u753b") + "\"";
			out += ",\"axis\":\"" + std::string(down ? "down" : "up") + "\"";
			out += ",\"date\":\"" + std::string(multiDay ? md(sm[a].t) : "") + "\"";
			// segments(ccm をポインタ同一でグループ化、高度範囲)
			out += ",\"segments\":[";
			bool fseg = true;
			size_t k = a;
			while (k <= b)
			{
				const hgc::ccmBase* c = activeCcmAtUnix(sm[k].t);
				double mnA = sm[k].alt, mxA = sm[k].alt;
				size_t st = k;
				while (k <= b && activeCcmAtUnix(sm[k].t) == c) { if (sm[k].alt < mnA) mnA = sm[k].alt; if (sm[k].alt > mxA) mxA = sm[k].alt; ++k; }
				if (c)
				{
					char nb[64];
					if (!fseg) out += ","; fseg = false;
					out += "{\"type\":" + std::to_string(static_cast<int>(c->type)) + ",\"name\":\"" + jesc(c->name) + "\"";
					std::snprintf(nb, sizeof(nb), "%.1f", clampAlt(mxA)); out += ",\"altTop\":" + std::string(nb);
					std::snprintf(nb, sizeof(nb), "%.1f", clampAlt(mnA)); out += ",\"altBottom\":" + std::string(nb);
					out += ",\"used\":true}";
				}
				(void)st;
			}
			// 排除した夕日/朝日(使用しない)
			const hgc::ccmBase* exC = nullptr;
			if (down && g_plan.sunsetMode == hgc::bandMode::off && g_planCcm.sunset) exC = g_planCcm.sunset.get();
			if (!down && g_plan.sunriseMode == hgc::bandMode::off && g_planCcm.sunrise) exC = g_planCcm.sunrise.get();
			if (exC)
			{
				double aTop = TOP, aBot = -6.0;
				if (exC->type == hgc::ccmType::sunset) { const auto* s = static_cast<const hgc::ccmSunset*>(exC); aTop = std::max(s->sunAltitude, s->sunAltitudeEnd); aBot = std::min(s->sunAltitude, s->sunAltitudeEnd); }
				else if (exC->type == hgc::ccmType::sunrise) { const auto* s = static_cast<const hgc::ccmSunrise*>(exC); aTop = std::max(s->sunAltitude, s->sunAltitudeEnd); aBot = std::min(s->sunAltitude, s->sunAltitudeEnd); }
				char nb[64];
				if (!fseg) out += ","; fseg = false;
				out += "{\"type\":" + std::to_string(static_cast<int>(exC->type)) + ",\"name\":\"" + jesc(exC->name) + "\"";
				std::snprintf(nb, sizeof(nb), "%.1f", clampAlt(aTop)); out += ",\"altTop\":" + std::string(nb);
				std::snprintf(nb, sizeof(nb), "%.1f", clampAlt(aBot)); out += ",\"altBottom\":" + std::string(nb);
				out += ",\"used\":false}";
			}
			out += "]";
			// marks(境目=種別変化の時刻/高度、開始/終了、月の出入り)
			out += ",\"marks\":[";
			bool fmk = true;
			auto addMark = [&](const std::string& label, long long t, double alt) {
				char nb[16]; if (!fmk) out += ","; fmk = false;
				out += "{\"label\":\"" + label + "\",\"time\":\"" + hms(t) + "\",";
				std::snprintf(nb, sizeof(nb), "%.1f", clampAlt(alt)); out += "\"alt\":" + std::string(nb) + "}";
			};
			const hgc::ccmBase* prevC = reinterpret_cast<const hgc::ccmBase*>(1);
			for (size_t m = a; m <= b; ++m)
			{
				const hgc::ccmBase* c = activeCcmAtUnix(sm[m].t);
				if (m == a || c != prevC)
				{
					std::string lbl = (sm[m].t == s0) ? "Start" : "";
					addMark(lbl, sm[m].t, sm[m].alt);
					prevC = c;
				}
			}
			if (sm[b].t == s1) { addMark("End", sm[b].t, sm[b].alt); }
			// 月の出入り
			for (const auto& ev : g_plan.events)
			{
				if (ev.event != hgc::csEvent::moonrise && ev.event != hgc::csEvent::moonset) continue;
				long long mt = hgc::toUnixUtc(ev.when, g_offMin);
				if (mt < sm[a].t || mt > sm[b].t) continue;
				addMark(ev.event == hgc::csEvent::moonrise ? "\\u6708\\u306e\\u51fa" : "\\u6708\\u306e\\u5165\\u308a", mt, sunAltAtUnix(mt));
			}
			out += "]}";
		}
		out += "]";
		return out;
	}

	// 全撮影制御方法の最長ss[秒] + 2 を最小撮影周期として返す(仕様 7.4.2)。
	// 最長ss = 各窓の明るい方向の限界(=最も露出の多い側 limitBright)の ss。
	int minIntervalSec(const hgc::cs& plan)
	{
		double maxSs = 0.0;
		for (const auto& w : plan.ccmList)
		{
			if (!w.ccm) { continue; }
			double s = expo::parseValue(w.ccm->limitBright.ss, expo::expoKind::ss);
			if (s > maxSs) { maxSs = s; }
		}
		return static_cast<int>(std::ceil(maxSs)) + 2;
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
		// 機材詳細(センサー/焦点距離)と画角[°](方位磁石・仰角ウィジェットの目安)
		std::snprintf(num, sizeof(num), "%.1f", g_plan.camera.sensorSize);
		j += ",\"sensorW\":" + std::string(num);
		std::snprintf(num, sizeof(num), "%.1f", g_plan.camera.sensorSizeV);
		j += ",\"sensorH\":" + std::string(num);
		std::snprintf(num, sizeof(num), "%.0f", g_plan.lens.focalLength);
		j += ",\"focalLength\":" + std::string(num);
		j += ",\"pixelW\":" + std::to_string(g_plan.camera.sensorPixel);
		std::snprintf(num, sizeof(num), "%.1f", g_plan.lens.fn);
		j += ",\"fn\":" + std::string(num);
		astro::fov fovDeg = astro::calcFov(g_plan.camera, g_plan.lens, g_plan.landscape);
		std::snprintf(num, sizeof(num), "%.1f", fovDeg.h);
		j += ",\"fovH\":" + std::string(num);
		std::snprintf(num, sizeof(num), "%.1f", fovDeg.v);
		j += ",\"fovV\":" + std::string(num);
		// NPF シャッター速度[秒](参考表示)と最小撮影周期[秒](最長ss+2)、帯モード。
		double npf = expo::npfShutterSec(g_plan.camera.sensorSize, static_cast<double>(g_plan.camera.sensorPixel),
		                                 g_plan.lens.focalLength, g_plan.lens.fn);
		std::snprintf(num, sizeof(num), "%.2f", npf);
		j += ",\"npf\":" + std::string(num);
		j += ",\"minInterval\":" + std::to_string(minIntervalSec(g_plan));
		j += ",\"sunriseMode\":" + std::to_string(static_cast<int>(g_plan.sunriseMode));
		j += ",\"sunsetMode\":"  + std::to_string(static_cast<int>(g_plan.sunsetMode));
		// 太陽/月の出没方位(方位磁石マーカー用)。範囲内に該当イベントがあれば付与。
		for (const auto& ev : g_plan.events)
		{
			const char* key = nullptr;
			astro::horiz hz{};
			switch (ev.event)
			{
			case hgc::csEvent::sunrise: key = "sunriseAz"; hz = astro::sunHoriz(ev.when, g_offMin, g_plan.place); break;
			case hgc::csEvent::sunset:  key = "sunsetAz";  hz = astro::sunHoriz(ev.when, g_offMin, g_plan.place); break;
			case hgc::csEvent::moonrise:key = "moonriseAz";hz = astro::moonHoriz(ev.when, g_offMin, g_plan.place); break;
			case hgc::csEvent::moonset: key = "moonsetAz"; hz = astro::moonHoriz(ev.when, g_offMin, g_plan.place); break;
			default: break;
			}
			if (key && j.find(std::string("\"") + key + "\"") == std::string::npos)
			{
				std::snprintf(num, sizeof(num), "%.1f", hz.azimuth);
				j += ",\"" + std::string(key) + "\":" + std::string(num);
			}
		}
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
			double sa = astro::sunHoriz(w.start, g_offMin, g_plan.place).altitude;	// 境目(開始)の太陽高度
			double ea = astro::sunHoriz(w.end,   g_offMin, g_plan.place).altitude;	// 境目(終了)の太陽高度
			char ab[32];
			j += "{\"type\":" + std::to_string(ct) +
			     ",\"name\":\"" + jesc(nm) + "\"" +
			     ",\"start\":\"" + dtToStr(w.start) + "\"" +
			     ",\"end\":\"" + dtToStr(w.end) + "\"";
			std::snprintf(ab, sizeof(ab), "%.1f", sa); j += ",\"startAlt\":" + std::string(ab);
			std::snprintf(ab, sizeof(ab), "%.1f", ea); j += ",\"endAlt\":" + std::string(ab);
			j += "}";
		}
		j += "],\"blocks\":" + buildBlocksJson() + "}";
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
			      "\",\"friendly\":\"" + jesc(d.friendName) +
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

	// UTC時刻(t) → ローカル日時 と UTCオフセット[分]。
	// オフセットはプラットフォーム依存(osclock): Android=端末TZ / エッジ端末=受信して永続化した値。
	void localFromTime(time_t t, hgc::dateTime& d, int& offMin)
	{
		offMin = osclock::utcOffsetMin();
		time_t local = t + static_cast<time_t>(offMin) * 60;	// 現地の壁時計時刻
		std::tm g{};
#if defined(_WIN32)
		gmtime_s(&g, &local);
#else
		gmtime_r(&local, &g);	// local を UTC として分解 = 現地の年月日時分秒
#endif
		d.year  = static_cast<uint16_t>(g.tm_year + 1900);
		d.month = static_cast<uint16_t>(g.tm_mon + 1);
		d.day   = static_cast<uint16_t>(g.tm_mday);
		d.hour  = static_cast<uint16_t>(g.tm_hour);
		d.min   = static_cast<uint16_t>(g.tm_min);
		d.sec   = static_cast<uint16_t>(g.tm_sec);
	}

	// --- 複数撮影計画(§7.4)の補助 ---
	// 撮影計画 id を現在のローカル時刻から採番する(yyyyMMdd-HHmmss、衝突時 -NN)。
	std::string makePlanId(void)
	{
		time_t now = std::time(nullptr);
		hgc::dateTime d; int off = 0;
		localFromTime(now, d, off);
		char base[20];
		std::snprintf(base, sizeof(base), "%04u%02u%02u-%02u%02u%02u",
		              d.year, d.month, d.day, d.hour, d.min, d.sec);
		std::vector<std::string> ids = dataManager::listPlanIds();
		auto exists = [&](const std::string& x) {
			for (const auto& e : ids) { if (e == x) { return true; } }
			return false;
		};
		if (!exists(base)) { return std::string(base); }
		for (int n = 2; n < 100; ++n)
		{
			char s[24];
			std::snprintf(s, sizeof(s), "%s-%02d", base, n);
			if (!exists(s)) { return std::string(s); }
		}
		return std::string(base);
	}

	// 現在(編集対象)の計画を保存ラッパー JSON {"planCcm":..,"plan":..} にする。
	std::string wrapCurrentPlan(void)
	{
		return "{\"planCcm\":" + dataManager::ccmSetToJson(g_planCcm, g_planMoon) +
		       ",\"plan\":" + csjson::toJson(g_plan) + "}";
	}

	// 現在の計画を plan_<g_editId>.json へ保存する(id 未割当なら採番)。
	errCode saveCurrentPlan(void)
	{
		if (g_editId.empty()) { g_editId = makePlanId(); }
		return dataManager::savePlanFile(g_editId, wrapCurrentPlan()) ? ERR_HGC_OK : ERR_HGC_INVALID_STATE;
	}

	// plan_<id>.json を編集対象として読み込み、スケジュールを再生成する。成功で g_editId=id。
	// 機材は出荷時で上書きする(現行 MVP 方針を踏襲。計画ごとのカメラ束縛は Phase3 で対応)。
	errCode loadPlanById(const std::string& id)
	{
		std::string saved, planJson, ccmJson;
		if (!dataManager::loadPlanFile(id, saved) ||
		    !dataManager::splitSavedPlan(saved, planJson, ccmJson) ||
		    !csjson::fromJson(planJson, g_plan)) { return ERR_HGC_NO_ELEMENT; }
		// 保存済みの機材(カメラ/レンズ)を尊重する(計画ごとの機材束縛/定数編集の永続化)。
		// 機種が空(壊れた保存)のときだけ出荷時で補う。
		if (g_plan.camera.model.empty())
		{
			hgc::cs fp; dataManager::factoryFixedPlan(fp);
			g_plan.camera = fp.camera;
			g_plan.lens   = fp.lens;
		}
		if (ccmJson.empty() || !dataManager::parseCcmSetJson(ccmJson, g_planCcm, g_planMoon))
		{ dataManager::parseCcmSetJson(dataManager::ccmDefaultsJson(), g_planCcm, g_planMoon); }
		if (!g_planMoon) { g_planMoon = dataManager::factoryMoon(); }
		astro::buildSchedule(g_plan, g_planCcm, g_offMin);
		buildScheduleJson();
		g_editId = id;
		g_planReady = true;
		return ERR_HGC_OK;
	}

	// 出荷時の固定計画を新規生成して現在(編集対象)に据える(開始=現在-60秒、終了=2時間後)。
	void makeFactoryCurrent(const char* name)
	{
		time_t now = std::time(nullptr);
		g_plan = hgc::cs{};
		dataManager::factoryFixedPlan(g_plan);
		if (name) { g_plan.name = name; }
		hgc::dateTime startDt; int o1 = 0; localFromTime(now - 60, startDt, o1);
		hgc::dateTime endDt;   int o2 = 0; localFromTime(now + 2 * 3600, endDt, o2);
		g_plan.start = startDt;
		g_plan.end   = endDt;
		dataManager::parseCcmSetJson(dataManager::ccmDefaultsJson(), g_planCcm, g_planMoon);
		if (!g_planMoon) { g_planMoon = dataManager::factoryMoon(); }
		astro::buildSchedule(g_plan, g_planCcm, g_offMin);
		buildScheduleJson();
	}

	// 起動時の撮影計画準備。旧 plan.json があれば新形式へ移行し、既存計画があれば最新を復元、
	// 無ければ出荷時の固定計画を新規作成して保存する。
	errCode loadFixedPlanImpl(void)
	{
		// 端末のローカルオフセット(TZは安定)を先に求めておく。
		time_t now = std::time(nullptr);
		hgc::dateTime nowDt; int off = 0;
		localFromTime(now, nowDt, off);
		g_offMin = off;
		dataManager::setLogOffset(g_offMin);

		// 旧単一ファイル plan.json があれば新形式 plan_<id>.json へ移行する。
		{
			std::string legacy;
			if (dataManager::loadPlanJson(legacy))
			{
				std::string id = makePlanId();
				if (dataManager::savePlanFile(id, legacy)) { dataManager::removeLegacyPlan(); }
			}
		}

		// 既存計画があれば最新(id 昇順の末尾)を編集対象として復元する。
		std::vector<std::string> ids = dataManager::listPlanIds();
		if (!ids.empty() && loadPlanById(ids.back()) == ERR_HGC_OK)
		{
			return ERR_HGC_OK;
		}

		// 無ければ出荷時の固定計画を作成して保存する。
		makeFactoryCurrent(nullptr);
		g_editId = makePlanId();
		dataManager::savePlanFile(g_editId, wrapCurrentPlan());
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
		// 接続時にシリアル/フレンドリ名を所持カメラへ自動保存(無ければ自動作成。§5.2拡張)。
		dataManager::recordConnectedCamera(g_devices[0]);
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

		// 接続したカメラのシリアル/フレンドリ名を所持カメラへ自動保存(§5.2拡張)。
		// モデル一致の所持カメラへ反映、無ければ master+device から自動作成(1台運用で無設定OK)。
		dataManager::recordConnectedCamera(g_devices[0]);

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
					// 撮影計画の内容もログに残す(後からどの計画で撮ったか検証するため)。
					char pb[56];
					std::snprintf(pb, sizeof(pb), "int=%.0fs az=%.0f el=%.0f",
					              g_plan.interval, g_plan.azimuth, g_plan.elevation);
					dataManager::logEvent("PLAN", pb);
					// 各撮影制御方法の適用区間(夜間/朝日/夕日/日中 …)。
					for (const auto& w : g_plan.ccmList)
					{
						if (!w.ccm) { continue; }
						const hgc::ccmBase* pc = w.ccm.get();
						// 初期値(§4.4の起点)= ccm の initial(exposure)。
						const hgc::exposure& pInit = pc->initial;
						char pev[16];
						if (isAutoCcm(pc->type))                  { std::snprintf(pev, sizeof(pev), "%+.1f", ccmTargetEv(pc)); }
						else if (pc->type == hgc::ccmType::night) { std::snprintf(pev, sizeof(pev), "fix"); }
						else                                      { std::snprintf(pev, sizeof(pev), "-"); }
						// PLAN: 適用区間 + iso/ss/fn の明所限界/暗所限界/初期値/露出補正の目標。
						// 注: 表示名は UI と同じ(明所限界=limitDark, 暗所限界=limitBright)。
						std::string wd = pc->name + " " + dtShort(w.start) + "~" + dtShort(w.end) +
						                 " 明所=" + expoBrief(pc->limitDark) +
						                 " 暗所=" + expoBrief(pc->limitBright) +
						                 " 初期=" + expoBrief(pInit) +
						                 " 目標ev=" + pev;
						dataManager::logEvent("PLAN", wd.c_str());
					}
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
				char b[200];
				std::snprintf(b, sizeof(b),
					"{\"frame\":%d,\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\",\"luminance\":%.3f}",
					c.frame, c.exp.iso.c_str(), c.exp.ss.c_str(), c.exp.fn.c_str(), c.luminance);
				notify(HGE_EV_CAPTURED, b);
				// 撮影制御方法が切り替わったらログ(CCMSW)
				if (c.ccm != g_lastCcm)
				{
					std::string d = (g_lastCcm.empty() ? "" : g_lastCcm + " -> ") + c.ccm;
					dataManager::logEvent("CCMSW", d.c_str());
					g_lastCcm = c.ccm;
				}
				dataManager::logShot(c.frame, c.exp, c.luminance, c.ccm.c_str(), c.metered);
			},
			[](errCode e, const std::string& m) { notifyError(e, m.c_str()); });

		hgc::exposureSmoothing smooth = dataManager::currentSmoothing();	// 全体設定(無ければ出荷時)
		errCode e = g_runner.ready(g_plan, &g_devices[0], smooth, g_offMin);
		if (e != ERR_HGC_OK) { notifyError(e, "ready"); setState(HGE_ST_ERROR); return e; }
		return g_runner.start();	// 撮影ループ(別スレッド)を起動。CAPTURING は runner が通知
	}
}

// ============================================================================
//  extern "C" インターフェース
// ============================================================================
extern "C" {

// 文字列をバッファ規約で返す共通処理(定義は機材セクション。先行使用のため前方宣言)。
static int32_t copyOut(const std::string& s, char* buf, int32_t* inoutLen);

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
	// 接続時にシリアル/フレンドリ名を所持カメラへ自動保存(無ければ自動作成。§5.2拡張)。
	dataManager::recordConnectedCamera(g_devices[0]);
	setState(HGE_ST_READY);
	return ERR_HGC_OK;
}

int32_t hge_loadFixedPlan(void)
{
	return loadFixedPlanImpl();
}

int32_t hge_setUtcOffset(int32_t offMin)
{
	osclock::setUtcOffsetMin(offMin);	// 永続化(エッジ端末)。次回起動後の固定計画でも使う。
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

	// 計画固有ccm(編集済みなら維持)を用いてスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	g_planReady = true;
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanDirection(double azimuth, double elevation)
{
	if (!g_planReady)	// 場所/機材など出荷時設定の基礎部を用意する
	{
		errCode e = loadFixedPlanImpl();
		if (e != ERR_HGC_OK) { return e; }
	}
	// 方位は 0..360 に正規化、仰角は -90..90 にクランプ
	while (azimuth >= 360.0) { azimuth -= 360.0; }
	while (azimuth < 0.0)    { azimuth += 360.0; }
	if (elevation >  90.0) { elevation =  90.0; }
	if (elevation < -90.0) { elevation = -90.0; }
	g_plan.azimuth   = azimuth;
	g_plan.elevation = elevation;

	// 撮影方向が変わると「太陽が画角に入る時刻」が変わるためスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
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

int32_t hge_savePlan(void)
{
	if (!g_planReady)
	{
		errCode e = loadFixedPlanImpl();
		if (e != ERR_HGC_OK) { return e; }
	}
	return saveCurrentPlan();	// plan_<g_editId>.json へ保存(id 未割当なら採番)
}

// --- 複数撮影計画(§7.4) ---
int32_t hge_listPlansJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	time_t now = std::time(nullptr);
	std::string j = "[";
	bool first = true;
	for (const std::string& id : dataManager::listPlanIds())
	{
		std::string saved, planJson, ccmJson; hgc::cs cs;
		if (!dataManager::loadPlanFile(id, saved) ||
		    !dataManager::splitSavedPlan(saved, planJson, ccmJson) ||
		    !csjson::fromJson(planJson, cs)) { continue; }
		long long endU = hgc::toUnixUtc(cs.end, g_offMin);
		bool capturable = endU > static_cast<long long>(now);
		int st = (id == g_editId) ? g_state.load() : static_cast<int>(HGE_ST_IDLE);
		if (!first) { j += ","; }
		first = false;
		j += "{\"id\":\"" + jesc(id) + "\",\"name\":\"" + jesc(cs.name) + "\"" +
		     ",\"start\":\"" + dtToStr(cs.start) + "\",\"end\":\"" + dtToStr(cs.end) + "\"" +
		     ",\"capturable\":" + std::string(capturable ? "true" : "false") +
		     ",\"state\":" + std::to_string(st) + "}";
	}
	j += "]";
	return copyOut(j, buf, inoutLen);
}

int32_t hge_newPlan(const char* presetName)
{
	(void)presetName;	// Phase0: 出荷時設定のみ(プリセット連携は後続フェーズ)
	if (!g_planReady) { loadFixedPlanImpl(); }	// g_offMin 確保
	makeFactoryCurrent("新規撮影計画");
	g_editId = makePlanId();
	g_planReady = true;
	dataManager::savePlanFile(g_editId, wrapCurrentPlan());
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_copyPlan(const char* id)
{
	if (id == nullptr || id[0] == '\0') { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	errCode e = loadPlanById(std::string(id));	// 元計画を現在へ読み込む
	if (e != ERR_HGC_OK) { return e; }
	g_plan.name += " コピー";
	g_editId = makePlanId();			// 別 id で複製保存
	buildScheduleJson();				// 名称変更を反映
	dataManager::savePlanFile(g_editId, wrapCurrentPlan());
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_deletePlan(const char* id)
{
	if (id == nullptr || id[0] == '\0') { return ERR_HGC_INVALID_ARG; }
	if (!dataManager::deletePlanFile(std::string(id))) { return ERR_HGC_NO_ELEMENT; }
	if (g_editId == std::string(id))
	{
		// 編集対象を消した → 残りの最新へ。無ければ出荷時を新規作成。
		g_editId.clear();
		std::vector<std::string> ids = dataManager::listPlanIds();
		if (!ids.empty()) { loadPlanById(ids.back()); }
		else
		{
			makeFactoryCurrent(nullptr);
			g_editId = makePlanId();
			g_planReady = true;
			dataManager::savePlanFile(g_editId, wrapCurrentPlan());
		}
		notify(HGE_EV_SCHEDULE, g_schedJson);
	}
	return ERR_HGC_OK;
}

int32_t hge_selectPlan(const char* id)
{
	if (id == nullptr || id[0] == '\0') { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }	// g_offMin 確保
	errCode e = loadPlanById(std::string(id));
	if (e != ERR_HGC_OK) { return e; }
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_getCurrentPlanId(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	return copyOut(g_editId, buf, inoutLen);
}

// 計画固有の撮影制御方法(night/sunrise/sunset/day/moon)を JSON で取得(バッファ規約)。
int32_t hge_getPlanCcmJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	std::string s = dataManager::ccmSetToJson(g_planCcm, g_planMoon);
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, s.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

// 計画固有ccmを JSON で設定し、スケジュールを再生成して HGE_EV_SCHEDULE で通知する。
int32_t hge_setPlanCcmJson(const char* json, int32_t len)
{
	if (json == nullptr || len <= 0) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	astro::ccmSet set;
	std::shared_ptr<hgc::ccmMoon> moon;
	if (!dataManager::parseCcmSetJson(std::string(json, static_cast<size_t>(len)), set, moon))
	{ return ERR_HGC_JSON_PARSE; }
	g_planCcm  = set;
	g_planMoon = moon ? moon : dataManager::factoryMoon();
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	// 仕様 7.4.2: ssが撮影周期-2を超えたら撮影周期を自動的に最長ss+2へ伸ばす。
	int mn = minIntervalSec(g_plan);
	if (g_plan.interval < static_cast<double>(mn)) { g_plan.interval = mn; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanInterval(double seconds)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	int mn = minIntervalSec(g_plan);
	if (seconds < static_cast<double>(mn)) { return ERR_HGC_INVALID_ARG; }	// 最小未満は不可(UIで「先にssを変更」警告)
	g_plan.interval = seconds;
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanLandscape(int32_t landscape)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	g_plan.landscape = (landscape != 0);
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);	// 画角が変わる
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanGearConstJson(const char* json)
{
	if (json == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	nlohmann::json o = nlohmann::json::parse(json, nullptr, false);
	if (o.is_discarded() || !o.is_object()) { return ERR_HGC_JSON_PARSE; }
	if (o.contains("sensorW"))     { g_plan.camera.sensorSize  = o.value("sensorW", g_plan.camera.sensorSize); }
	if (o.contains("sensorH"))     { g_plan.camera.sensorSizeV = o.value("sensorH", g_plan.camera.sensorSizeV); }
	if (o.contains("pixelW"))      { g_plan.camera.sensorPixel = o.value("pixelW", g_plan.camera.sensorPixel); }
	if (o.contains("focalLength")) { g_plan.lens.focalLength   = o.value("focalLength", g_plan.lens.focalLength); }
	if (o.contains("fn"))          { g_plan.lens.fn            = o.value("fn", g_plan.lens.fn); }
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setBandMode(int32_t sunriseMode, int32_t sunsetMode)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	auto cl = [](int32_t m) { return (m < 0 || m > 2) ? hgc::bandMode::autoDetect : static_cast<hgc::bandMode>(m); };
	g_plan.sunriseMode = cl(sunriseMode);
	g_plan.sunsetMode  = cl(sunsetMode);
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setBoundary(int32_t beforeType, int32_t afterType, int32_t occ, const char* whenIso)
{
	if (whenIso == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	hgc::dateTime w{};
	if (std::sscanf(whenIso, "%hu-%hu-%huT%hu:%hu:%hu",
	                &w.year, &w.month, &w.day, &w.hour, &w.min, &w.sec) != 6) { return ERR_HGC_INVALID_ARG; }
	hgc::boundaryOverride bo;
	bo.before = static_cast<hgc::ccmType>(beforeType);
	bo.after  = static_cast<hgc::ccmType>(afterType);
	bo.occ    = static_cast<uint16_t>(occ < 0 ? 0 : occ);
	bo.when   = w;
	bool replaced = false;
	for (auto& b : g_plan.boundaries)
	{
		if (b.before == bo.before && b.after == bo.after && b.occ == bo.occ) { b.when = w; replaced = true; break; }
	}
	if (!replaced) { g_plan.boundaries.push_back(bo); }
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setBoundaryByAlt(int32_t beforeType, int32_t afterType, int32_t occ, double altDeg, int32_t rising)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	// 既存の (before,after,occ) 境目の日付を探索の基準にする(無ければ計画開始日)。
	hgc::dateTime base = g_plan.start;
	{
		int cnt = 0;
		for (size_t i = 0; i + 1 < g_plan.ccmList.size(); ++i)
		{
			int bt = g_plan.ccmList[i].ccm ? static_cast<int>(g_plan.ccmList[i].ccm->type) : 0;
			int at = g_plan.ccmList[i + 1].ccm ? static_cast<int>(g_plan.ccmList[i + 1].ccm->type) : 0;
			if (bt == beforeType && at == afterType) { if (cnt == occ) { base = g_plan.ccmList[i].end; break; } ++cnt; }
		}
	}
	astro::altTime at = astro::sunAltitudeTime(g_plan.place, base, altDeg, rising != 0, g_offMin);
	if (!at.valid) { return ERR_HGC_NOT_FOUND; }
	char iso[32];
	std::snprintf(iso, sizeof(iso), "%04u-%02u-%02uT%02u:%02u:%02u",
	              at.when.year, at.when.month, at.when.day, at.when.hour, at.when.min, at.when.sec);
	return hge_setBoundary(beforeType, afterType, occ, iso);
}

int32_t hge_clearScheduleEdits(void)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	g_plan.sunriseMode = hgc::bandMode::autoDetect;
	g_plan.sunsetMode  = hgc::bandMode::autoDetect;
	g_plan.boundaries.clear();
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
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

int32_t hge_getExpoValuesJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	double fmin = (g_plan.lens.fn > 0.0) ? g_plan.lens.fn : 1.0;
	double fmax = (g_plan.lens.fnMax > 0.0) ? g_plan.lens.fnMax : 32.0;	// レンズのF最大があれば使う
	auto iso = expo::standardValues(expo::expoKind::iso);
	auto ss  = expo::standardValues(expo::expoKind::ss);
	auto fn  = expo::standardFn(fmin, fmax);
	auto arr = [](const std::vector<std::string>& v) {
		std::string s = "[";
		for (size_t i = 0; i < v.size(); ++i) { if (i) { s += ","; } s += "\"" + v[i] + "\""; }
		s += "]";
		return s;
	};
	std::string j = "{\"iso\":" + arr(iso) + ",\"ss\":" + arr(ss) + ",\"fn\":" + arr(fn) + "}";
	int32_t need = static_cast<int32_t>(j.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, j.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_getCameraAbilityJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	// 接続済みカメラが無ければ検索する。
	if (g_devices.empty() || g_devices[0].apiBase == nullptr)
	{
		g_devices.clear();
		cameraController::detectTarget(g_devices);
	}
	if (g_devices.empty() || g_devices[0].apiBase == nullptr) { return ERR_HGC_NOT_FOUND; }

	cmdt::shotRange r;
	errCode e = cameraController::getSettings(g_devices[0], r);
	if (e != ERR_HGC_OK) { return e; }

	auto arr = [](const std::vector<std::string>& v) {
		std::string s = "[";
		for (size_t i = 0; i < v.size(); ++i) { if (i) { s += ","; } s += "\"" + jesc(v[i]) + "\""; }
		s += "]";
		return s;
	};
	std::string j = "{\"iso\":" + arr(r.iso) + ",\"ss\":" + arr(r.ss) + ",\"fn\":" + arr(r.fNum) + "}";
	int32_t need = static_cast<int32_t>(j.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, j.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_sunAltitudeTimes(int32_t altitudeDeg, char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	double alt = static_cast<double>(altitudeDeg);
	astro::altTime st = astro::sunAltitudeTime(g_plan.place, g_plan.start, alt, false, g_offMin);
	astro::altTime en = astro::sunAltitudeTime(g_plan.place, g_plan.start, alt, true,  g_offMin);
	auto fmt = [](const astro::altTime& a) -> std::string {
		if (!a.valid) { return "--:--"; }
		char t[32];
		std::snprintf(t, sizeof(t), "%02d/%02d %02d:%02d", a.when.month, a.when.day, a.when.hour, a.when.min);
		return std::string(t);
	};
	std::string j = "{\"start\":\"" + fmt(st) + "\",\"end\":\"" + fmt(en) + "\"}";
	int32_t need = static_cast<int32_t>(j.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, j.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

// ============================================================================
//  機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6)
// ============================================================================
// 文字列をバッファ規約で返す共通処理(内部リンケージ)。
static int32_t copyOut(const std::string& s, char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, s.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

int32_t hge_getMasterCamerasJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::masterCamerasJson(), buf, inoutLen);
}

int32_t hge_getMasterLensesJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::masterLensesJson(), buf, inoutLen);
}

int32_t hge_getOwnedCamerasJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::ownedCamerasJson(), buf, inoutLen);
}

int32_t hge_getOwnedLensesJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::ownedLensesJson(), buf, inoutLen);
}

int32_t hge_addOwnedCamera(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::addOwnedCameraFromMaster(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_addOwnedLens(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::addOwnedLensFromMaster(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_removeOwnedCamera(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::removeOwnedCamera(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_removeOwnedLens(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::removeOwnedLens(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_setOwnedCameraAutoInsert(const char* name, int32_t autoInsert)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setOwnedCameraAutoInsert(std::string(name), autoInsert != 0) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_setPlanCamera(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	hgc::camera c;
	if (!dataManager::findOwnedCamera(std::string(name), c)) { return ERR_HGC_NO_ELEMENT; }
	g_plan.camera = c;
	// センサーサイズ/画角が変わると太陽の画角侵入時刻が変わるためスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setPlanLens(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	hgc::lens l;
	if (!dataManager::findOwnedLens(std::string(name), l)) { return ERR_HGC_NO_ELEMENT; }
	g_plan.lens = l;
	// 焦点距離が変わると画角が変わるためスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return ERR_HGC_OK;
}

int32_t hge_setOwnedCameraDetail(const char* origName, const char* json)
{
	if (origName == nullptr || json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setOwnedCameraDetailJson(std::string(origName), std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

int32_t hge_setOwnedLensDetail(const char* origName, const char* json)
{
	if (origName == nullptr || json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setOwnedLensDetailJson(std::string(origName), std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

int32_t hge_getColorsJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::colorsJson(), buf, inoutLen);
}

int32_t hge_setColorsJson(const char* json)
{
	if (json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setColorsJson(std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

int32_t hge_getSmoothingJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::smoothingJson(), buf, inoutLen);
}

int32_t hge_setSmoothingJson(const char* json)
{
	if (json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setSmoothingJson(std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

int32_t hge_pruneOldLogs(int32_t offMin)
{
	return dataManager::pruneOldLogs(offMin);
}

int32_t hge_getCcmPresetsJson(const char* type, char* buf, int32_t* inoutLen)
{
	if (type == nullptr) { return ERR_HGC_INVALID_ARG; }
	return copyOut(dataManager::ccmPresetsJson(std::string(type)), buf, inoutLen);
}

int32_t hge_setCcmPreset(const char* type, const char* origName, const char* json)
{
	if (type == nullptr || origName == nullptr || json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setCcmPresetJson(std::string(type), std::string(origName), std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

int32_t hge_removeCcmPreset(const char* type, const char* name)
{
	if (type == nullptr || name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::removeCcmPreset(std::string(type), std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_getPreferredCcm(const char* type, char* buf, int32_t* inoutLen)
{
	if (type == nullptr) { return ERR_HGC_INVALID_ARG; }
	return copyOut(dataManager::preferredCcmName(std::string(type)), buf, inoutLen);
}

int32_t hge_setPreferredCcm(const char* type, const char* name)
{
	if (type == nullptr || name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setPreferredCcm(std::string(type), std::string(name)) ? ERR_HGC_OK : ERR_HGC_INVALID_STATE;
}

// 接続カメラ検索(同期)。検出した全カメラを g_devices に格納し、一覧 JSON を返す。
//  [{"model":..,"friendly":..,"serial":..}, ...]。コンテキストメニューの「接続カメラ検索」用。
int32_t hge_searchDevicesListJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	// バッファ規約では buf==null(サイズ問い合わせ)→ buf!=null(本取得)の順で2回呼ばれる。
	// 検索は重いので初回(サイズ問い合わせ)だけ実行し、結果(g_devices)を両呼び出しで共有する。
	if (buf == nullptr)
	{
		g_devices.clear();
		cameraController::detectTarget(g_devices);
	}
	std::string s = "[";
	for (size_t i = 0; i < g_devices.size(); ++i)
	{
		if (i) { s += ","; }
		const auto& d = g_devices[i];
		s += "{\"model\":\"" + jesc(d.model) + "\",\"friendly\":\"" + jesc(d.friendName) +
		     "\",\"serial\":\"" + jesc(d.serialno) + "\"}";
	}
	s += "]";
	return copyOut(s, buf, inoutLen);
}

// 検索で見つかったカメラ(g_devices[index])を所持カメラへ追加/更新する。
int32_t hge_addOwnedDetected(int32_t index)
{
	if (index < 0 || static_cast<size_t>(index) >= g_devices.size()) { return ERR_HGC_NO_ELEMENT; }
	return dataManager::recordConnectedCamera(g_devices[index]) ? ERR_HGC_OK : ERR_HGC_INVALID_STATE;
}

int32_t hge_getProgressJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	char tmp[256];
	std::snprintf(tmp, sizeof(tmp),
		"{\"state\":%d,\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d,"
		"\"ccm\":\"%s\",\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\"}",
		g_state.load(), g_pgFrame, g_pgTotal, g_pgRemain, g_pgElapsed,
		jesc(g_lastCcm).c_str(), g_pgExp.iso.c_str(), g_pgExp.ss.c_str(), g_pgExp.fn.c_str());
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
