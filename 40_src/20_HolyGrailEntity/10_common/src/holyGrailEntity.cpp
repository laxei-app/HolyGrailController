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
	void*                 g_searchThread = nullptr;	// カメラ自動検索ワーカー
	bool                  g_inited = false;

	// --- 並行撮影セッション(Phase3。計画ごとに1セッション=1ランナー+1カメラ) ---
	// 同時実行は当面 MAX_CONCURRENT まで。将来はこの定数とカメラ台数を増やせば拡張できる。
	constexpr size_t MAX_CONCURRENT = 2;
	struct captureSession
	{
		std::string                   planId;
		hgc::cs                       plan;
		astro::ccmSet                 planCcm;
		std::shared_ptr<hgc::ccmMoon> planMoon;
		std::unique_ptr<captureRunner> runner;
		class device                  dev;					// このセッション専用のカメラ(アドレス安定。撮影開始の都度ディスカバリで再取得)
		std::atomic<int>              state{ HGE_ST_IDLE };
		bool                          logCapturing = false;	// START/STOP検出
		std::string                   lastCcm;				// CCMSW検出
		void*                         startThread = nullptr;
	};
	std::vector<std::unique_ptr<captureSession>> g_sessions;

	// --- SSDP受動待ち受け → 取得フェーズのポーク配線(3b) ---
	// 待ち受けスレッド(cameraController の共有リスナ)から呼ばれる onAppear は、取得フェーズ中の
	// 各 runner を pokeAcquire して60秒待ちを前倒しする。g_sessions を待ち受けスレッドから直接
	// 触らず、専用の軽量ロック付きレジストリ経由にして競合/デッドロックを避ける。
	std::mutex                    g_pokeMutex;
	std::vector<captureRunner*>   g_pokeRunners;	// 撮影中セッションの runner(生存中のみ登録)
	bool                          g_watching = false;	// 共有SSDPリスナ稼働中か

	void pokeRegister(captureRunner* r)
	{
		if (r == nullptr) { return; }
		std::lock_guard<std::mutex> lk(g_pokeMutex);
		g_pokeRunners.push_back(r);
	}
	void pokeUnregister(captureRunner* r)
	{
		std::lock_guard<std::mutex> lk(g_pokeMutex);
		g_pokeRunners.erase(std::remove(g_pokeRunners.begin(), g_pokeRunners.end(), r), g_pokeRunners.end());
	}
	void pokeAllAcquiring(void)	// 待ち受けスレッドから: 取得フェーズの runner を全て前倒し
	{
		std::lock_guard<std::mutex> lk(g_pokeMutex);
		for (auto* r : g_pokeRunners) { if (r) { r->pokeAcquire(); } }
	}
	void ensureWatching(void)	// 撮影セッション開始時に共有SSDPリスナを起動(冪等)
	{
		if (g_watching) { return; }
		cameraController::watchStart([]() { pokeAllAcquiring(); });
		g_watching = true;
	}
	void stopWatchingIfIdle(void)	// セッションが無くなったら待ち受けを止める
	{
		if (g_watching && g_sessions.empty())
		{
			cameraController::watchStop();
			g_watching = false;
		}
	}

	// 進捗スナップショット(progress(get) 応答・エッジ端末用。最後に撮影したセッションの値)
	int                   g_pgFrame = 0, g_pgTotal = 0, g_pgRemain = 0, g_pgElapsed = 0;
	hgc::exposure         g_pgExp{};
	std::string           g_pgCcm;	// 最後の撮影制御方法名

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

		// 薄明にかからない純夜間の断片(最高高度が低い)はブロックにしない。
		std::vector<Blk> kept;
		for (const auto& bk : blks)
		{
			if (bk.b <= bk.a) { continue; }
			double mx = -90.0; for (size_t k = bk.a; k <= bk.b; ++k) { if (sm[k].alt > mx) { mx = sm[k].alt; } }
			if (mx > -16.0) { kept.push_back(bk); }	// -16°より上に届く=薄明/直接撮影にかかる
		}

		std::string out = "[";
		for (size_t bi = 0; bi < kept.size(); ++bi)
		{
			size_t a = kept[bi].a, b = kept[bi].b;
			bool down = sm[a].alt > sm[b].alt;	// 夕方=下降
			int sunType = down ? 3 : 2;			// このブロックの直接撮影(夕方=夕日3 / 朝=朝日2)
			if (bi) { out += ","; }
			out += "{\"title\":\"" + std::string(down ? "\\u5915\\u65b9\\u306e\\u8a08\\u753b" : "\\u671d\\u306e\\u8a08\\u753b") + "\"";
			out += ",\"axis\":\"" + std::string(down ? "down" : "up") + "\"";
			out += ",\"date\":\"" + std::string(multiDay ? md(sm[a].t) : "") + "\"";
			// segments(ccm をポインタ同一でグループ化、高度範囲)。後処理のため一旦集める。
			struct Seg { int type; std::string name; double altTop; double altBottom; };
			std::vector<Seg> segs;
			bool hasSunDirect = false;
			{
				size_t k = a;
				while (k <= b)
				{
					const hgc::ccmBase* c = activeCcmAtUnix(sm[k].t);
					double mnA = sm[k].alt, mxA = sm[k].alt;
					while (k <= b && activeCcmAtUnix(sm[k].t) == c) { if (sm[k].alt < mnA) mnA = sm[k].alt; if (sm[k].alt > mxA) mxA = sm[k].alt; ++k; }
					if (c)
					{
						int ty = static_cast<int>(c->type);
						if (ty == 2 || ty == 3) { ty = sunType; hasSunDirect = true; }	// ブロック方向に種別を統一(item4)
						segs.push_back({ ty, c->name, clampAlt(mxA), clampAlt(mnA) });
					}
				}
			}
			// 直接撮影(夕日/朝日)が +6°(上端)まで広がり日中が消えても、日中↔夕日/朝日の境目を
			// 掴み直して戻せるよう、0 幅の日中セグメントを上端に差し込む(夕方=先頭/朝=末尾)。
			if (hasSunDirect && g_planCcm.day && !segs.empty())
			{
				size_t edge = down ? 0 : segs.size() - 1;
				// クリップ端(最上端)のセグメントが直接撮影=日中が +6° まで潰れている状態。
				// (60秒サンプリングのため上端は +5.9° 程度になる。+4.5° 以上で潰れと判定)
				if (segs[edge].type == sunType && segs[edge].altTop >= 4.5)
				{
					Seg dz{ 4, g_planCcm.day->name, TOP, TOP };
					if (down) segs.insert(segs.begin(), dz); else segs.push_back(dz);
				}
			}
			out += ",\"segments\":[";
			bool fseg = true;
			for (const auto& s : segs)
			{
				char nb[64];
				if (!fseg) out += ","; fseg = false;
				out += "{\"type\":" + std::to_string(s.type) + ",\"name\":\"" + jesc(s.name) + "\"";
				std::snprintf(nb, sizeof(nb), "%.1f", s.altTop); out += ",\"altTop\":" + std::string(nb);
				std::snprintf(nb, sizeof(nb), "%.1f", s.altBottom); out += ",\"altBottom\":" + std::string(nb);
				out += ",\"used\":true}";
			}
			// 直接撮影(夕日/朝日)が使用中に無ければ「使用しない」に置く(挿入できるように。item3)。
			if (!hasSunDirect)
			{
				const hgc::ccmBase* sc = down ? (g_planCcm.sunset ? static_cast<hgc::ccmBase*>(g_planCcm.sunset.get()) : nullptr)
				                              : (g_planCcm.sunrise ? static_cast<hgc::ccmBase*>(g_planCcm.sunrise.get()) : nullptr);
				if (!fseg) out += ","; fseg = false;
				out += "{\"type\":" + std::to_string(sunType) + ",\"name\":\"" + jesc(sc ? sc->name : "") + "\"";
				out += ",\"altTop\":3.0,\"altBottom\":0.0,\"used\":false}";
			}
			out += "]";
			// marks: 内部の可動境目(種別変化の時刻/高度)＋開始/終了＋月出入り。
			//  ・ブロック先頭(クリップ端 +6° や、計画開始と一致して動かせない境目)はマーク化しない。
			//  ・撮影開始/終了は計画の start/end 時刻を最初/最後のブロックに必ず表示。
			//    範囲外(日中/深夜)の時は実際の太陽高度を渡し、View 側でブロック外側に描く。
			out += ",\"marks\":[";
			bool fmk = true;
			auto addMark = [&](const std::string& label, long long t, double alt) {
				char nb[16]; if (!fmk) out += ","; fmk = false;
				out += "{\"label\":\"" + label + "\",\"time\":\"" + hms(t) + "\",";
				std::snprintf(nb, sizeof(nb), "%.1f", alt); out += "\"alt\":" + std::string(nb) + "}";
			};
			const hgc::ccmBase* prevC = activeCcmAtUnix(sm[a].t);
			for (size_t m = a + 1; m <= b; ++m)
			{
				const hgc::ccmBase* c = activeCcmAtUnix(sm[m].t);
				if (c != prevC) { addMark("", sm[m].t, clampAlt(sm[m].alt)); prevC = c; }	// 内部の可動境目のみ
			}
			if (bi == 0) { addMark("Start", s0, sunAltAtUnix(s0)); }
			if (bi == kept.size() - 1) { addMark("End", s1, sunAltAtUnix(s1)); }
			// 月の出入り
			for (const auto& ev : g_plan.events)
			{
				if (ev.event != hgc::csEvent::moonrise && ev.event != hgc::csEvent::moonset) continue;
				long long mt = hgc::toUnixUtc(ev.when, g_offMin);
				if (mt < sm[a].t || mt > sm[b].t) continue;
				addMark(ev.event == hgc::csEvent::moonrise ? "\\u6708\\u306e\\u51fa" : "\\u6708\\u306e\\u5165\\u308a", mt, clampAlt(sunAltAtUnix(mt)));
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
		if (!g_sessions.empty())	// 撮影中は手動再検索で表示用デバイス一覧を作り直さない(一覧の安定のため。各撮影は自分専用の device で動作)
		{
			notify(HGE_EV_DEVICE, devicesJson());
			setState(HGE_ST_READY);
			return ERR_HGC_OK;
		}
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

	// --- 並行撮影セッション補助(Phase3) ---
	captureSession* sessionFor(const std::string& planId)
	{
		for (auto& s : g_sessions) { if (s->planId == planId) { return s.get(); } }
		return nullptr;
	}
	// 集約状態(どれか撮影中なら CAPTURING 系、無ければ IDLE)。legacy hge_getState 用。
	void refreshAggregateState(void)
	{
		int agg = HGE_ST_IDLE;
		for (auto& s : g_sessions) { int st = s->state.load(); if (st == HGE_ST_CAPTURING || st == HGE_ST_SEARCHING || st == HGE_ST_STOPPING || st == HGE_ST_DISCONNECTED || st == HGE_ST_WAITING || st == HGE_ST_NOCAMERA) { agg = st; } }
		g_state = agg;
	}
	// 撮影実行状況(/asset/capturing.json)に「撮影の意図」を追加/削除する(item2: 電源復帰/再起動の再開用)。
	//  on  = 撮影開始(意図あり)。off = 完了 or 明示停止。
	// 失敗(ERROR/接続断)や予期せぬ電源断では呼ばず意図を残す → 再起動時に再試行する。
	// 何も撮影していないときは空配列 [] がファイルに残る(=その旨を明示)。ファイル自体は消さない。
	void setCapturing(const std::string& planId, bool on)
	{
		if (planId.empty()) { return; }
		std::vector<std::string> ids;
		dataManager::loadCapturingIds(ids);
		auto it = std::find(ids.begin(), ids.end(), planId);
		bool changed = false;
		if (on && it == ids.end())       { ids.push_back(planId); changed = true; }
		else if (!on && it != ids.end()) { ids.erase(it);         changed = true; }
		if (changed) { dataManager::saveCapturingIds(ids); }	// 空でも [] を書く(削除しない)
	}

	// --- item3: ccm露出をカメラ(isoList/ssList)・レンズ(fn/fnMax)の上下限へクランプ ---
	std::string fmtFn(double f)
	{
		char b[16];
		if (std::fabs(f - std::round(f)) < 0.05) { std::snprintf(b, sizeof(b), "%.0f", f); }
		else                                     { std::snprintf(b, sizeof(b), "%.1f", f); }
		return std::string(b);
	}
	// 露出(iso/ss/fn)1件をカメラ/レンズの範囲へクランプ。範囲を超えた値のみ限界値へ。
	// 範囲内・リスト未設定・空文字はそのまま(初期値ccm単体=カメラ未規定では呼ばない)。
	void clampExposureToGear(hgc::exposure& e, const hgc::camera& cam, const hgc::lens& lens)
	{
		auto clampList = [](std::string& cur, const std::vector<std::string>& list, expo::expoKind k)
		{
			if (cur.empty() || list.empty()) { return; }
			double v = expo::parseValue(cur, k);
			if (v <= 0) { return; }
			const std::string* lo = nullptr; const std::string* hi = nullptr;
			double loR = 1e300, hiR = -1e300;
			for (const auto& s : list) { double r = expo::parseValue(s, k); if (r <= 0) { continue; } if (r < loR) { loR = r; lo = &s; } if (r > hiR) { hiR = r; hi = &s; } }
			if      (lo && v < loR) { cur = *lo; }
			else if (hi && v > hiR) { cur = *hi; }
		};
		clampList(e.iso, cam.isoList, expo::expoKind::iso);
		clampList(e.ss,  cam.ssList,  expo::expoKind::ss);
		// fn はレンズの開放(fn)〜最小絞り(fnMax)。fnMax 0=未設定なら下限のみ。
		if (!e.fn.empty() && lens.fn > 0.0)
		{
			double v = expo::parseValue(e.fn, expo::expoKind::fn);
			if      (v > 0 && v < lens.fn)               { e.fn = fmtFn(lens.fn); }
			else if (lens.fnMax > 0.0 && v > lens.fnMax) { e.fn = fmtFn(lens.fnMax); }
		}
	}
	void clampCcmSetToGear(astro::ccmSet& set, const hgc::camera& cam, const hgc::lens& lens)
	{
		auto one = [&](hgc::ccmBase* c) { if (!c) { return; } clampExposureToGear(c->limitBright, cam, lens); clampExposureToGear(c->limitDark, cam, lens); clampExposureToGear(c->initial, cam, lens); };
		one(set.night.get()); one(set.sunrise.get()); one(set.sunset.get()); one(set.day.get());
	}
	// 編集中計画のccm一式を、その計画のカメラ/レンズの上下限へクランプする(item3)。
	void clampPlanCcmToGear(void) { clampCcmSetToGear(g_planCcm, g_plan.camera, g_plan.lens); }

	// --- item5: 名称の重複回避(リスト+分割バー画面共通の方針) ---
	// names に重複しない名前を返す。base が空き=base、使用中=base+"1","2"...。
	std::string uniqueName(const std::string& base, const std::vector<std::string>& names)
	{
		auto exists = [&](const std::string& n) { for (const auto& e : names) { if (e == n) { return true; } } return false; };
		if (!exists(base)) { return base; }
		for (int i = 1; i < 100000; ++i) { std::string c = base + std::to_string(i); if (!exists(c)) { return c; } }
		return base;
	}
	// 保存済み全計画の名称(excludeId は除外=改名時の自分自身を除く)。編集中計画は未保存の現名を使う。
	std::vector<std::string> collectPlanNames(const std::string& excludeId)
	{
		std::vector<std::string> names;
		for (const std::string& id : dataManager::listPlanIds())
		{
			if (id == excludeId) { continue; }
			if (id == g_editId && g_planReady) { names.push_back(g_plan.name); continue; }
			std::string saved, pj, cj; hgc::cs cs;
			if (dataManager::loadPlanFile(id, saved) && dataManager::splitSavedPlan(saved, pj, cj) && csjson::fromJson(pj, cs)) { names.push_back(cs.name); }
		}
		return names;
	}
	void notifyStateP(const std::string& planId, int s)
	{
		std::string j = "{\"planId\":\"" + jesc(planId) + "\",\"state\":" + std::to_string(s) + "}";
		notify(HGE_EV_STATE, j);
	}

	// device の urlAccess/location から host(IP)を取り出す。
	// "http://192.168.1.7:8080/ccapi" -> "192.168.1.7"。失敗時は空。
	std::string hostFromDevice(const class device& d)
	{
		auto extract = [](const std::string& url) -> std::string {
			auto p = url.find("://");
			if (p == std::string::npos) { return std::string(); }
			size_t s = p + 3, e = s;
			while (e < url.size() && url[e] != ':' && url[e] != '/') { ++e; }
			return url.substr(s, e - s);
		};
		std::string h = extract(d.urlAccess);
		if (h.empty()) { h = extract(d.location); }
		return h;
	}

	// 1セッションの撮影開始シーケンス(カメラ割当→配線→ループ起動)。ワーカースレッドで実行。
	errCode startSessionSequence(captureSession* S)
	{
		S->state = HGE_ST_SEARCHING; notifyStateP(S->planId, HGE_ST_SEARCHING); refreshAggregateState();
		// 撮影開始操作をした時点(=実際の撮影開始時刻より前。例: 1時間後開始の計画でもタップ時)で
		// カメラ検索が走ることを記録する。後続の「カメラ接続 …」(成功経路) または ERR「…見つかりません」と
		// 対になる。タップ時刻はこのログのタイムスタンプ、撮影窓は detail で分かる。
		{
			std::string d = "撮影開始操作 計画=" + S->plan.name +
			                " 窓=" + dtToStr(S->plan.start) + "~" + dtToStr(S->plan.end) +
			                " カメラ検索(SSDP)開始";
			dataManager::logEvent("NET", d.c_str());
		}
		// 撮影開始(=このシーケンスの実行)の都度、計画のカメラを SSDP で検索する。
		//  ・カメラのIPはオートパワーオフ→再起動(DHCP)で変わるため、前回のIPは使い回さず毎回探す。
		//  ・撮影開始"時刻"ではなく「撮影開始操作をした時点」で検索する(本関数はワーカーで即実行され、
		//    実際の露光開始までは runner が開始時刻まで待機する)。
		//  ・複数同時撮影でも、これから撮るカメラは動作中のものとは必ず別の1台。各セッションは自分専用の
		//    device(S->dev: アドレス安定)を持つので、検索しても実行中の他セッションを壊さない。
		std::vector<class device> found;
		cameraController::detectTarget(found);

		const hgc::camera& pc = S->plan.camera;
		// 他の動作中セッションが使用中のカメラ(serial)は割り当て不可(同じカメラで二重撮影しない)。
		auto serialBusy = [&](const std::string& serial) -> bool {
			if (serial.empty()) { return false; }
			for (auto& s : g_sessions)
			{
				if (s.get() == S) { continue; }
				int st = s->state.load();
				bool active = (st == HGE_ST_CAPTURING || st == HGE_ST_SEARCHING || st == HGE_ST_READY || st == HGE_ST_STOPPING || st == HGE_ST_WAITING || st == HGE_ST_NOCAMERA);
				if (active && s->dev.apiBase && s->dev.serialno == serial) { return true; }
			}
			return false;
		};

		// §4b: 計画のカメラを特定する。同機種(例 EOS R10)が複数あっても、シリアルNo か フレンドリ名 で1台を選ぶ。
		//  ・serial 指定           → そのシリアルの個体。
		//  ・friendly 指定         → 所持リストからシリアルを解決して個体特定(未接続でserial不明なら下のモデル照合へ)。
		//  ・どちらも無い("それ以外のEOS-R10") → 同機種で、既に識別済み(所持リストにシリアル登録済み)の個体"以外"の1台。
		std::string wantSerial = pc.serial;
		if (wantSerial.empty() && !pc.friendly.empty())
		{
			std::string s; if (dataManager::serialForFriendly(pc.friendly, s)) { wantSerial = s; }
		}
		const bool hasModel = !pc.model.empty() || !pc.name.empty();

		// SSDP広告を止めるカメラ(スマホ接続後など)対策: 計画カメラの直近IPが分かっていれば、
		// SSDPで見つからなくてもそのIPへCCAPI直接接続を試し、応答すれば候補(found)に加える。
		// 本人確認(serial/model)は下の特定処理に委ねる。serial未解決の計画は最後に繋がったhostを使う。
		std::string manualUrl;	// 直近IP直結で追加した候補のurlAccess(どの経路で繋がったかのログ判定用)
		{
			std::string host = dataManager::loadCameraHost(wantSerial);
			if (!host.empty())
			{
				// 既に SSDP で同じ host が見つかっていれば手動接続はしない(種別非依存で host 比較)。
				bool already = false;
				for (auto& d : found) { if (d.apiBase && hostFromDevice(d) == host) { already = true; break; } }
				if (!already)
				{
					std::vector<class device> man;
					if (cameraController::connectManual(man, host) > 0 && man[0].apiBase)
					{
						manualUrl = man[0].urlAccess;
						found.push_back(man[0]);	// apiBase はポインタ共有(device は解放しない設計)
					}
				}
			}
		}

		class device* hit = nullptr;
		if (!wantSerial.empty())
		{	// 特定個体(シリアル)を探す。見つからなくても中断せず武装する(3a: 未検出許容)。
			for (auto& d : found) { if (d.apiBase && d.serialno == wantSerial) { hit = &d; break; } }
			if (hit != nullptr && serialBusy(wantSerial))
			{	// 既に撮影中のカメラを使う計画は開始しない(これは正当な拒否。未検出とは別)。
				notifyError(ERR_HGC_INVALID_STATE, "このカメラは既に撮影中です");
				S->state = HGE_ST_ERROR; notifyStateP(S->planId, HGE_ST_ERROR); refreshAggregateState();
				return ERR_HGC_INVALID_STATE;
			}
		}
		else if (hasModel)
		{	// シリアル未解決だが機種は指定 → 「それ以外の個体(=識別済みでない同機種)」を選ぶ。
			// 見つからなくても中断せず武装する(3a: 未検出許容)。
			std::vector<std::string> knownSerials; dataManager::ownedCameraSerials(knownSerials);
			auto isKnown = [&](const std::string& s) -> bool { for (auto& k : knownSerials) { if (k == s) { return true; } } return false; };
			for (auto& d : found) { if (d.apiBase && dataManager::cameraModelMatches(d, pc) && !isKnown(d.serialno) && !serialBusy(d.serialno)) { hit = &d; break; } }
			if (hit == nullptr)	// 該当が無ければ同機種の空き1台で代替。
			{ for (auto& d : found) { if (d.apiBase && dataManager::cameraModelMatches(d, pc) && !serialBusy(d.serialno)) { hit = &d; break; } } }
		}
		else
		{	// 計画にカメラ未指定: 他セッションが使っていない最初の1台。見つからなくても武装(3a)。
			for (auto& d : found) { if (d.apiBase && !serialBusy(d.serialno)) { hit = &d; break; } }
		}

		if (hit != nullptr)
		{
			S->dev = *hit;	// セッション専用 device へコピー(アドレス安定。実行中の他セッションに影響しない)
			// どの経路でカメラに繋がったか(SSDP発見 / 直近IP直結)と IP をログに残す。次回SSDPが
			// 応答しない時のため、今回つながったカメラのIPも保存する(SSDP広告停止対策)。
			{
				std::string h = hostFromDevice(S->dev);
				const bool viaCache = (!manualUrl.empty() && S->dev.urlAccess == manualUrl);
				std::string d = std::string("カメラ接続 ") + (viaCache ? "直近IP直結(SSDP不発)" : "SSDP発見")
				              + " ip=" + (h.empty() ? "?" : h)
				              + (S->dev.serialno.empty() ? "" : (" serial=" + S->dev.serialno));
				dataManager::logEvent("NET", d.c_str());
				if (!h.empty()) { dataManager::saveCameraHost(S->dev.serialno, h); }
			}
			// 表示用デバイス一覧(g_devices)も最新へ反映(同serialは更新、無ければ追加)。runner は g_devices を参照しない。
			{
				int gi = -1;
				for (size_t i = 0; i < g_devices.size(); ++i) { if (g_devices[i].serialno == S->dev.serialno) { gi = (int)i; break; } }
				if (gi >= 0) { g_devices[gi] = *hit; } else { g_devices.push_back(*hit); }
			}
			notify(HGE_EV_DEVICE, devicesJson());
			dataManager::recordConnectedCamera(S->dev);
		}
		else
		{	// 3a: カメラ未検出のまま撮影要求を受理する。中断せず、runner が取得まで NOCAMERA(✖点灯)で
			// 探し続ける(60秒ごと再探索/直近IP直結)。撮影窓の終了か中止まで継続。
			S->dev.clear();
			dataManager::logEvent("NET", "カメラ未検出のまま撮影要求を受理(探索を継続)");
		}

		S->runner->setCallbacks(
			[S](int s) {
				if (s == HGE_ST_CAPTURING && !S->logCapturing)
				{
					S->logCapturing = true; S->lastCcm.clear();
					std::string d = "plan=" + S->plan.name + " " + dtToStr(S->plan.start) + "~" + dtToStr(S->plan.end);
					dataManager::logEvent("START", d.c_str());
					char pb[56]; std::snprintf(pb, sizeof(pb), "int=%.0fs az=%.0f el=%.0f", S->plan.interval, S->plan.azimuth, S->plan.elevation);
					dataManager::logEvent("PLAN", pb);
					for (const auto& w : S->plan.ccmList)
					{
						if (!w.ccm) { continue; }
						const hgc::ccmBase* pc = w.ccm.get();
						char pev[16];
						if (isAutoCcm(pc->type))                  { std::snprintf(pev, sizeof(pev), "%+.1f", ccmTargetEv(pc)); }
						else if (pc->type == hgc::ccmType::night) { std::snprintf(pev, sizeof(pev), "fix"); }
						else                                      { std::snprintf(pev, sizeof(pev), "-"); }
						std::string wd = pc->name + " " + dtShort(w.start) + "~" + dtShort(w.end) +
						                 " 明所=" + expoBrief(pc->limitDark) + " 暗所=" + expoBrief(pc->limitBright) +
						                 " 初期=" + expoBrief(pc->initial) + " 目標ev=" + pev;
						dataManager::logEvent("PLAN", wd.c_str());
					}
				}
				else if ((s == HGE_ST_IDLE || s == HGE_ST_ERROR) && S->logCapturing)
				{
					S->logCapturing = false; dataManager::logEvent("STOP", "");
				}
				S->state = s; notifyStateP(S->planId, s); refreshAggregateState();
				// 実行状況: 撮影窓を終えてのIDLE(=完了)のみ意図を消す。失敗(ERROR/接続断)や
				// 窓内での停止/シャットダウンでは残し、再起動時に再開できるようにする。
				if (s == HGE_ST_IDLE &&
				    static_cast<long long>(std::time(nullptr)) >= hgc::toUnixUtc(S->plan.end, g_offMin))
				{ setCapturing(S->planId, false); }
			},
			[S](const captureRunner::progressInfo& p) {
				g_pgFrame = p.frame; g_pgTotal = p.total; g_pgRemain = p.remainSec; g_pgElapsed = p.elapsedSec;
				char b[160];
				std::snprintf(b, sizeof(b), "{\"planId\":\"%s\",\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d}",
				              S->planId.c_str(), p.frame, p.total, p.remainSec, p.elapsedSec);
				notify(HGE_EV_PROGRESS, b);
			},
			[S](const captureRunner::capturedInfo& c) {
				g_pgExp = c.exp; g_pgCcm = c.ccm;
				char b[256];
				std::snprintf(b, sizeof(b), "{\"planId\":\"%s\",\"frame\":%d,\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\",\"luminance\":%.3f}",
				              S->planId.c_str(), c.frame, c.exp.iso.c_str(), c.exp.ss.c_str(), c.exp.fn.c_str(), c.luminance);
				notify(HGE_EV_CAPTURED, b);
				if (c.ccm != S->lastCcm) { std::string d = (S->lastCcm.empty() ? "" : S->lastCcm + " -> ") + c.ccm; dataManager::logEvent("CCMSW", d.c_str()); S->lastCcm = c.ccm; }
				dataManager::logShot(c.frame, c.exp, c.luminance, c.ccm.c_str(), c.metered, c.rdyMeteringMs, c.rdyShutterMs, c.tm0Ms, c.offMs, c.rdyOk, c.setOk, c.shutterMs);
			},
			[S](errCode e, const std::string& m) { notifyError(e, m.c_str()); });

		// カメラの取得/再接続を1回試みる(3a: 初回取得も撮影中再接続もこの1本に集約)。
		//  ・撮影要求時にカメラ未検出でも中断しないため、runner の取得フェーズがこれを繰り返し呼ぶ。
		//  ・撮影中の連続失敗時も同じ経路で再接続する。成功で *dev_(=S->dev)を最新IP/apiへ更新。
		S->runner->setReconnect([S]() -> bool {
			// 計画のカメラ(serial 明示 or friendly から解決)を毎回求める。初回未取得時は S->dev.serialno が
			// 空のため、それに依存しない(旧実装の空serial依存バグを解消)。既知IP直結にもこの serial を使う。
			std::string wantSerial = S->plan.camera.serial;
			if (wantSerial.empty() && !S->plan.camera.friendly.empty())
			{ std::string s; if (dataManager::serialForFriendly(S->plan.camera.friendly, s)) { wantSerial = s; } }
			if (wantSerial.empty() && !S->dev.serialno.empty()) { wantSerial = S->dev.serialno; }

			std::vector<class device> found;
			cameraController::detectTarget(found);
			class device* hit = nullptr;
			// 判明しているシリアルで同一個体を再特定するのが最優先(IPが変わっていても追従)。
			if (!wantSerial.empty())
			{
				for (auto& d : found) { if (d.apiBase && d.serialno == wantSerial) { hit = &d; break; } }
			}
			if (hit == nullptr)	// シリアル不明/不一致なら機種一致でフォールバック。
			{
				for (auto& d : found) { if (d.apiBase && dataManager::cameraModelMatches(d, S->plan.camera)) { hit = &d; break; } }
			}
			const bool viaSsdp = (hit != nullptr);	// SSDP再探索で見つかったか(下の直近IP直結と区別)
			// SSDPで見つからない時は直近IPへCCAPI直接接続(SSDP広告停止対策)。man は hit 確定まで生かす。
			std::vector<class device> man;
			if (hit == nullptr)
			{
				std::string host = dataManager::loadCameraHost(wantSerial);
				if (!host.empty() && cameraController::connectManual(man, host) > 0 && man[0].apiBase)
				{
					bool ok = wantSerial.empty() || man[0].serialno.empty()
					        || man[0].serialno == wantSerial
					        || dataManager::cameraModelMatches(man[0], S->plan.camera);
					if (ok) { hit = &man[0]; }
				}
			}
			if (hit == nullptr)
			{	// SSDP再探索・直近IP直結ともに不可。runner の取得/再接続フェーズが後で再試行する。
				dataManager::logEvent("NET", "カメラ取得/再接続失敗(SSDP/直近IPとも不可)", true);
				return false;
			}
			S->dev = *hit;	// runner の dev_ が指す先(=S->dev)を最新のIP/apiへ更新。
			{
				std::string h = hostFromDevice(S->dev);
				std::string d = std::string("再接続成功 ") + (viaSsdp ? "SSDP発見" : "直近IP直結(SSDP不発)")
				              + " ip=" + (h.empty() ? "?" : h);
				dataManager::logEvent("NET", d.c_str());
				if (!h.empty()) { dataManager::saveCameraHost(S->dev.serialno, h); }
			}
			notify(HGE_EV_DEVICE, devicesJson());
			return true;
		});

		if (hit == nullptr)
		{	// 3a: 未検出のまま武装。即座に NOCAMERA(✖点灯)を反映(runner も取得まで NOCAMERA を出す)。
			S->state = HGE_ST_NOCAMERA; notifyStateP(S->planId, HGE_ST_NOCAMERA); refreshAggregateState();
		}
		hgc::exposureSmoothing smooth = dataManager::currentSmoothing();
		errCode e = S->runner->ready(S->plan, &S->dev, smooth, g_offMin);
		if (e != ERR_HGC_OK) { notifyError(e, "ready"); S->state = HGE_ST_ERROR; notifyStateP(S->planId, HGE_ST_ERROR); refreshAggregateState(); return e; }
		return S->runner->start();
	}

	// 1セッションを停止・破棄する(ランナーを join してから erase)。
	void stopSessionAt(size_t i)
	{
		captureSession* s = g_sessions[i].get();
		pokeUnregister(s->runner ? s->runner.get() : nullptr);	// 3b: ポーク対象から外す(runner破棄前)
		s->state = HGE_ST_STOPPING; notifyStateP(s->planId, HGE_ST_STOPPING); refreshAggregateState();
		if (s->startThread) { ossc::threadEnd(s->startThread); s->startThread = nullptr; }
		if (s->runner) { s->runner->stop(); }	// ワーカースレッドを join(以後コールバック無し)
		std::string pid = s->planId;
		g_sessions.erase(g_sessions.begin() + i);
		notifyStateP(pid, HGE_ST_IDLE);
		refreshAggregateState();
		stopWatchingIfIdle();	// 3b: 最後のセッションが消えたらSSDP待ち受けを停止
		// 注: 実行状況ファイルはここでは触らない。明示停止は hge_captureStopPlan で off にし、
		//     シャットダウン(全停止)では意図を残して再起動時に再開できるようにする。
	}

	// 実行が終了した(ランナー停止)セッションを掃除する。IDLE/ERROR は除去して
	// 再開始できるようにし、エッジが ERROR 表示のまま固まるのを防ぐ。DISCONNECTED
	// (接続断=赤表示)はユーザーが中止するまで残す。ランナー停止済みのみ対象なので安全。
	void reapDeadSessions(void)
	{
		bool removed = false;
		for (size_t i = 0; i < g_sessions.size(); )
		{
			int  st      = g_sessions[i]->state.load();
			bool running = g_sessions[i]->runner && g_sessions[i]->runner->isRunning();
			if (!running && (st == HGE_ST_IDLE || st == HGE_ST_ERROR))
			{
				pokeUnregister(g_sessions[i]->runner ? g_sessions[i]->runner.get() : nullptr);	// 3b
				if (g_sessions[i]->startThread) { ossc::threadEnd(g_sessions[i]->startThread); g_sessions[i]->startThread = nullptr; }
				if (g_sessions[i]->runner)      { g_sessions[i]->runner->stop(); }
				g_sessions.erase(g_sessions.begin() + i);
				removed = true;
			}
			else { ++i; }
		}
		if (removed) { refreshAggregateState(); stopWatchingIfIdle(); }
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
	while (!g_sessions.empty()) { stopSessionAt(g_sessions.size() - 1); }	// 全セッション停止(最後の1つで待ち受けも停止)
	cameraController::watchStop(); g_watching = false;	// 3b: 念のためSSDP待ち受けを確実に停止
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
	if (!g_sessions.empty()) { return ERR_HGC_INVALID_STATE; }	// 撮影中は検索しない
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

// スマホから受信した計画を、指定 id の計画ファイルとして取り込む(エッジが複数計画を蓄積するため)。
// 同じ id の再送は上書き(重複しない)。id 空なら新規採番。
int32_t hge_importPlan(const char* id, const char* json, int32_t len)
{
	int32_t r = hge_setPlanJson(json, len);
	if (r != ERR_HGC_OK) { return r; }
	if (id != nullptr && id[0] != '\0') { g_editId = id; }
	else if (g_editId.empty())          { g_editId = makePlanId(); }
	return saveCurrentPlan();
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
	return saveCurrentPlan();	// 編集を即永続化(エッジ転送/再起動/再選択で失わない)
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
	return saveCurrentPlan();	// 編集を即永続化
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
	// 各計画を {開始時刻(ソートキー), 行JSON} に展開する。
	std::vector<std::pair<long long, std::string>> rows;
	for (const std::string& id : dataManager::listPlanIds())
	{
		hgc::cs cs;
		bool got = false;
		if (id == g_editId && g_planReady)
		{	// 編集中の計画は未保存の変更も即反映する(名称/時刻の変更で撮影可否アイコンが即出るように)。
			cs = g_plan; got = true;
		}
		else
		{
			std::string saved, planJson, ccmJson;
			got = dataManager::loadPlanFile(id, saved) &&
			      dataManager::splitSavedPlan(saved, planJson, ccmJson) &&
			      csjson::fromJson(planJson, cs);
		}
		if (!got) { continue; }
		long long startU = hgc::toUnixUtc(cs.start, g_offMin);
		long long endU   = hgc::toUnixUtc(cs.end, g_offMin);
		bool capturable = endU > static_cast<long long>(now);
		captureSession* sess = sessionFor(id);
		int st = sess ? sess->state.load() : static_cast<int>(HGE_ST_IDLE);
		std::string o = "{\"id\":\"" + jesc(id) + "\",\"name\":\"" + jesc(cs.name) + "\"" +
		     ",\"start\":\"" + dtToStr(cs.start) + "\",\"end\":\"" + dtToStr(cs.end) + "\"" +
		     ",\"capturable\":" + std::string(capturable ? "true" : "false") +
		     ",\"state\":" + std::to_string(st) + "}";
		rows.push_back(std::make_pair(startU, o));
	}
	// 撮影開始時刻の新しい順(降順)に並べる(指示3: 古い計画が上に来ないように)。
	std::stable_sort(rows.begin(), rows.end(),
		[](const std::pair<long long, std::string>& a, const std::pair<long long, std::string>& b) { return a.first > b.first; });
	std::string j = "[";
	for (size_t i = 0; i < rows.size(); ++i) { if (i) { j += ","; } j += rows[i].second; }
	j += "]";
	return copyOut(j, buf, inoutLen);
}

int32_t hge_newPlan(const char* presetName)
{
	(void)presetName;	// Phase0: 出荷時設定のみ(プリセット連携は後続フェーズ)
	if (!g_planReady) { loadFixedPlanImpl(); }	// g_offMin 確保
	std::string nm = uniqueName("新規撮影計画", collectPlanNames(""));	// item5: 既存と重複しない名前
	makeFactoryCurrent("新規撮影計画");
	g_plan.name = nm;
	buildScheduleJson();
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
	g_plan.name = uniqueName(g_plan.name + " コピー", collectPlanNames(""));	// item5: 重複回避
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
	clampPlanCcmToGear();	// item3: 計画のカメラ/レンズ上下限へccm露出をクランプ(初期値選択も含む)
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

// 編集中の撮影計画の名称を変更する(離脱/保存で永続化。リストへは即反映)。
// 撮影計画(id指定)の名称を変更する。編集中の計画なら g_plan も更新して通知。いずれも即永続化(リストへ即反映)。
int32_t hge_renamePlan(const char* id, const char* name)
{
	if (id == nullptr || id[0] == '\0' || name == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	std::string sid = id;
	// item5: 他の計画と同名にはできない(自分自身は除外)。重複なら拒否しUIがポップアップ表示。
	{
		std::vector<std::string> others = collectPlanNames(sid);
		for (const auto& nm : others) { if (nm == std::string(name)) { return ERR_HGC_NAME_DUP; } }
	}
	if (sid == g_editId)
	{
		g_plan.name = name;
		buildScheduleJson();
		notify(HGE_EV_SCHEDULE, g_schedJson);
		return saveCurrentPlan();
	}
	// 非編集中の計画はファイルの plan.name を直接書き換える。
	std::string saved;
	if (!dataManager::loadPlanFile(sid, saved)) { return ERR_HGC_NO_ELEMENT; }
	nlohmann::json w = nlohmann::json::parse(saved, nullptr, false);
	if (w.is_discarded() || !w.is_object()) { return ERR_HGC_JSON_PARSE; }
	if (w.contains("plan") && w["plan"].is_object()) { w["plan"]["name"] = std::string(name); }
	else { w["name"] = std::string(name); }
	return dataManager::savePlanFile(sid, w.dump()) ? ERR_HGC_OK : ERR_HGC_INVALID_STATE;
}

int32_t hge_setPlanLandscape(int32_t landscape)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	g_plan.landscape = (landscape != 0);
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);	// 画角が変わる
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化
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
	return saveCurrentPlan();	// 編集を即永続化
}

int32_t hge_setBandMode(int32_t sunriseMode, int32_t sunsetMode)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	auto cl = [](int32_t m) { return (m < 0 || m > 2) ? hgc::bandMode::autoDetect : static_cast<hgc::bandMode>(m); };
	g_plan.sunriseMode = cl(sunriseMode);
	g_plan.sunsetMode  = cl(sunsetMode);
	g_plan.boundaries.clear();	// 帯の挿入/排除は構造が変わるため、古い境界上書きを破棄(整合性維持)
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
	// 撮影制御方法移動範囲(仕様 7.3.2 追加)。境界に接する種別ペアで可動高度をクランプする。
	//  night=1 sunrise=2 sunset=3 day=4 preNight=6 postNight=7
	{
		auto has = [&](int t) { return beforeType == t || afterType == t; };
		double lo = -24.0, hi = 6.0;
		bool sun = has(2) || has(3), trans = has(6) || has(7);
		if (has(4) && sun)            { lo = 3.0;   hi = 6.0;   }  // 日中↔夕日/朝日(上境界 +6〜+3)
		else if (sun && trans)        { lo = -1.0;  hi = 2.0;   }  // 夕日/朝日↔移行(下境界 +2〜-1)
		else if (has(4) && trans)     { lo = -3.0;  hi = 4.0;   }  // 日中↔移行(日中境界 +4〜-3)
		else if (has(1) && trans)     { lo = -19.0; hi = -12.0; }  // 夜間↔移行(夜間境界 -12〜-19)
		if (altDeg < lo) altDeg = lo;
		if (altDeg > hi) altDeg = hi;
	}
	// sunAltitudeTime は基準日の正午から前方探索する。朝(上昇)の交差は正午より前にあるため、
	// そのままだと翌日の上昇交差を拾って境界が翌日へ飛ぶ。基準を前日へ寄せて当日の朝を拾わせる。
	hgc::dateTime searchBase = base;
	if (rising != 0) { searchBase = localFromUnix(hgc::toUnixUtc(base, g_offMin) - 18 * 3600); }
	astro::altTime at = astro::sunAltitudeTime(g_plan.place, searchBase, altDeg, rising != 0, g_offMin);
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
	// item3: iso/ss はスライダ選択範囲も計画のカメラの設定可能範囲(isoList/ssList)に限定する。
	// カメラ未設定時は標準1/3段にフォールバック。ss の "Bulb" はスライダ対象外として除く。
	std::vector<std::string> iso = !g_plan.camera.isoList.empty()
	                               ? g_plan.camera.isoList : expo::standardValues(expo::expoKind::iso);
	std::vector<std::string> ss;
	if (!g_plan.camera.ssList.empty()) { for (const auto& s : g_plan.camera.ssList) { if (s != "Bulb") { ss.push_back(s); } } }
	else                               { ss = expo::standardValues(expo::expoKind::ss); }
	auto fn  = expo::standardFn(fmin, fmax);	// fn はレンズの開放〜最小絞り(従来どおり)
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
	// 接続済みカメラが無ければ検索する(撮影中は作り直さない)。
	if ((g_devices.empty() || g_devices[0].apiBase == nullptr) && g_sessions.empty())
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
	clampPlanCcmToGear();	// item3: 新しいカメラの上下限へccm露出をクランプ
	// センサーサイズ/画角が変わると太陽の画角侵入時刻が変わるためスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化
}

int32_t hge_setPlanLens(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	hgc::lens l;
	if (!dataManager::findOwnedLens(std::string(name), l)) { return ERR_HGC_NO_ELEMENT; }
	g_plan.lens = l;
	clampPlanCcmToGear();	// item3: 新しいレンズの開放/最小絞りへfnをクランプ
	// 焦点距離が変わると画角が変わるためスケジュールを再生成する。
	errCode e = astro::buildSchedule(g_plan, g_planCcm, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化
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
	if (buf == nullptr && g_sessions.empty())	// 撮影中は表示用一覧を作り直さない(各撮影は自分専用の device で動作)
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
	char tmp[320];
	std::snprintf(tmp, sizeof(tmp),
		"{\"state\":%d,\"name\":\"%s\",\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d,"
		"\"ccm\":\"%s\",\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\"}",
		g_state.load(), jesc(g_plan.name).c_str(), g_pgFrame, g_pgTotal, g_pgRemain, g_pgElapsed,
		jesc(g_pgCcm).c_str(), g_pgExp.iso.c_str(), g_pgExp.ss.c_str(), g_pgExp.fn.c_str());
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

// 計画id指定で撮影開始(並行撮影。planId 空=編集対象 g_editId)。
int32_t hge_captureStartPlan(const char* planId_)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	std::string planId = (planId_ && planId_[0]) ? std::string(planId_) : g_editId;
	if (planId.empty()) { return ERR_HGC_INVALID_ARG; }
	reapDeadSessions();		// 終了/エラーで残った同名セッションを掃除し、再開始できるようにする
	if (sessionFor(planId)) { return ERR_HGC_INVALID_STATE; }	// まだ実行中なら二重開始しない
	if (g_sessions.size() >= MAX_CONCURRENT) { notifyError(ERR_HGC_INVALID_STATE, "max concurrent reached"); return ERR_HGC_INVALID_STATE; }

	auto sess = std::make_unique<captureSession>();
	sess->planId = planId;
	sess->state  = HGE_ST_SEARCHING;	// 起動シーケンス実行前から非IDLEにし、reapDeadSessions に消されないようにする
	if (planId == g_editId)
	{
		sess->plan = g_plan; sess->planCcm = g_planCcm; sess->planMoon = g_planMoon;	// 編集中スナップショット
	}
	else
	{
		std::string saved, pj, cj; hgc::cs cs;
		if (!dataManager::loadPlanFile(planId, saved) || !dataManager::splitSavedPlan(saved, pj, cj) || !csjson::fromJson(pj, cs)) { return ERR_HGC_NO_ELEMENT; }
		sess->plan = cs;
		if (cj.empty() || !dataManager::parseCcmSetJson(cj, sess->planCcm, sess->planMoon)) { dataManager::parseCcmSetJson(dataManager::ccmDefaultsJson(), sess->planCcm, sess->planMoon); }
		if (!sess->planMoon) { sess->planMoon = dataManager::factoryMoon(); }
		if (sess->plan.ccmList.empty()) { astro::buildSchedule(sess->plan, sess->planCcm, g_offMin); }
	}
	sess->runner = std::make_unique<captureRunner>();
	captureSession* raw = sess.get();
	g_sessions.push_back(std::move(sess));
	pokeRegister(raw->runner.get());	// 3b: SSDP出現ポークの対象に登録
	ensureWatching();					// 3b: 共有SSDP受動待ち受けを起動(冪等)
	ossc::THREAD_FUNC fn = [raw](void*) -> errCode { return startSessionSequence(raw); };
	raw->startThread = ossc::threadNet(fn, nullptr);
	setCapturing(planId, true);	// item2: 撮影意図を記録(電源復帰/再起動で再開する)
	return ERR_HGC_OK;
}

// 計画id指定で撮影停止(planId 空=編集対象)。
int32_t hge_captureStopPlan(const char* planId_)
{
	std::string planId = (planId_ && planId_[0]) ? std::string(planId_) : g_editId;
	setCapturing(planId, false);	// item2: ユーザーによる明示停止 → 実行意図を消す(再起動で再開しない)
	for (size_t i = 0; i < g_sessions.size(); ++i)
	{
		if (g_sessions[i]->planId == planId) { stopSessionAt(i); return ERR_HGC_OK; }
	}
	return ERR_HGC_OK;
}

// カメラ未検出で待機中の計画に即再探索を促す(継続ボタン/SSDP出現)。planId 空=全取得フェーズ。
int32_t hge_pokeAcquire(const char* planId_)
{
	std::string planId = (planId_ && planId_[0]) ? std::string(planId_) : std::string();
	if (planId.empty()) { pokeAllAcquiring(); return ERR_HGC_OK; }
	captureSession* s = sessionFor(planId);
	if (s && s->runner) { s->runner->pokeAcquire(); }
	return ERR_HGC_OK;
}

// 計画id指定の撮影状態。planId 空=集約状態。
int32_t hge_getStatePlan(const char* planId_)
{
	std::string planId = (planId_ && planId_[0]) ? std::string(planId_) : std::string();
	if (planId.empty()) { return g_state.load(); }
	captureSession* s = sessionFor(planId);
	return s ? s->state.load() : static_cast<int>(HGE_ST_IDLE);
}

// --- legacy(無引数。編集対象計画に作用) ---
int32_t hge_captureStart(void) { return hge_captureStartPlan(nullptr); }
int32_t hge_captureStop(void)  { return hge_captureStopPlan(nullptr); }
int32_t hge_getState(void)     { reapDeadSessions(); return g_state.load(); }

// 電源復帰/アプリ再起動時に、保存した撮影実行状況(/asset/capturing.json)を読み、
// 撮影中だった計画を再開する(item2)。撮影窓を過ぎていればランナーが即終了する。
// return: 再開を試みた計画数。
int32_t hge_resumeCapture(void)
{
	if (!g_inited) { return ERR_HGC_READY; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	std::vector<std::string> ids;
	dataManager::loadCapturingIds(ids);
	int n = 0;
	for (const auto& id : ids)
	{
		if (id.empty() || sessionFor(id)) { continue; }		// 空/既に動作中はスキップ
		std::string saved;
		if (!dataManager::loadPlanFile(id, saved)) { setCapturing(id, false); continue; }	// 計画ファイルが無い→意図クリア
		// 撮影窓が既に終了している計画は再開不要 → 意図をクリアして次回起動で蒸し返さない。
		std::string pj, cj; hgc::cs cs;
		if (dataManager::splitSavedPlan(saved, pj, cj) && csjson::fromJson(pj, cs) &&
		    static_cast<long long>(std::time(nullptr)) >= hgc::toUnixUtc(cs.end, g_offMin))
		{ setCapturing(id, false); continue; }
		if (hge_captureStartPlan(id.c_str()) == ERR_HGC_OK) { ++n; }
	}
	return n;
}

} // extern "C"
