#include "common.h"
#include "holyGrailEntity.h"
#include "captureRunner.h"
#include "tool.h"			// tool::sleep(予約セッションの開始待ち)
#include "astroSched.h"
#include "osClock.h"
#include "cameraController.h"
#include "dataManager.h"
#include "roleDiscovery.h"	// 役割別の発見(エッジ=IP直結ヒント / スマホ=スタブ)。30_role
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

	std::vector<device>   g_devices;
	void*                 g_searchThread = nullptr;	// カメラ自動検索ワーカー
	bool                  g_inited = false;

	// --- 並行撮影セッション(Phase3。計画ごとに1セッション=1ランナー+1カメラ) ---
	// 同時に「重なって」撮影できるのは MAX_CONCURRENT(=2)台まで(=カメラ台数)。
	// ただし撮影開始要求(=予約)は MAX_PENDING(=100)件まで登録でき、撮影期間が重ならなければ
	// いくつでも受け付ける(§7.4)。将来はこの定数とカメラ台数を増やせば拡張できる。
	constexpr size_t MAX_CONCURRENT = 2;
	constexpr size_t MAX_PENDING    = 100;	// 自撮影の撮影開始要求(予約含む)の受付上限
	// 撮影期間 = [撮影窓 start の PRE_MARGIN_SEC 秒前, 撮影窓 end の 1フレーム(=interval秒)後]。
	// PRE_MARGIN_SEC は撮影窓前の初期露出収束(captureRunner::kPreConvergeSec=30秒)に一致させる。
	constexpr long long PRE_MARGIN_SEC = 30;
	// 遅延アーム: 予約セッションの開始スレッドは armAt(=窓start-PRE_MARGIN_SEC) の ARM_LEAD_SEC 前まで作らない。
	// 待機セッションごとに 14KB のタスクスタックを常駐させると内部RAM(M5Stack)が断片化し、4件目以降の
	// xTaskCreate が失敗して SEARCHING のまま固まる(2026-07-12 テスト3で実測: free=84KB でも largest=10.7KB)。
	// スレッド生成は hge_pump()(エッジ=loop毎秒/スマホ=UIタイマ)が期日に行い、失敗しても再試行する。
	constexpr long long ARM_LEAD_SEC = 60;
	struct captureSession
	{
		std::string                   planId;
		hgc::cs                       plan;
		std::unique_ptr<captureRunner> runner;
		class device                  dev;					// このセッション専用のカメラ(アドレス安定。撮影開始の都度ディスカバリで再取得)
		std::atomic<int>              state{ HGE_ST_IDLE };
		int                           slot = 0;				// Phase4: 同時撮影のずらしスロット(周期内位置=slot/MAX_CONCURRENT)
		bool                          logCapturing = false;	// START/STOP検出
		dataManager::captureReport    report;				// 撮影結果レポートの積算(STOP時にファイルへ出す)
		std::string                   lastCcm;				// CCMSW検出
		void*                         startThread = nullptr;
		std::atomic<bool>             cancel{ false };		// 停止要求(予約待ちの開始スレッドを中断する)
		std::atomic<bool>             deferred{ false };	// 遅延アーム待ち(開始スレッド未生成。hge_pump が期日に生成)
		std::atomic<bool>             seqActive{ false };	// 起動シーケンス実行中(同時1本に直列化して14KBブロックの競合を減らす)
		long long                     armAtEpoch = 0;		// カメラ確保開始時刻(=窓start-PRE_MARGIN_SEC, UTC epoch)
		long long                     nextTryEpoch = 0;		// スレッド生成失敗時の次回再試行時刻(UTC epoch)
		std::atomic<bool>             armed{ false };		// 撮影窓前の予約待ちを抜けカメラ確保フェーズへ入ったか
		// 進捗スナップショット(このセッション専用)。C_PROGRESS の「計画別」応答に使う。集約 g_pg*(最後に
		// 撮影したセッションの値)とは別物。1エッジ複数カメラ時、スマホが計画idごとに正しい状態/進捗を取れる。
		int                           pgFrame = 0, pgTotal = 0, pgRemain = 0, pgElapsed = 0;
		hgc::exposure                 pgExp{};
		std::string                   pgCcm;
	};
	std::vector<std::unique_ptr<captureSession>> g_sessions;

	// 撮影期間(±マージン)が同時に重なる自撮影セッション数の最大が MAX_CONCURRENT を超えるか。
	// 既存の全セッション + 新規1件[ns,ne) を掃引して判定する(半開区間=端点接触は重ならない)。
	bool selfCaptureOverlapExceeds(long long ns, long long ne)
	{
		struct Ev { long long t; int d; };
		std::vector<Ev> ev;
		auto add = [&](long long s, long long e) { if (e > s) { ev.push_back({ s, +1 }); ev.push_back({ e, -1 }); } };
		for (auto& s : g_sessions)
		{
			long long ss = hgc::toUnixUtc(s->plan.start, g_offMin) - PRE_MARGIN_SEC;
			long long se = hgc::toUnixUtc(s->plan.end, g_offMin) + (long long)std::llround(s->plan.interval);
			add(ss, se);
		}
		add(ns, ne);
		std::sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t != b.t ? a.t < b.t : a.d < b.d; });
		int cur = 0, mx = 0;
		for (auto& e : ev) { cur += e.d; if (cur > mx) { mx = cur; } }
		return mx > (int)MAX_CONCURRENT;
	}

	// 既知カメラテーブル(エッジ役: スマホからの cameraInfo プッシュ由来のIP直結ヒント)と、それを使う
	// IP直結+本人確認のオーケストレーションは 30_role/20_edge へ移設した(スマホ役 10_phone はスタブ)。
	// common は hge::role::tryIpDirect / setKnownCameras の窓口(roleDiscovery.h)経由でのみ触れる。

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

	std::string jesc(const std::string& s);	// 前方宣言(定義は後方)

	// ユーザーへの「お知らせ」を流す。**文言は載せない**(2026-08-19 ユーザー指示)。
	//  通信路には番号だけを流し、何と表示するかは受け取ったUIが決める。
	//  n1 の意味は notice ごとに決まる(notice.h 参照。例: cardLow なら残り枚数)。
	//  ログには番号を残す。後から追うのに要るが、これは表示用の文ではない。
	void notifyNotice(const std::string& planId, int noticeCode, long long n1)
	{
		std::string j = "{\"notice\":" + std::to_string(noticeCode) +
		                ",\"n1\":" + std::to_string(n1) +
		                ",\"planId\":\"" + jesc(planId) + "\"}";
		notify(HGE_EV_ERROR, j);
		char d[96];
		std::snprintf(d, sizeof(d), "notice=%d n1=%lld plan=%s", noticeCode, n1, planId.c_str());
		dataManager::logEvent("NOTICE", d);
	}

	void notifyError(errCode code, const char* msg)
	{
		// msg にはHTTP応答本文(引用符・波括弧を含む)がそのまま入ることがある。エスケープせずに
		// 連結するとJSONが壊れ、受信側(AndroidのJSONObject)が例外→アプリごと落ちて全撮影が
		// 停止した(2026-07-26 18:15 実事故: actShutter http=503 {"message":"Device busy"})。
		std::string j = "{\"code\":" + std::to_string(static_cast<unsigned>(code)) +
		                ",\"msg\":\"" + jesc(msg ? msg : "") + "\"}";
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
			const unsigned char u = static_cast<unsigned char>(c);
			if (c == '"' || c == '\\')  { o.push_back('\\'); o.push_back(c); }
			else if (c == '\n')         { o += "\\n"; }
			else if (c == '\r')         { o += "\\r"; }
			else if (c == '\t')         { o += "\\t"; }
			else if (u < 0x20)          { o.push_back(' '); }	// その他の制御文字はJSONを壊すので潰す
			else                        { o.push_back(c); }
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

	// sec を含む撮影制御方法の窓の時間範囲。見つからなければ false。
	//  スケジュール表示の高度は**この境目の時刻**から求める(サンプルの最小/最大ではなく)。
	bool ccmWindowRangeAtUnix(long long sec, long long& outStart, long long& outEnd)
	{
		for (const auto& w : g_plan.ccmList)
		{
			long long s = hgc::toUnixUtc(w.start, g_offMin), e = hgc::toUnixUtc(w.end, g_offMin);
			if (sec >= s && sec < e) { outStart = s; outEnd = e; return true; }
		}
		return false;
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

		// 朝日/夕日の「計画ブロック」は、太陽が地平線付近(=実際に朝日/夕日を撮れる高さ)まで
		// 届く区間だけにする。開始時に既に薄明の途中(例: 20:19で高度-12°の下降)で、そのまま深夜へ
		// 沈むだけの断片は、夕日撮影にかからないためブロックにしない(項目F: 夕方薄明ボタンの誤表示対策)。
		// しきい値=市民薄明の上端付近(-6°)。ここより上に届けば地平線近傍の朝日/夕日がある。
		std::vector<Blk> kept;
		for (const auto& bk : blks)
		{
			if (bk.b <= bk.a) { continue; }
			double mx = -90.0; for (size_t k = bk.a; k <= bk.b; ++k) { if (sm[k].alt > mx) { mx = sm[k].alt; } }
			if (mx > -6.0) { kept.push_back(bk); }	// 地平線近傍(市民薄明以上)に届く=朝日/夕日にかかる
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
			// ブロックの時間範囲(UNIX秒)。概要スケジュールで「どのイベントの隣に置くか」を
			// 時刻で決めるために出す(2026-08-11)。以前はスマホ側が「i番目の朝 → i番目の日の出」と
			// 順番で対応付けていたため、対応する日の出/日の入が撮影窓の外にあるとき
			// (例: 5:00開始で日の出4:56が窓の外)に End の前へ落ちてしまっていた。
			out += ",\"tStart\":" + std::to_string(static_cast<long long>(sm[a].t));
			out += ",\"tEnd\":"   + std::to_string(static_cast<long long>(sm[b].t));
			// segments(ccm をポインタ同一でグループ化、高度範囲)。後処理のため一旦集める。
			struct Seg { int type; std::string name; double altTop; double altBottom; };
			std::vector<Seg> segs;
			bool hasSunDirect = false;
			{
				size_t k = a;
				while (k <= b)
				{
					const hgc::ccmBase* c = activeCcmAtUnix(sm[k].t);
					const size_t k0 = k;
					double mnA = sm[k].alt, mxA = sm[k].alt;
					while (k <= b && activeCcmAtUnix(sm[k].t) == c) { if (sm[k].alt < mnA) mnA = sm[k].alt; if (sm[k].alt > mxA) mxA = sm[k].alt; ++k; }
					if (c)
					{
						// 表示する高度は**窓の境目の時刻**から求める(2026-08-17)。
						//  サンプル(60秒刻み)の最小/最大を使うと、境目を1サンプルぶん行き過ぎた
						//  高度になる。太陽は1分で約0.2°動くので、夜間の境目が -12.0° のはずが
						//  夕方 -12.2° / 朝 -11.8° と表示されていた(符号が逆なのは行き過ぎる向きの違い)。
						//  ブロック(=この図の1枚)の範囲は超えないように切る。
						long long ws = 0, we = 0;
						if (ccmWindowRangeAtUnix(sm[k0].t, ws, we))
						{
							if (ws < sm[a].t) { ws = sm[a].t; }
							if (we > sm[b].t) { we = sm[b].t; }
							const double h1 = sunAltAtUnix(ws), h2 = sunAltAtUnix(we);
							mxA = (h1 > h2) ? h1 : h2;
							mnA = (h1 < h2) ? h1 : h2;
						}
						int ty = static_cast<int>(c->type);
						if (ty == 2 || ty == 3) { ty = sunType; hasSunDirect = true; }	// ブロック方向に種別を統一(item4)
						segs.push_back({ ty, c->name, clampAlt(mxA), clampAlt(mnA) });
					}
				}
			}
			// 直接撮影(夕日/朝日)が +6°(上端)まで広がり日中が消えても、日中↔夕日/朝日の境目を
			// 掴み直して戻せるよう、0 幅の日中セグメントを上端に差し込む(夕方=先頭/朝=末尾)。
			if (hasSunDirect && g_plan.ccm.day && !segs.empty())
			{
				size_t edge = down ? 0 : segs.size() - 1;
				// クリップ端(最上端)のセグメントが直接撮影=日中が +6° まで潰れている状態。
				// (60秒サンプリングのため上端は +5.9° 程度になる。+4.5° 以上で潰れと判定)
				if (segs[edge].type == sunType && segs[edge].altTop >= 4.5)
				{
					Seg dz{ 4, g_plan.ccm.day->name, TOP, TOP };
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
				const hgc::ccmBase* sc = down ? (g_plan.ccm.sunset ? static_cast<hgc::ccmBase*>(g_plan.ccm.sunset.get()) : nullptr)
				                              : (g_plan.ccm.sunrise ? static_cast<hgc::ccmBase*>(g_plan.ccm.sunrise.get()) : nullptr);
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
				if (c != prevC)
				{
					// 境目の時刻/高度は**窓が持つ正確な時刻**から出す(2026-08-17)。サンプル位置
					//  (60秒刻み)をそのまま使うと1サンプルぶん行き過ぎ、太陽は1分で約0.2°動くので
					//  夜間の境目が -12.0° のはずが -12.2°/-11.8° と表示されていた。
					long long ws = 0, we = 0, bt = sm[m].t;
					if (ccmWindowRangeAtUnix(sm[m].t, ws, we) && ws > sm[a].t && ws <= sm[m].t) { bt = ws; }
					addMark("", bt, clampAlt(sunAltAtUnix(bt)));
					prevC = c;
				}
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

	// 計画のカメラの控えで空の項目を所持カメラから引き直す(実体は下)。
	void applyOwnedCameraSettings(hgc::camera& cam);

	// --- スケジュールの JSON 生成 ---
	void buildScheduleJson(void)
	{
		// 表示を作る前に、計画のカメラの控えで空になっている項目を所持カメラから引き直す。
		//  センサー寸法は撮影中に画像から埋まることがある(機材マスターに無い機種)。
		//  ここで引き直さないと NPF と画角が "---" のまま戻らない(2026-08-19)。
		applyOwnedCameraSettings(g_plan.camera);
		char num[64];
		std::string j = "{\"name\":\"" + jesc(g_plan.name) + "\"";
		// 撮影周期は小数第1位まで扱う(長秒ss時に ss+2.0/+2.5/+3.0 のような細かい設定を可能にするため)。
		// int へ切り捨てると 15.5 が 15 に化けて UI と実体がズレるので小数で出す。
		std::snprintf(num, sizeof(num), "%.1f", g_plan.interval);
		j += ",\"interval\":" + std::string(num);
		j += ",\"start\":\"" + dtToStr(g_plan.start) + "\"";
		j += ",\"end\":\""   + dtToStr(g_plan.end)   + "\"";
		// 表示用の静的フィールド(場所/機材/方向/仰角/向き)
		j += ",\"place\":\"" + jesc(g_plan.place.name) + "\"";
		std::snprintf(num, sizeof(num), "%.4f,%.4f", g_plan.place.latitude, g_plan.place.longitude);
		j += ",\"latlng\":\"" + std::string(num) + "\"";
		j += ",\"altitude\":" + std::to_string(static_cast<int>(g_plan.place.altitude));
		// 項目C: 計画のカメラ表示はアプリ登録の「名称」(name)に統一する(選択肢と同じ)。
		//  従来は maker+model("Canon EOS R10")=愛称相当を出していた。name が空なら model へフォールバック。
		j += ",\"camera\":\"" + jesc(g_plan.camera.name.empty() ? g_plan.camera.model : g_plan.camera.name) + "\"";
		// 同じ機種を複数台持つと名称だけでは分からないので、カメラ本体で付けた名前も添える。
		j += ",\"cameraAssignedName\":\"" + jesc(g_plan.camera.assignedName) + "\"";
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
		// 【センサー未登録なら算出不可(2026-08-19)】NPFはセンサー横寸法と画素数が要る。
		//  マスタに無い機種はどちらも空なので、0で計算した数字を出すと嘘になる。
		//  -1 を「出せない」の合図にして、画面は "---" と出す。
		double npf = (g_plan.camera.sensorSize > 0.0 && g_plan.camera.sensorPixel > 0)
		             ? expo::npfShutterSec(g_plan.camera.sensorSize, static_cast<double>(g_plan.camera.sensorPixel),
		                                   g_plan.lens.focalLength, g_plan.lens.fn)
		             : -1.0;
		std::snprintf(num, sizeof(num), "%.2f", npf);
		j += ",\"npf\":" + std::string(num);
		j += ",\"minInterval\":" + std::to_string(minIntervalSec(g_plan));
		j += ",\"useSunrise\":" + std::string(g_plan.ccm.useSunrise ? "true" : "false");
		j += ",\"useSunset\":"  + std::string(g_plan.ccm.useSunset  ? "true" : "false");
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
			      "\",\"assignedName\":\"" + jesc(d.assignedName) +
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

	// 前方宣言(定義は下の「機材へのクランプ」節)。
	void seedPlanCcmFromDefaults(hgc::cs& plan);
	void clampOwnedToGear(hgc::ccmOwned& own, const hgc::camera& cam, const hgc::lens& lens);
	std::string ownedCcmToJson(const hgc::ccmOwned& own);
	void applyCcmSetToPlan(hgc::cs& plan, const astro::ccmSet& set);

	// 現在(編集対象)の計画を保存用 JSON にする。
	// 撮影制御方法は計画自身(g_plan.ccm)が持つので、以前のような別枠 planCcm は書かない。
	// 別枠にしていたころは、受信した計画に「そのときメモリにあった別計画の ccm」が貼り付いて
	// 保存される事故が起きた(2026-08-08 実害)。貼り付ける口ごと無くす。
	std::string wrapCurrentPlan(void)
	{
		return csjson::toJson(g_plan);
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
		std::string saved;
		if (!dataManager::loadPlanFile(id, saved) ||
		    !csjson::fromJson(saved, g_plan)) { return ERR_HGC_NO_ELEMENT; }
		// 保存済みの機材(カメラ/レンズ)を尊重する(計画ごとの機材束縛/定数編集の永続化)。
		// 機種が空(壊れた保存)のときだけ出荷時で補う。
		if (g_plan.camera.model.empty())
		{
			hgc::cs fp; dataManager::factoryFixedPlan(fp);
			g_plan.camera = fp.camera;
			g_plan.lens   = fp.lens;
		}
		// 撮影制御方法は計画のJSONに入っている。欠けている型があるときだけ初期値で補う
		// (保存が壊れていた場合の保険。正常な計画では取り込み直さない)。
		if (!g_plan.ccm.complete()) { seedPlanCcmFromDefaults(g_plan); }
		const errCode be = astro::buildSchedule(g_plan, g_offMin);
		if (be != ERR_HGC_OK) { return be; }
		buildScheduleJson();
		g_editId = id;
		g_planReady = true;
		return ERR_HGC_OK;
	}

	// 出荷時の固定計画を新規生成して現在(編集対象)に据える(開始=現在-60秒、終了=2時間後)。
	// 撮影制御方法は「優先的な初期値にする」で指定されたプリセットから採る(項目14)。指定が無い型は
	// 内蔵初期値(factoryCcmSet)で補われる(dataManager::preferredCcmSetJson がフォールバック済み)。
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
		seedPlanCcmFromDefaults(g_plan);	// 新規作成: ここでだけ4種すべてを初期値から取り込む
		astro::buildSchedule(g_plan, g_offMin);
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
		// 接続時にシリアル/設定名を所持カメラへ自動保存(無ければ自動作成。§5.2拡張)。
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
	// 計画が所有する ccm 一式をカメラ/レンズの上下限へクランプする(item3)。
	void clampOwnedToGear(hgc::ccmOwned& own, const hgc::camera& cam, const hgc::lens& lens)
	{
		auto one = [&](hgc::ccmBase* c)
		{
			if (!c) { return; }
			clampExposureToGear(c->limitBright, cam, lens);
			clampExposureToGear(c->limitDark,   cam, lens);
			clampExposureToGear(c->initial,     cam, lens);
		};
		one(own.night.get()); one(own.sunrise.get()); one(own.sunset.get()); one(own.day.get());
	}

	// ccmSet(初期値/プリセット由来) を計画の所有一式へ入れる。used は変えない
	// (「使わない」に戻した型をもう一度使うとき、以前の編集内容を初期値で潰さないため)。
	void applyCcmSetToPlan(hgc::cs& plan, const astro::ccmSet& set)
	{
		if (set.night)   { plan.ccm.night   = set.night; }
		if (set.sunrise) { plan.ccm.sunrise = set.sunrise; }
		if (set.sunset)  { plan.ccm.sunset  = set.sunset; }
		if (set.day)     { plan.ccm.day     = set.day; }
	}

	// 計画の所有一式を JSON にする(UIへの受け渡し用)。使う/使わないも一緒に返す。
	std::string ownedCcmToJson(const hgc::ccmOwned& own)
	{
		std::string j = "{\"version\":1";
		auto put = [&](const char* k, const hgc::ccmBase* c)
		{ if (c) { j += ",\"" + std::string(k) + "\":" + csjson::ccmToJson(*c); } };
		put("night",   own.night.get());
		put("sunrise", own.sunrise.get());
		put("sunset",  own.sunset.get());
		put("day",     own.day.get());
		j += std::string(",\"useNight\":")   + (own.useNight   ? "true" : "false");
		j += std::string(",\"useSunrise\":") + (own.useSunrise ? "true" : "false");
		j += std::string(",\"useSunset\":")  + (own.useSunset  ? "true" : "false");
		j += std::string(",\"useDay\":")     + (own.useDay     ? "true" : "false");
		return j + "}";
	}

	// **初期値からの取り込みはここだけ**(新規作成 / 保存が壊れていた場合の補完)。
	// 優先プリセット(「優先的な初期値にする」で選ばれたもの)を使い、壊れていれば内蔵初期値へ落とす。
	// 既に実体がある型は触らない = ユーザーの編集内容を初期値で潰さない。
	void seedPlanCcmFromDefaults(hgc::cs& plan)
	{
		astro::ccmSet set;
		if (!dataManager::parseCcmSetJson(dataManager::preferredCcmSetJson(), set))
		{ dataManager::parseCcmSetJson(dataManager::ccmDefaultsJson(), set); }
		if (!plan.ccm.night)   { plan.ccm.night   = set.night; }
		if (!plan.ccm.sunrise) { plan.ccm.sunrise = set.sunrise; }
		if (!plan.ccm.sunset)  { plan.ccm.sunset  = set.sunset; }
		if (!plan.ccm.day)     { plan.ccm.day     = set.day; }
		clampOwnedToGear(plan.ccm, plan.camera, plan.lens);
	}

	// 編集中計画のccm一式を、その計画のカメラ/レンズの上下限へクランプする(item3)。
	// 測光方式は「その機体の性質」であって計画ごとに変える設定ではない。計画は自分のカメラの
	//  コピーを持つので、所持カメラ側でチェックを変えても既存の計画には届かない。使う直前に
	//  所持カメラの値へ揃える。スマホ直結の撮影と、エッジへ送る計画JSONの両方で通す。
	//
	// 【エッジでは絶対にやらない(2026-08-14 の回帰)】当初「エッジは所持カメラを持たないので
	//  無害」と書いたが**誤り**。エッジも接続したカメラを所持台帳へ記録する
	//  (recordConnectedCamera)。その控えの meterLv は既定 false なので、ここを無条件に走らせると
	//  **スマホが計画へ載せて送った true を false へ塗り潰す**。実際 R10 がライブビュー主体に
	//  ならず毎コマサムネイルを取りに行き、178コマで Device busy に陥った(R10 の取得回数上限)。
	//  設定の出所はスマホだけ。受け取った側は計画を信用する。
	//  測光方式と同じ理由で、ダイジェスト認証のユーザーID/パスワードもここで揃える。
	//  どちらも「その機体の性質」で、計画ごとに変える設定ではない。
	void applyOwnedCameraSettings(hgc::camera& cam)
	{
		if (!hge::role::ownedCamerasAuthoritative()) { return; }
		hgc::camera oc;
		const std::string key = cam.name.empty() ? cam.model : cam.name;
		if (!key.empty() && dataManager::findOwnedCamera(key, oc))
		{
			cam.meterLv  = oc.meterLv;
			cam.authUser = oc.authUser;
			cam.authPass = oc.authPass;
			// 【個体の取り違え対策(2026-08-19)】計画はカメラを「控え」で持つ。所持カメラへ
			//  あとからシリアルが入っても、その前に作った計画の控えは空のままになる。
			//  空のまま撮影を始めると個体を指定できず、同機種の空き1台で代替されるため、
			//  2台目の計画のつもりが1台目を掴む(実機で発生: EOS R50 V 2台がどちらも同じ個体)。
			//  名称は所持カメラを指す鍵なので、そこから今のシリアル/設定名を引き直す。
			//  既に入っている値は上書きしない(ユーザーが選んだ個体を勝手に変えない)。
			if (cam.serial.empty()       && !oc.serial.empty())       { cam.serial       = oc.serial; }
			if (cam.assignedName.empty() && !oc.assignedName.empty()) { cam.assignedName = oc.assignedName; }
			// センサー寸法/画素数も同じ扱い。撮影画像から後で埋まることがあるので
			//  (機材マスターに無い機種)、控えが空なら所持カメラの今の値を採る。
			//  こうしないと NPF と撮影シミュレーションが "---" のまま戻らない。
			if (cam.sensorSize  <= 0.0 && oc.sensorSize  > 0.0) { cam.sensorSize  = oc.sensorSize;  }
			if (cam.sensorSizeV <= 0.0 && oc.sensorSizeV > 0.0) { cam.sensorSizeV = oc.sensorSizeV; }
			if (cam.sensorPixel == 0   && oc.sensorPixel > 0)   { cam.sensorPixel = oc.sensorPixel; }
		}
	}

	void clampPlanCcmToGear(void) { clampOwnedToGear(g_plan.ccm, g_plan.camera, g_plan.lens); }

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
			std::string saved; hgc::cs cs;
			if (dataManager::loadPlanFile(id, saved) && csjson::fromJson(saved, cs)) { names.push_back(cs.name); }
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
		// §7.4 予約(将来開始)の撮影要求は、撮影窓の PRE_MARGIN_SEC 秒前までカメラを確保しない。
		//   これにより「重ならなければ何件でも予約」しても、同時にカメラを掴むのは重なり制限どおり最大2台に収まる。
		//   待機中は WAITING(待機表示)。停止要求(cancel)で即座に中断する。
		{
			long long armAt = hgc::toUnixUtc(S->plan.start, g_offMin) - PRE_MARGIN_SEC;
			if ((long long)std::time(nullptr) < armAt)
			{
				S->state = HGE_ST_WAITING; notifyStateP(S->planId, HGE_ST_WAITING); refreshAggregateState();
				while (!S->cancel.load() && (long long)std::time(nullptr) < armAt) { tool::sleep(500); }
				if (S->cancel.load()) { return ERR_HGC_OK; }	// 予約待ち中に停止された
			}
		}
		// カメラ確保フェーズへ。ずらしスロットは「今まさに確保中(armed)」の他セッションを避けて割り当てる。
		S->armed = true;
		{
			bool used[MAX_CONCURRENT] = { false };
			for (auto& s : g_sessions) { if (s.get() != S && s->armed.load() && s->slot >= 0 && (size_t)s->slot < MAX_CONCURRENT) { used[s->slot] = true; } }
			int slot = 0;
			while (slot < (int)MAX_CONCURRENT && used[slot]) { ++slot; }
			S->slot = (slot < (int)MAX_CONCURRENT) ? slot : 0;
			S->runner->setStagger((double)S->slot / (double)MAX_CONCURRENT);	// Phase4: 周期内スロット位置でずらす
		}
		S->state = HGE_ST_SEARCHING; notifyStateP(S->planId, HGE_ST_SEARCHING); refreshAggregateState();
		// 撮影開始操作をした時点(=実際の撮影開始時刻より前。例: 1時間後開始の計画でもタップ時)で
		// カメラ検索が走ることを記録する。後続の「カメラ接続 …」(成功経路) または ERR「…見つかりません」と
		// 対になる。タップ時刻はこのログのタイムスタンプ、撮影窓は detail で分かる。
		{
			std::string d = "撮影開始操作 計画=" + S->plan.name +
			                " 窓=" + dtToStr(S->plan.start) + "~" + dtToStr(S->plan.end) +
			                " カメラ探索開始(IP直結→SSDP)";
			dataManager::logEvent("NET", d.c_str());
		}
		// 撮影開始(=このシーケンスの実行)の都度、計画のカメラを特定する(§3.3)。
		//  発見順は「IP直結+本人確認 → SSDP M-SEARCH」。既知IP(スマホプッシュ/前回成功)へ直結できれば
		//  低速なSSDPを省き発見を高速化する。IPはオートパワーオフ→DHCPで変わり得るが、接続後に (model,serial)
		//  で本人確認するので誤接続はしない。
		//  ・撮影開始"時刻"ではなく「撮影開始操作をした時点」で探す(本関数はワーカーで即実行され、
		//    実際の露光開始までは runner が開始時刻まで待機する)。
		//  ・複数同時撮影でも、これから撮るカメラは動作中のものとは必ず別の1台。各セッションは自分専用の
		//    device(S->dev: アドレス安定)を持つので、探索しても実行中の他セッションを壊さない。
		applyOwnedCameraSettings(S->plan.camera);	// 探索の前に個体(シリアル)を引き直す
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

		// §4b: 計画のカメラを特定する。同機種(例 EOS R10)が複数あっても、シリアルNo か ユーザーが付けた名前(assignedName) で1台を選ぶ。
		//  ・serial 指定           → そのシリアルの個体。
		//  ・assignedName 指定         → 所持リストからシリアルを解決して個体特定(未接続でserial不明なら下のモデル照合へ)。
		//  ・どちらも無い("それ以外のEOS-R10") → 同機種で、既に識別済み(所持リストにシリアル登録済み)の個体"以外"の1台。
		std::string wantSerial = pc.serial;
		if (wantSerial.empty() && !pc.assignedName.empty())
		{
			std::string s; if (dataManager::serialForAssignedName(pc.assignedName, s)) { wantSerial = s; }
		}
		const bool hasModel = !pc.model.empty() || !pc.name.empty();

		std::vector<class device> found;

		// §3.3.1 IP直結を最優先(役割別=エッジ役の実装 30_role/20_edge)。既知IP(スマホプッシュ)へ直結し
		//   (model, serial) 本人確認まで済ませて1台採れたらSSDPを省略する。スマホ役(10_phone)は常に false を返す
		//   ため、以下は必ずSSDP発見に進む(スマホ自身の発見は信頼できる)。
		bool ipDirect = false;
		{
			class device dv;
			if (hge::role::tryIpDirect(wantSerial, pc, hasModel, serialBusy, dv))
			{
				found.push_back(dv);
				ipDirect = true;
			}
		}

		// IP直結が不発なら SSDP M-SEARCH で探す(従来経路)。
		if (!ipDirect)
		{
			cameraController::detectTarget(found);
			{	// 診断: SSDP検索で何台見つかったか(0台=SSDP不発の可能性)。機種/シリアルも残す。
				std::string d = "SSDP検索結果 " + std::to_string(found.size()) + "台";
				for (auto& fd : found) { if (fd.apiBase) { d += " [" + (fd.model.empty() ? fd.assignedName : fd.model) + "/" + (fd.serialno.empty() ? "?" : fd.serialno) + "]"; } }
				dataManager::logEvent("NET", d.c_str());
			}
		}

		class device* hit = nullptr;
		if (!wantSerial.empty())
		{	// 特定個体(シリアル)を探す。見つからなくても中断せず武装する(3a: 未検出許容)。
			// 本人確認: シリアルは機種内一意が限界なので (model, serial) 両方一致で採用(別モデル同一シリアル衝突を防ぐ)。
			for (auto& d : found) { if (d.apiBase && d.serialno == wantSerial && dataManager::cameraModelMatches(d, pc)) { hit = &d; break; } }
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
			// 該当が無ければ同機種の空き1台で代替。ただし**同機種を2台以上登録している**ときはしない。
			//  どちらのつもりか決められないのに1台目を掴んでしまい、2台目の計画のはずが
			//  1台目を操作する(2026-08-19 実機で発生)。個体が決められないなら未検出として扱い、
			//  探索を続ける方が正しい(誤ったカメラを動かさない)。
			if (hit == nullptr)
			{
				if (dataManager::ownedCountForModel(pc) >= 2)
				{
					dataManager::logEvent("NET", (pc.model + " は同機種を複数登録しているため、"
					                              "個体(シリアル)が決まらない計画では代替しない").c_str(), true);
				}
				else
				{ for (auto& d : found) { if (d.apiBase && dataManager::cameraModelMatches(d, pc) && !serialBusy(d.serialno)) { hit = &d; break; } } }
			}
		}
		else
		{	// 計画にカメラ未指定: 他セッションが使っていない最初の1台。見つからなくても武装(3a)。
			for (auto& d : found) { if (d.apiBase && !serialBusy(d.serialno)) { hit = &d; break; } }
		}

		// §3.3 tier3: IP直結もSSDPも不発なら、限定サブネットの :8080 バッチ探索で最終確認(STA・スマホ不在の保険)。
		//   エッジ役のみ実体を持つ(スマホ役は false)。本人確認済みの1台を found へ載せて hit にする。
		if (hit == nullptr && !ipDirect)
		{
			found.emplace_back();
			if (hge::role::trySubnetSweep(wantSerial, pc, hasModel, serialBusy, found.back())) { hit = &found.back(); }
			else { found.pop_back(); }
		}

		if (hit != nullptr)
		{
			S->dev = *hit;	// セッション専用 device へコピー(アドレス安定。実行中の他セッションに影響しない)
			{	// 接続経路(常にM-SEARCH発見)と IP/serial をログに残す。
				std::string h = hostFromDevice(S->dev);
				std::string d = std::string("カメラ接続 SSDP発見 ip=") + (h.empty() ? "?" : h)
				              + (S->dev.serialno.empty() ? "" : (" serial=" + S->dev.serialno));
				dataManager::logEvent("NET", d.c_str());
			}
			// 表示用デバイス一覧(g_devices)も最新へ反映(同serialは更新、無ければ追加)。runner は g_devices を参照しない。
			{
				int gi = -1;
				for (size_t i = 0; i < g_devices.size(); ++i) { if (g_devices[i].serialno == S->dev.serialno) { gi = (int)i; break; } }
				if (gi >= 0) { g_devices[gi] = *hit; } else { g_devices.push_back(*hit); }
			}
			notify(HGE_EV_DEVICE, devicesJson());
			dataManager::recordConnectedCamera(S->dev);
			// エッジ役: 接続成功IP(serial→ip)を不揮発へ記録。無人再起動後の「前回IP直結」に使う(スマホ役は no-op)。
			hge::role::noteConnected(S->dev.serialno, S->dev.model, hostFromDevice(S->dev));
		}
		else
		{	// 3a: カメラ未検出のまま撮影要求を受理する。中断せず、runner が取得まで NOCAMERA(✖点灯)で
			// M-SEARCH(再送)で探し続ける。撮影窓の終了か中止まで継続。
			S->dev.clear();
			std::string d = "カメラ未検出のまま撮影要求を受理(探索を継続) SSDP=" + std::to_string(found.size()) + "台(未発見)";
			dataManager::logEvent("NET", d.c_str(), true);
		}

		S->runner->setNoticeCallback([S](int code, long long n1) { notifyNotice(S->planId, code, n1); });
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
					// 撮影結果レポートをファイルへ出す(UIには出さない)。1コマも撮っていなければ出さない。
					if (S->report.frames > 0)
					{
						std::string rp = dataManager::writeCaptureReport(S->report, S->plan, S->planId.c_str());
						if (!rp.empty()) { dataManager::logEvent("INFO", ("report: " + rp).c_str()); }
						S->report = dataManager::captureReport{};	// 次の撮影に持ち越さない
					}
				}
				S->state = s; notifyStateP(S->planId, s); refreshAggregateState();
				// 実行状況: 撮影窓を終えてのIDLE(=完了)のみ意図を消す。失敗(ERROR/接続断)や
				// 窓内での停止/シャットダウンでは残し、再起動時に再開できるようにする。
				if (s == HGE_ST_IDLE &&
				    static_cast<long long>(std::time(nullptr)) >= hgc::toUnixUtc(S->plan.end, g_offMin))
				{ setCapturing(S->planId, false); }
			},
			[S](const captureRunner::progressInfo& p) {
				S->pgFrame = p.frame; S->pgTotal = p.total; S->pgRemain = p.remainSec; S->pgElapsed = p.elapsedSec;	// 計画別(C_PROGRESS応答)
				g_pgFrame = p.frame; g_pgTotal = p.total; g_pgRemain = p.remainSec; g_pgElapsed = p.elapsedSec;
				char b[160];
				std::snprintf(b, sizeof(b), "{\"planId\":\"%s\",\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d}",
				              S->planId.c_str(), p.frame, p.total, p.remainSec, p.elapsedSec);
				notify(HGE_EV_PROGRESS, b);
			},
			[S](const captureRunner::capturedInfo& c) {
				S->pgExp = c.exp; S->pgCcm = c.ccm;	// 計画別(C_PROGRESS応答)
				g_pgExp = c.exp; g_pgCcm = c.ccm;
				char b[256];
				std::snprintf(b, sizeof(b), "{\"planId\":\"%s\",\"frame\":%d,\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\",\"luminance\":%.3f}",
				              S->planId.c_str(), c.frame, c.exp.iso.c_str(), c.exp.ss.c_str(), c.exp.fn.c_str(), c.luminance);
				notify(HGE_EV_CAPTURED, b);
				if (c.ccm != S->lastCcm) { std::string d = (S->lastCcm.empty() ? "" : S->lastCcm + " -> ") + c.ccm; dataManager::logEvent("CCMSW", d.c_str()); S->lastCcm = c.ccm; }
				dataManager::logShot(c.frame, c.exp, c.luminance, c.ccm.c_str(), c.metered, c.rdyMeteringMs, c.rdyShutterMs, c.prepMs, c.lateMs, c.rdyOk, c.setOk, c.meterTry, c.applyTry, c.histSum, c.lvTimeMs, c.staleSkip, c.shutterMs, c.busyMs, c.firstApplyTries);
				// 【診断トラップ(一時)】測光ヒストの明るい側をSHOTと別行で記録。夜明けにライブビューが
				// 明るい画素を捉えているか(p99/最大ビン)を後で実写と突き合わせるため。測光時のみ・タイミング不変。
				if (c.metered >= 0.0 && c.lvP99 >= 0.0)
				{
					// mss=測光に使ったシャッター(-=撮影露出のまま=従来動作) / stl=Tv変更がLVへ反映されるまでの待ち[ms]
					// pin=1: 測光ssを変えても値が動かない=ライブビュー張り付き(測光値は信用できない)
					// late=このコマのシャッターが撮影周期からどれだけ遅れたか[ms](0=周期ぴったり)
					// prep=準備(測光→計算→設定)の合計[ms]。リード kPrepLeadMs=5000 に収まったかの判定。
					// wait/fetch/dec = 測光所要の内訳(登録通知待ち/サムネ取得/復号)。ftry=取得の試行回数。
					// ai=切替なしで測ったリニア値。測光ssへ切替えた(mss≠-)コマでは、この値が
					// 使える範囲を外れたことが切替の理由。2.6秒の反映待ちを毎コマ payする
					// 原因がここでしか分からない(2026-08-05 追加)。-1=切替なし経路を通らず。
					// use=0: 測れたが中央値が使える帯の外で、露出制御には使わず据え置いた
					//        (2026-08-07。黒つぶれの測光値で撮影露出を絞り切った事故の再発防止)。
					//        Y= は据え置いたコマでも残すので「何が見えていたか」は追える。
					// mean/p75/p90/sat: 測光統計量の比較用(2026-08-08 診断。制御には使わない)。
					//        夜明けに露出の追従が約1時間遅れた。中央値が実写より最大4.35段暗く出るのが
					//        原因だが、どの統計量なら何時に反応したかを後から机上で比べられるよう残す。
					//        mean=リニア平均 / p75,p90=パーセンタイル(sRGB位置) / sat=最上位ビンの画素割合。
					char d[320];
					std::snprintf(d, sizeof(d),
						"fr=%d Y=%.4f p99=%.3f pMx=%.3f mss=%s stl=%dms pin=%d use=%d late=%dms prep=%dms"
						" wait=%dms fetch=%dms dec=%dms ftry=%d ai=%.5f"
						" mean=%.5f p75=%.3f p90=%.3f sat=%.4f",
						c.frame, c.metered, c.lvP99, c.lvPMax,
						c.meterSs.empty() ? "-" : c.meterSs.c_str(), c.meterSettleMs, c.lvPinned ? 1 : 0,
						c.meterUsable ? 1 : 0,
						c.lateMs, c.prepMs,
						c.meterWaitMs, c.meterFetchMs, c.meterDecodeMs, c.meterFetchTries, c.asIsLinear,
						c.lvMeanLin, c.lvP75, c.lvP90, c.lvSat);
					dataManager::logEvent("LVHIST", d);
				}
				// 撮影結果レポートの積算(1コマ=1回)。撮影終了時にまとめてファイルへ出す。
				{
					dataManager::captureReport& R = S->report;
					++R.frames;
					// 測光は自動露出区間だけ試みる(夜間の固定露出は測光しない=meterTry==0)。
					// 分母を「測光を試みたコマ」にしないと、夜間が混じって測光失敗率が過大に見える。
					if (c.meterTry >= 1)
					{
						++R.meterTried;
						if (c.metered < 0.0) { ++R.meterFail; }	// 試みたが取得できなかった(露出据え置き)
					}
					if (!c.setOk)        { ++R.setFail; }		// リトライしても露出設定できず
					// 測光の「やり直し」= サムネイルを取り直した回数。meterTry は登録通知を
					// 引いた回数(1コマ6〜8回が普通)なので、これを使うと全コマが「リトライ」に
					// なってしまい意味を失う(2026-08-13 実測で判明)。
					if (c.meterFetchTries > 1) { ++R.meterRetryFrames; }
					// 何で測ったかの内訳。カメラ依存層の申告(apiBase::via_*)をそのまま数えるだけで、
					//  この値で動作は変えない(測光方式は下の層に隠したままにする)。
					if      (c.meterVia == 2) { ++R.thumbFrames; }
					else if (c.meterVia == 1) { ++R.lvFrames; }
					else if (c.meterVia == 3) { ++R.heldFrames; }
					if (c.applyTry > 1)  { ++R.applyRetryFrames; }
					if (c.staleSkip > 0) { ++R.staleFrames; R.staleTotal += c.staleSkip; }
					if (c.lateMs >= 0)
					{
						++R.lateCnt; R.lateSum += c.lateMs;
						if (c.lateMs <= captureRunner::kLateOkMs) { ++R.lateOk; }
						else { ++R.lateOverCnt; R.lateOverSumMs += c.lateMs; }	// 間に合わなかったコマだけを別に数える
						if (c.lateMs > R.lateMax) { R.lateMax = c.lateMs; }
					}
					if (c.prepMs >= 0)
					{
						R.prepSum += c.prepMs;
						if (c.prepMs > R.prepMax) { R.prepMax = c.prepMs; }
						// 準備に使えた時間(リード)は測光方式で変わる。サムネイル測光では
						// 「周期 - (露光 + 余裕)」なので、固定値ではなくそのコマの実値と比べる。
						if (c.leadMs > 0 && c.prepMs > c.leadMs) { ++R.prepOver; }
					}
					if (c.shutterMs > 0)
					{
						if (R.firstShutterMs == 0) { R.firstShutterMs = c.shutterMs; }
						R.lastShutterMs = c.shutterMs;
					}
					// busy(露光終了→測光可=新しい画像の登録通知が来るまで)。>=0 が実測。
					if (c.busyMs >= 0)
					{
						++R.busyCnt; R.busySumMs += c.busyMs;
						if (c.busyMs > R.busyMaxMs) { R.busyMaxMs = c.busyMs; }
					}
					// 準備の内訳。測光と露出設定を分けて持つ(どちらがリードを食っているかを見るため)。
					if (c.rdyMeteringMs >= 0)
					{
						++R.meterCnt; R.meterSumMs += c.rdyMeteringMs;
						if (c.rdyMeteringMs > R.meterMaxMs) { R.meterMaxMs = c.rdyMeteringMs; }
					}
					if (c.rdyShutterMs >= 0)
					{
						++R.applyCnt; R.applySumMs += c.rdyShutterMs;
						if (c.rdyShutterMs > R.applyMaxMs) { R.applyMaxMs = c.rdyShutterMs; }
					}
					// 最長ss。最短周期の目安(ss + busy + 準備 + 余裕)の第1項になる。
					{
						const double ss = expo::parseValue(c.exp.ss, expo::expoKind::ss);
						if (ss > R.maxSsSec) { R.maxSsSec = ss; }
					}
					if (c.leadMs > 0) { R.leadMs = c.leadMs; }
					// 初期収束の結果はセッション単位。毎コマ同じ値が来るのでそのまま写す。
					R.cvSteps = c.converge.steps; R.cvApplyNg = c.converge.applyNg;
					R.cvMeterNg = c.converge.meterNg; R.cvOutcome = c.converge.outcome;
					R.cvShots   = c.converge.shots;
				}
			},
			[S](errCode e, const std::string& m) {
				// レポート用: シャッター失敗を数える(runner が "actShutter" で通知する)。
				if (m.find("actShutter") != std::string::npos) { ++S->report.shootFail; }
				notifyError(e, m.c_str());
			});

		// カメラの取得/再接続を1回試みる(3a: 初回取得も撮影中再接続もこの1本に集約)。
		//  ・撮影要求時にカメラ未検出でも中断しないため、runner の取得フェーズがこれを繰り返し呼ぶ。
		//  ・撮影中の連続失敗時も同じ経路で再接続する。成功で *dev_(=S->dev)を最新IP/apiへ更新。
		S->runner->setReconnect([S]() -> bool {
			// 計画のカメラ(serial 明示 or assignedName から解決)を毎回求める。初回未取得時は S->dev.serialno が
			// 空のため、それに依存しない(旧実装の空serial依存バグを解消)。既知IP直結にもこの serial を使う。
			std::string wantSerial = S->plan.camera.serial;
			if (wantSerial.empty() && !S->plan.camera.assignedName.empty())
			{ std::string s; if (dataManager::serialForAssignedName(S->plan.camera.assignedName, s)) { wantSerial = s; } }
			if (wantSerial.empty() && !S->dev.serialno.empty()) { wantSerial = S->dev.serialno; }

			class device* hit = nullptr;
			std::vector<class device> known;	// 既知IP直結の結果(hit の生存管理)
			std::vector<class device> found;	// M-SEARCH結果
			// ①既知IP直結を最優先(スマホプッシュ/前回IP)。役割別(エッジ役 30_role/20_edge)が本人確認
			//   (model+serial)まで済ませて1台採れたら M-SEARCH を省く(速い・確実)。スマホ役は false→②へ。
			{
				const hgc::camera& rc = S->plan.camera;
				bool hasModel = !rc.model.empty() || !rc.name.empty();
				known.emplace_back();
				if (hge::role::tryIpDirect(wantSerial, rc, hasModel, [](const std::string&){ return false; }, known[0]))
				{ hit = &known[0]; }
				else { known.clear(); }
			}
			// ②外れたら M-SEARCH。判明シリアルで再特定(本人確認 model+serial)、無ければ機種一致。
			if (hit == nullptr)
			{
				cameraController::detectTarget(found);
				if (!wantSerial.empty())
				{
					for (auto& d : found) { if (d.apiBase && d.serialno == wantSerial && dataManager::cameraModelMatches(d, S->plan.camera)) { hit = &d; break; } }
				}
				if (hit == nullptr)
				{
					for (auto& d : found) { if (d.apiBase && dataManager::cameraModelMatches(d, S->plan.camera)) { hit = &d; break; } }
				}
			}
			// §3.3 tier3: SSDPでも未発見なら限定サブネットのバッチ探索(前回IP永続化+M-SEARCHの保険)。エッジ役のみ実体。
			if (hit == nullptr)
			{
				const hgc::camera& rc = S->plan.camera;
				bool hasModel = !rc.model.empty() || !rc.name.empty();
				found.emplace_back();
				if (hge::role::trySubnetSweep(wantSerial, rc, hasModel, [](const std::string&){ return false; }, found.back())) { hit = &found.back(); }
				else { found.pop_back(); }
			}
			// 【方針変更 2026-07-05】既知IP直結フォールバックは廃止(別カメラ誤接続の防止)。
			// SSDP(M-SEARCH再送)+サブネット探索で見つからなければ hit は null のまま → 後で再試行(その間 NOCAMERA)。
			if (hit == nullptr)
			{	// 再探索で未発見。runner の取得/再接続フェーズが後で再試行する。
				std::string d = "カメラ取得/再接続失敗 SSDP=" + std::to_string(found.size()) + "台(未発見)";
				dataManager::logEvent("NET", d.c_str(), true);
				return false;
			}
			S->dev = *hit;	// runner の dev_ が指す先(=S->dev)を最新のIP/apiへ更新。
			{
				std::string h = hostFromDevice(S->dev);
				std::string d = std::string("再接続成功 ip=") + (h.empty() ? "?" : h);
				dataManager::logEvent("NET", d.c_str());
				// エッジ役: 再接続で確定した最新IPを不揮発へ記録(次回起動の前回IP直結を最新化)。
				hge::role::noteConnected(S->dev.serialno, S->dev.model, h);
			}
			notify(HGE_EV_DEVICE, devicesJson());
			return true;
		});

		if (hit == nullptr)
		{	// 3a: 未検出のまま武装。即座に NOCAMERA(✖点灯)を反映(runner も取得まで NOCAMERA を出す)。
			S->state = HGE_ST_NOCAMERA; notifyStateP(S->planId, HGE_ST_NOCAMERA); refreshAggregateState();
		}
		hgc::exposureSmoothing smooth = dataManager::currentSmoothing();
		applyOwnedCameraSettings(S->plan.camera);	// スマホ直結でも所持カメラの測光方式で撮る
		errCode e = S->runner->ready(S->plan, &S->dev, smooth, g_offMin);
		if (e != ERR_HGC_OK) { notifyError(e, "ready"); S->state = HGE_ST_ERROR; notifyStateP(S->planId, HGE_ST_ERROR); refreshAggregateState(); return e; }
		// 撮影ループを本スレッド上で実行する(セッションあたり1スレッド)。runner 用の2本目の
		// タスクスタック(内部RAM14KB)の確保が断片化で慢性的に失敗する問題(2026-07-12 テスト4の
		// 15:00窓で実測: カメラ確保は毎回成功するが2本目のxTaskCreateだけ失敗し続ける)を、
		// 確保そのものを不要にして根治する。直列化(seqActive)はカメラ確保までを対象にし、
		// 撮影ループへ入る前に解除する(次のセッションの起動を塞がない)。
		S->seqActive = false;
		return S->runner->runInline();
	}

	// 起動シーケンスが走行中のセッションが有るか(同時1本に直列化するための判定)。
	bool anySeqActive(void)
	{
		for (auto& s : g_sessions) { if (s->seqActive.load()) { return true; } }
		return false;
	}

	// セッションの開始シーケンス(startSessionSequence)を実行するスレッドを生成する。
	// 失敗(=xTaskCreate 不発)なら false。呼び出し側(hge_captureStartPlan / hge_pump)が deferred で再試行する。
	bool launchStartThread(captureSession* raw)
	{
		if (raw->startThread) { ossc::threadEnd(raw->startThread); raw->startThread = nullptr; }	// 再デファー経路: 前回スレッドの残骸を回収(join+解放)
		raw->seqActive = true;
		ossc::THREAD_FUNC fn = [raw](void*) -> errCode {
			errCode e = startSessionSequence(raw);
			raw->seqActive = false;
			return e;
		};
		raw->startThread = ossc::threadNet(fn, nullptr);
		if (raw->startThread == nullptr) { raw->seqActive = false; return false; }
		return true;
	}

	// 1セッションを停止・破棄する(ランナーを join してから erase)。
	// 停止の記録(2026-08-07)。従来ここは何もログを残さず、STOP は撮影ループが正常終了した
	// ときだけ出ていた。そのため「外から止められた」痕跡がログに一切残らない。
	// 2026-08-07 02:00 の Edje00 で、朝R10 のセッション起動が2回走り(01:59:30 起点と
	// 02:00:08 起点)、2回目は ccm の中身が違っていた。開始経路には二重開始ガード
	// (hge_captureStartPlan の sessionFor)があるので1本目は消されたはずだが、消えた記録が
	// 無く原因を特定できなかった。次に起きたとき1回で分かるようにする。
	void stopSessionAt(size_t i, const char* why)
	{
		captureSession* s = g_sessions[i].get();
		{
			std::string d = "セッション停止 計画=" + s->plan.name + " id=" + s->planId
			              + " 理由=" + (why ? why : "?")
			              + " 状態=" + std::to_string(s->state.load());
			dataManager::logEvent("INFO", d.c_str());
		}
		s->cancel = true;	// 予約待ち中の開始スレッドを中断させてから join する(長時間 sleep のデッドロック回避)
		s->deferred = false;	// 遅延アーム待ちなら hge_pump の生成対象から外す(スレッド未生成のまま破棄)
		pokeUnregister(s->runner ? s->runner.get() : nullptr);	// 3b: ポーク対象から外す(runner破棄前)
		s->state = HGE_ST_STOPPING; notifyStateP(s->planId, HGE_ST_STOPPING); refreshAggregateState();
		// 順序が重要: 撮影ループは起動スレッド上で回る(runInline)ため、先に runner->stop() で
		// running_ を落としてから起動スレッドを join する(逆順だと窓終了まで join が返らない)。
		if (s->runner) { s->runner->stop(); }
		if (s->startThread) { ossc::threadEnd(s->startThread); s->startThread = nullptr; }	// ループ終了を join(以後コールバック無し)
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
				g_sessions[i]->cancel = true;	// 予約待ちスレッドが残っていても抜けられるように
				g_sessions[i]->deferred = false;	// 遅延アーム待ちも生成対象から外す
				pokeUnregister(g_sessions[i]->runner ? g_sessions[i]->runner.get() : nullptr);	// 3b
				if (g_sessions[i]->runner)      { g_sessions[i]->runner->stop(); }	// runInline: 先に停止要求→起動スレッド join の順
				if (g_sessions[i]->startThread) { ossc::threadEnd(g_sessions[i]->startThread); g_sessions[i]->startThread = nullptr; }
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
	hge::role::loadPersisted();	// 無人再起動後の「前回IP直結」用に不揮発の既知カメラを読み込む(エッジ役)
	// カメラを探し始める前に所持カメラを読んでおく。読み込みでダイジェスト認証の資格情報が
	//  候補に入る(エッジ役は所持を持たないが、撮影計画の受信/読み込みで同じ入口を通る)。
	dataManager::preloadOwned();
	g_inited = true;
	setState(HGE_ST_IDLE);
	return ERR_HGC_OK;
}

int32_t hge_term(void)
{
	while (!g_sessions.empty()) { stopSessionAt(g_sessions.size() - 1, "終了処理"); }	// 全セッション停止(最後の1つで待ち受けも停止)
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
	// 接続時にシリアル/設定名を所持カメラへ自動保存(無ければ自動作成。§5.2拡張)。
	dataManager::recordConnectedCamera(g_devices[0]);
	setState(HGE_ST_READY);
	return ERR_HGC_OK;
}

// スマホから発見中のオンラインカメラ一覧を受け取り、既知カメラテーブルを更新する(43 §6 C_CAMERA_INFO)。
// 実体は役割別(エッジ役=既知テーブル更新 / スマホ役=no-op)。common は窓口に委譲する。
int32_t hge_setKnownCameras(const char* json, int32_t len)
{
	return static_cast<int32_t>(hge::role::setKnownCameras(json, static_cast<int>(len)));
}

// スマホ常駐プレゼンスマップ(§3.2/§5.4)。マップ変化のたび HGE_EV_PRESENCE を通知する。
int32_t hge_presenceStart(void)
{
	hge::role::presenceStart([]() { notify(HGE_EV_PRESENCE, hge::role::presenceJson()); });
	return ERR_HGC_OK;
}

int32_t hge_presenceStop(void)
{
	hge::role::presenceStop();
	return ERR_HGC_OK;
}

int32_t hge_presenceJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	return copyOut(hge::role::presenceJson(), buf, inoutLen);
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
	// 受信の記録(2026-08-07)。従来ここは何もログを残さず、走行中の計画を黙って上書きできた。
	// 走行中セッションは自分のスナップショットで走り続けるので、ログに出ている計画内容と
	// 実際に効いている制御がずれる。2026-08-07 02:00 の Edje00 で、2回のSTARTのあいだに
	// ccm の中身が入れ替わっていた(表示名/暗所/初期値がすべて違う)。書き換えたのは
	// この関数しか無いので、いつ誰が上書きしたかを残す。
	{
		std::string existing;
		const bool  overwrite = dataManager::loadPlanFile(g_editId, existing);
		const bool  running   = (sessionFor(g_editId) != nullptr);
		std::string d = "計画受信 id=" + g_editId + " 名前=" + g_plan.name
		              + (overwrite ? " 上書き" : " 新規")
		              + (running   ? " ※走行中セッションあり(内容がずれる)" : "");
		dataManager::logEvent("INFO", d.c_str());
	}
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
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
	applyOwnedCameraSettings(g_plan.camera);
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

// 指定 id の撮影計画を JSON で取り出す。**編集対象(g_plan/g_editId)には触れない**。
//
// 【なぜ要るか】これまでは別の計画をエッジへ送るとき hge_selectPlan で編集対象を移し、
//  取り終わってから元へ戻していた。戻すのが非同期なので、その隙にユーザーが一覧で計画を
//  選ぶと戻し処理が後から走り、画面の詳細だけ前の計画に化けていた(一覧は新しい計画・
//  詳細は前の計画)。取り出しが編集対象を動かさなければ、戻す処理そのものが要らなくなる。
//
// 中身は loadPlanById と同じ整え方を、g_plan ではなく手元の cs に対して行う
//  (機材が空なら出荷時で補う → 撮影制御方法が欠けていたら初期値で補う → スケジュール再生成)。
//  そうしないと、これまで hge_selectPlan 経由で得ていた JSON と中身が変わってしまう。
int32_t hge_getPlanJsonById(const char* id, char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (id == nullptr || id[0] == '\0') { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady)
	{
		errCode e = loadFixedPlanImpl();	// g_offMin(タイムゾーン)を確保するため
		if (e != ERR_HGC_OK) { return e; }
	}
	// 編集対象そのものなら、既にメモリ上にある編集中の姿を返す(未保存の変更も含めるため)。
	if (std::string(id) == g_editId) { return hge_getPlanJson(buf, inoutLen); }

	std::string saved; hgc::cs cs;
	if (!dataManager::loadPlanFile(id, saved) ||
	    !csjson::fromJson(saved, cs)) { return ERR_HGC_NO_ELEMENT; }
	if (cs.camera.model.empty())
	{
		hgc::cs fp; dataManager::factoryFixedPlan(fp);
		cs.camera = fp.camera;
		cs.lens   = fp.lens;
	}
	if (!cs.ccm.complete()) { seedPlanCcmFromDefaults(cs); }
	const errCode be = astro::buildSchedule(cs, g_offMin);
	if (be != ERR_HGC_OK) { return be; }
	applyOwnedCameraSettings(cs.camera);

	std::string s = csjson::toJson(cs);
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
			std::string saved;
			got = dataManager::loadPlanFile(id, saved) && csjson::fromJson(saved, cs);
		}
		if (!got) { continue; }
		long long startU = hgc::toUnixUtc(cs.start, g_offMin);
		long long endU   = hgc::toUnixUtc(cs.end, g_offMin);
		bool capturable = endU > static_cast<long long>(now);
		captureSession* sess = sessionFor(id);
		int st = sess ? sess->state.load() : static_cast<int>(HGE_ST_IDLE);
		// カメラ予約表(項目17)と重複開始の抑止に使うため、計画のカメラも載せる。
		//  model/serial=同一機体の判定に使う / assignedName・name=表示用。
		const hgc::camera& pc = cs.camera;
		std::string o = "{\"id\":\"" + jesc(id) + "\",\"name\":\"" + jesc(cs.name) + "\"" +
		     ",\"start\":\"" + dtToStr(cs.start) + "\",\"end\":\"" + dtToStr(cs.end) + "\"" +
		     ",\"capturable\":" + std::string(capturable ? "true" : "false") +
		     ",\"camModel\":\"" + jesc(pc.model) + "\"" +
		     ",\"camName\":\"" + jesc(pc.name) + "\"" +
		     ",\"camAssignedName\":\"" + jesc(pc.assignedName) + "\"" +
		     ",\"camSerial\":\"" + jesc(pc.serial) + "\"" +
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
	{
		hgc::place ap;	// §7.9: 「撮影計画に自動的に挿入する」場所があれば初期値に入れ、位置に応じて再生成
		if (dataManager::autoInsertPlace(ap))
		{
			g_plan.place = ap;
			astro::buildSchedule(g_plan, g_offMin);
		}
	}
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
	// 項目9: 削除対象の計画が撮影中/待機中なら、まずセッションを停止してスレッドを破棄する。
	//  (従来は計画ファイルだけ消してセッションが走り続ける不具合があった。エッジ削除=
	//   edgeRemoveReceivedPlan 経由でも hge_deletePlan を通るので、エッジ側の撮影も停止する。)
	{
		const std::string pid(id);
		setCapturing(pid, false);	// 実行意図を消す(再起動で再開しない)
		for (size_t i = 0; i < g_sessions.size(); ++i)
		{
			if (g_sessions[i]->planId == pid) { stopSessionAt(i, "計画削除"); break; }	// cancel→runner->stop()→erase(スレッド破棄)
		}
	}
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

// 計画固有の撮影制御方法(night/sunrise/sunset/day)を JSON で取得(バッファ規約)。
int32_t hge_getPlanCcmJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { loadFixedPlanImpl(); }
	std::string s = ownedCcmToJson(g_plan.ccm);
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
	if (!dataManager::parseCcmSetJson(std::string(json, static_cast<size_t>(len)), set))
	{ return ERR_HGC_JSON_PARSE; }
	applyCcmSetToPlan(g_plan, set);
	clampPlanCcmToGear();	// item3: 計画のカメラ/レンズ上下限へccm露出をクランプ(初期値選択も含む)
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	// 仕様 7.4.2: ssが撮影周期-2を超えたら撮影周期を自動的に最長ss+2へ伸ばす。
	int mn = minIntervalSec(g_plan);
	if (g_plan.interval < static_cast<double>(mn)) { g_plan.interval = mn; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	// 他の setter と同様に即永続化する。これが無いと編集がメモリ上の g_plan/g_planCcm だけに残り、
	// 計画を選び直す(loadPlanById がファイルから読み直す)と黙って元へ戻る
	// (2026-07-17 実機: 日中撮影の露出補正を +2.0ev にしても保存されず、選び直すと +0.0ev に戻った)。
	return saveCurrentPlan();
}

int32_t hge_setPlanInterval(double seconds)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	int mn = minIntervalSec(g_plan);
	if (seconds < static_cast<double>(mn)) { return ERR_HGC_INVALID_ARG; }	// 最小未満は不可(UIで「先にssを変更」警告)
	g_plan.interval = seconds;
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	// 他の setter と同様に即永続化する。これが無いと g_plan(メモリ)だけが変わり、計画を
	// 選び直す(loadPlanById がファイルから読み直す)と編集が消えて元の周期で撮影してしまう
	// (2026-07-17: UIは15.5秒と表示するのに撮影は15.0秒で走る、として発覚)。
	return saveCurrentPlan();
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);	// 画角が変わる
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化
}

int32_t hge_setBandMode(int32_t sunriseMode, int32_t sunsetMode)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	// 0/1=使う, 2=使わない(旧 bandMode の値をそのまま受ける)。
	g_plan.ccm.useSunrise = (sunriseMode != 2);
	g_plan.ccm.useSunset  = (sunsetMode  != 2);
	g_plan.boundaries.clear();	// 帯の挿入/排除は構造が変わるため、古い境界上書きを破棄(整合性維持)
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化(他の setter と同様。選び直しで消えるのを防ぐ)
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化(設計方針「編集は各操作でその場保存」)
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
	// 【2026-08-17 修正】種別番号を直書きしていて **preNight/postNight がずれていた**。
	//  実際の ccmType は night=1 sunrise=2 sunset=3 day=4 **preNight=5 postNight=6** だが、
	//  ここは preNight=6 postNight=7 のつもりで書かれていた。そのため
	//   ・夕日↔夜間前移行、夜間前移行↔夜間 … どの分岐にも当たらず既定(-24〜+6)=事実上チェック無し
	//   ・朝日↔夜間後移行、夜間後移行↔夜間 … postNight が偶然 6 に当たって効いていた
	//  という非対称になっていた(夕方だけ効かない)。enum から取って二度とずれないようにする。
	{
		const int tNight = static_cast<int>(hgc::ccmType::night);
		const int tSunUp = static_cast<int>(hgc::ccmType::sunrise);
		const int tSunDn = static_cast<int>(hgc::ccmType::sunset);
		const int tDay   = static_cast<int>(hgc::ccmType::day);
		const int tPre   = static_cast<int>(hgc::ccmType::preNight);
		const int tPost  = static_cast<int>(hgc::ccmType::postNight);
		auto has = [&](int t) { return beforeType == t || afterType == t; };
		double lo = -24.0, hi = 6.0;
		bool sun = has(tSunUp) || has(tSunDn), trans = has(tPre) || has(tPost);
		if (has(tDay) && sun)          { lo = 3.0;   hi = 6.0;   }  // 日中↔夕日/朝日(上境界 +6〜+3)
		else if (sun && trans)         { lo = -1.0;  hi = 2.0;   }  // 夕日/朝日↔移行(下境界 +2〜-1)
		else if (has(tDay) && trans)   { lo = -3.0;  hi = 4.0;   }  // 日中↔移行(日中境界 +4〜-3)
		else if (has(tNight) && trans) { lo = -19.0; hi = -6.0;  }  // 夜間↔移行(夜間境界 -6〜-19。2026-08-17 に -12 から拡張)
		if (altDeg < lo) altDeg = lo;
		if (altDeg > hi) altDeg = hi;
	}
	// sunAltitudeTime は基準日の正午から前方探索する。朝(上昇)の交差は正午より前にあるため、
	// そのままだと翌日の上昇交差を拾って境界が翌日へ飛ぶ。基準を前日へ寄せて当日の朝を拾わせる。
	hgc::dateTime searchBase = base;
	if (rising != 0) { searchBase = localFromUnix(hgc::toUnixUtc(base, g_offMin) - 18 * 3600); }
	astro::altTime at = astro::sunAltitudeTime(g_plan.place, searchBase, altDeg, rising != 0, g_offMin);
	if (!at.valid) { return ERR_HGC_NOT_FOUND; }
	// 境目は「時刻」ではなく「太陽高度(移動範囲でクランプ済)」で保存する。撮影日時を変えても
	// その高度で現在の窓内に再適用でき、古い時刻の使い回しでスケジュールが壊れない(§7.3.2)。
	hgc::boundaryOverride bo;
	bo.before = static_cast<hgc::ccmType>(beforeType);
	bo.after  = static_cast<hgc::ccmType>(afterType);
	bo.occ    = static_cast<uint16_t>(occ < 0 ? 0 : occ);
	bo.when   = at.when;			// 表示/後方互換用
	bo.altDeg = altDeg;				// 適用時の主データ(移動範囲でクランプ済)
	bo.rising = (rising != 0);
	bool replaced = false;
	for (auto& b : g_plan.boundaries)
	{
		if (b.before == bo.before && b.after == bo.after && b.occ == bo.occ) { b = bo; replaced = true; break; }
	}
	if (!replaced) { g_plan.boundaries.push_back(bo); }
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化(設計方針「編集は各操作でその場保存」)
}

int32_t hge_clearScheduleEdits(void)
{
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	g_plan.ccm.useSunrise = true;
	g_plan.ccm.useSunset  = true;
	g_plan.boundaries.clear();
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化(設計方針「編集は各操作でその場保存」)
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

int32_t hge_ownedCameraAuthPassJson(const char* name, char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::ownedCameraAuthPass(name ? std::string(name) : std::string()), buf, inoutLen);
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
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
	errCode e = astro::buildSchedule(g_plan, g_offMin);
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();	// 編集を即永続化
}

// 撮影場所(緯度経度)を設定する。地図/テキスト貼り付けの両入力から呼ぶ。標高は変更しない(既存値を維持)。
// 位置が変わると日の出/日の入り/薄明時刻が変わるためスケジュールを再生成して通知する。
int32_t hge_setPlanLocation(double latitude, double longitude, const char* name)
{
	if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	g_plan.place.latitude  = latitude;
	g_plan.place.longitude = longitude;
	if (name != nullptr && name[0]) { g_plan.place.name = name; }
	errCode e = astro::buildSchedule(g_plan, g_offMin);	// 位置変化→太陽/月の時刻が変わる
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

// --- 撮影場所(§7.9)。登録した場所を撮影計画で選択する ---
int32_t hge_getPlacesJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::placesJson(), buf, inoutLen);
}

int32_t hge_addPlace(const char* name)
{
	return dataManager::addPlace(name ? std::string(name) : std::string()) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_removePlace(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::removePlace(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_setPlaceAutoInsert(const char* name, int32_t autoInsert)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setPlaceAutoInsert(std::string(name), autoInsert != 0) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
}

int32_t hge_setPlaceDetail(const char* origName, const char* json)
{
	if (origName == nullptr || json == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::setPlaceDetailJson(std::string(origName), std::string(json)) ? ERR_HGC_OK : ERR_HGC_JSON_PARSE;
}

// 登録済みの撮影場所を名称で選び、現在の撮影計画へ反映してスケジュールを再生成する。
int32_t hge_setPlanPlace(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (!g_planReady) { errCode e = loadFixedPlanImpl(); if (e != ERR_HGC_OK) { return e; } }
	hgc::place p;
	if (!dataManager::findPlace(std::string(name), p)) { return ERR_HGC_NO_ELEMENT; }
	g_plan.place = p;	// name/memo/緯度経度/標高/autoInsert をまるごと反映
	errCode e = astro::buildSchedule(g_plan, g_offMin);	// 位置変化→太陽/月の時刻が変わる
	if (e != ERR_HGC_OK) { return e; }
	buildScheduleJson();
	notify(HGE_EV_SCHEDULE, g_schedJson);
	return saveCurrentPlan();
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

int32_t hge_reportListJson(char* buf, int32_t* inoutLen)
{
	return copyOut(dataManager::reportListJson(), buf, inoutLen);
}

int32_t hge_reportCount(void)
{
	return static_cast<int32_t>(dataManager::reportNames().size());
}

int32_t hge_reportJson(const char* name, char* buf, int32_t* inoutLen)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return copyOut(dataManager::reportJson(std::string(name)), buf, inoutLen);
}

int32_t hge_removeReport(const char* name)
{
	if (name == nullptr) { return ERR_HGC_INVALID_ARG; }
	return dataManager::removeReport(std::string(name)) ? ERR_HGC_OK : ERR_HGC_NO_ELEMENT;
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
//  [{"model":..,"assignedName":..,"serial":..}, ...]。コンテキストメニューの「接続カメラ検索」用。
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
		s += "{\"model\":\"" + jesc(d.model) + "\",\"assignedName\":\"" + jesc(d.assignedName) +
		     "\",\"serial\":\"" + jesc(d.serialno) + "\",\"ip\":\"" + jesc(hostFromDevice(d)) + "\"}";
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

// 発見/接続したカメラ識別情報(model/serial/assignedName)を所持カメラへ反映する。
//  用途: ①エッジ→スマホ書き戻し(edgeの進捗JSONの serial/assignedName を受けて allowAdd=1)。
//        ②裏の発見(プレゼンス)で allowAdd=0 → 返り値 ISNEW のとき UI が「登録しますか？」を出す。
//  返り値: >=0 は dataManager::camApply(0=updated/1=filled/2=isNew)、<0 はエラー。
//  maker は model 先頭トークン(空白まで)から導出する(stripMaker 用。Canon運用では "Canon EOS R100"→"Canon")。
int32_t hge_recordCameraIdentity(const char* model, const char* serial, const char* assignedName, int32_t allowAdd)
{
	device d;
	d.model      = (model    != nullptr) ? model    : "";
	d.serialno   = (serial   != nullptr) ? serial   : "";
	d.assignedName = (assignedName != nullptr) ? assignedName : "";
	if (d.model.empty() && d.serialno.empty()) { return ERR_HGC_INVALID_ARG; }
	size_t sp = d.model.find(' ');
	d.manufacturer = (sp != std::string::npos) ? d.model.substr(0, sp) : std::string();

	// 【新規登録は実機に繋いでから(2026-08-19)】ここへ来る model/serial は在否監視(SSDP)由来で、
	//  カメラを叩かずに得た情報しか無い。マスタに無い機種は ISO/SS をカメラ本人から取りたいので、
	//  「新規として追加する」ときだけ実機を探し直し、APIを持った device で登録する。
	//  既存の更新(型番/愛称の反映)は通信不要なので、そのまま済ませて余計な探索をしない。
	//  撮影中(セッションあり)は探索しない。撮影主体以外がカメラへ触れないための既存の約束。
	if (allowAdd != 0 && g_sessions.empty())
	{
		const int32_t pre = dataManager::recordConnectedCameraStatus(d, false);
		if (pre != static_cast<int32_t>(dataManager::camApply::isNew)) { return pre; }
		std::vector<device> found;
		cameraController::detectTarget(found);
		for (auto& fd : found)
		{
			if (fd.apiBase && !fd.serialno.empty() && fd.serialno == d.serialno)
			{ return dataManager::recordConnectedCameraStatus(fd, true); }
		}
	}
	return dataManager::recordConnectedCameraStatus(d, allowAdd != 0);
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

// 計画id指定の進捗JSON(1エッジ複数カメラ対応)。planId 空/NULL は従来の集約スナップショットへフォールバック
// (復元時の「このエッジは何か撮っているか」問い合わせ・生存確認用)。planId 指定時は該当セッションの
// state/frame/total/… を返し、該当が無ければ state=IDLE(=この端末ではその計画は走っていない)を返す。
// これにより、同一エッジで2台撮影中に片方の一過性状態がもう片方の計画へ貼られる誤NOCAMERAポップを解消する。
int32_t hge_getProgressJsonFor(const char* planId, char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	if (planId == nullptr || planId[0] == '\0') { return hge_getProgressJson(buf, inoutLen); }

	int st = HGE_ST_IDLE; std::string nm, ccm; hgc::exposure exp{};
	int fr = 0, tot = 0, rem = 0, el = 0;
	std::string cSerial, cAssignedName, cModel;	// 接続確定カメラの識別情報(エッジ→スマホ書き戻し用)
	if (captureSession* S = sessionFor(planId))
	{
		st = S->state.load(); nm = S->plan.name;
		fr = S->pgFrame; tot = S->pgTotal; rem = S->pgRemain; el = S->pgElapsed;
		ccm = S->pgCcm; exp = S->pgExp;
		cSerial = S->dev.serialno; cAssignedName = S->dev.assignedName; cModel = S->dev.model;	// 未接続なら空
	}
	char tmp[512];
	std::snprintf(tmp, sizeof(tmp),
		"{\"state\":%d,\"name\":\"%s\",\"frame\":%d,\"total\":%d,\"remainSec\":%d,\"elapsedSec\":%d,"
		"\"ccm\":\"%s\",\"iso\":\"%s\",\"ss\":\"%s\",\"fn\":\"%s\","
		"\"serial\":\"%s\",\"assignedName\":\"%s\",\"model\":\"%s\"}",
		st, jesc(nm).c_str(), fr, tot, rem, el,
		jesc(ccm).c_str(), exp.iso.c_str(), exp.ss.c_str(), exp.fn.c_str(),
		jesc(cSerial).c_str(), jesc(cAssignedName).c_str(), jesc(cModel).c_str());
	int32_t need = static_cast<int32_t>(std::strlen(tmp)) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, tmp, need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

// 実行中セッションの一覧を JSON 配列で返す(バッファ規約)。[{"id":"<planId>","state":N},...]
// エッジ端末が C_SEARCH 応答(edgeInfo)へ載せ、スマホの常時スイープが「このエッジで何が走っているか」を
// 1リクエストで把握する。エッジ側ローカル開始/再起動後の自動再開の検出用。UDP 1データグラムに収める
// ため件数を制限する(同時撮影は2件・予約を含めても実用上は数件)。
int32_t hge_getSessionsJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	constexpr size_t kMaxSessions = 16;	// UDP応答(スマホ側受信バッファ2KB)に収める上限。1件~34B×16+edgeInfo≒800Bで収まる。
										// 上限を超えた分は応答に載らない(スマホは満杯時に除去同期を保留する)。
	std::string s = "[";
	size_t n = 0;
	for (auto& sess : g_sessions)
	{
		if (n >= kMaxSessions) { break; }
		if (n > 0) { s += ","; }
		s += "{\"id\":\"" + jesc(sess->planId) + "\",\"state\":" + std::to_string(sess->state.load()) + "}";
		++n;
	}
	s += "]";
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, s.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

// 項目6: エッジが保有する全撮影計画の id 一覧(=保有ロスター)を返す。スマホはこれを見て
//  「スマホ側でエッジ担当にしたのにエッジに無い計画」=エッジ側で削除された、と検知しロック解除する。
//  走行中に限らずファイルとして保持する全計画が対象(hge_getSessionsJson は走行中のみ)。
int32_t hge_getHeldPlansJson(char* buf, int32_t* inoutLen)
{
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	constexpr size_t kMaxIds = 32;	// UDP応答に収める上限。id~15B×32 で ~600B。エッジ保有数は通常数件。
	std::string s = "[";
	size_t n = 0;
	for (const std::string& id : dataManager::listPlanIds())
	{
		if (n >= kMaxIds) { break; }
		if (n > 0) { s += ","; }
		s += "\"" + jesc(id) + "\"";
		++n;
	}
	s += "]";
	int32_t need = static_cast<int32_t>(s.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, s.c_str(), need);
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
	// §7.4 受付上限: 自撮影の撮影開始要求(予約含む)は MAX_PENDING(=100)件まで。
	//  ユーザー通知は UI 側(戻り値→日本語トースト)で行うため、ここは記録のみ(EV_ERRORは出さない)。
	if (g_sessions.size() >= MAX_PENDING) { dataManager::logEvent("INFO", "capture request rejected: queue full (100)"); return ERR_HGC_QUEUE_FULL; }

	auto sess = std::make_unique<captureSession>();
	sess->planId = planId;
	sess->state  = HGE_ST_SEARCHING;	// 起動シーケンス実行前から非IDLEにし、reapDeadSessions に消されないようにする
	if (planId == g_editId)
	{
		sess->plan = g_plan;	// 編集中スナップショット(撮影制御方法も plan が持つ)
	}
	else
	{
		std::string saved; hgc::cs cs;
		if (!dataManager::loadPlanFile(planId, saved) || !csjson::fromJson(saved, cs)) { return ERR_HGC_NO_ELEMENT; }
		sess->plan = cs;
		// 撮影制御方法は計画が持っている。欠けていたときだけ初期値で補い、窓が無いときだけ組み立てる。
		if (!sess->plan.ccm.complete()) { seedPlanCcmFromDefaults(sess->plan); }
		if (sess->plan.ccmList.empty())
		{
			const errCode be = astro::buildSchedule(sess->plan, g_offMin);
			if (be != ERR_HGC_OK) { dataManager::logEvent("INFO", "buildSchedule failed at start"); return be; }
		}
	}
	// §7.4 重なり制限: 撮影期間[start-30s, end+1フレーム]が同時に重なる自撮影は2件まで。
	//  受付(=撮影開始要求)の時点でエラーにする。スマホ→エッジ投げ(hge_edgeStart)はこの経路を通らず対象外。
	{
		long long ns = hgc::toUnixUtc(sess->plan.start, g_offMin) - PRE_MARGIN_SEC;
		long long ne = hgc::toUnixUtc(sess->plan.end, g_offMin) + (long long)std::llround(sess->plan.interval);
		if (selfCaptureOverlapExceeds(ns, ne)) { dataManager::logEvent("INFO", "capture request rejected: overlap limit (2)"); return ERR_HGC_OVERLAP_LIMIT; }
	}
	sess->runner = std::make_unique<captureRunner>();
	captureSession* raw = sess.get();
	if (g_sessions.capacity() < MAX_PENDING) { g_sessions.reserve(MAX_PENDING); }	// push_back での再確保(走査中の他スレッドとの競合)を根絶
	g_sessions.push_back(std::move(sess));
	pokeRegister(raw->runner.get());	// 3b: SSDP出現ポークの対象に登録
	ensureWatching();					// 3b: 共有SSDP受動待ち受けを起動(冪等)
	// 遅延アーム: 窓が遠い予約は開始スレッドを作らず WAITING 表示のみ(スレッドは hge_pump が期日に生成)。
	// 期日が近い/過去なら即生成し、失敗(内部RAM断片化)しても deferred に落として hge_pump が再試行する。
	raw->armAtEpoch = hgc::toUnixUtc(raw->plan.start, g_offMin) - PRE_MARGIN_SEC;
	long long now = static_cast<long long>(std::time(nullptr));
	// #1: 撮影開始要求を受けた「今」カメラを確かめる。定期監視のオフライン化は TTL(150秒)待ちで、
	//  電源が入っていないカメラでも ×アイコンが出るまで最大2分半かかっていた(エッジで顕著)。
	//  非ブロッキングで即確認を促し、次の hge_pump で WAITING/NOCAMERA が正しく切り替わるようにする。
	hge::role::cameraPresenceVerify(raw->plan.camera);
	// #1: 要求を受けた「今」オンラインと分かっていなければ、待たずに ×(NOCAMERA) を出す。
	//  取得シーケンス(IP直結→SSDP再送→サブネット探索)は全部走ると数十秒かかり、それを待つと
	//  電源の入っていないカメラでも×が出るのが遅い(実測30秒以上)。不明(-1)も×にする
	//  ("検索して見つかっていない"は要求時点では同じ意味)。見つかれば取得成功時に上書きされる。
	const int initSt = (hge::role::cameraPresence(raw->plan.camera) == 1) ? HGE_ST_WAITING : HGE_ST_NOCAMERA;
	// 取得スレッドを起こす前に反映しておく(起こしてからでは取得シーケンスの完了待ちになる)。
	raw->state = initSt; notifyStateP(raw->planId, initSt); refreshAggregateState();
	if (now < raw->armAtEpoch - ARM_LEAD_SEC)
	{
		raw->deferred = true;
	}
	else if (anySeqActive())
	{	// 起動シーケンスは同時1本(直列化)。実行中なら pump に委ねて数秒後に起動する。
		// 同窓2計画の一括開始で 14KB タスクスタック×4本の同時要求(断片化した内部RAMで失敗しやすい)を避ける。
		raw->deferred = true; raw->nextTryEpoch = now + 2;
	}
	else if (!launchStartThread(raw))
	{
		raw->deferred = true; raw->nextTryEpoch = now + 5;
		dataManager::logEvent("ERR", ("開始スレッド生成失敗(再試行待ち) 計画=" + raw->plan.name).c_str(), true);
	}
	setCapturing(planId, true);	// item2: 撮影意図を記録(電源復帰/再起動で再開する)
	return ERR_HGC_OK;
}

// 遅延アームのポンプ。エッジ=メインloop(毎秒)/スマホ=UIタイマ(数秒毎)から呼ぶ。
// 期日(armAt-ARM_LEAD_SEC)に達した予約セッションの開始スレッドを生成する。生成失敗(内部RAM断片化)は
// 10秒後に再試行し続ける(自己修復)。呼び出しスレッドは hge の他APIと同じでよい(軽量・非ブロッキング)。
int32_t hge_pump(void)
{
	long long now = static_cast<long long>(std::time(nullptr));
	// 項目8: 遅延アーム待ち(=撮影開始要求済みだが窓が遠く、まだ取得スレッドを作っていない)のセッションは、
	//  カメラの在否をプレゼンス情報で確認して WAITING(カメラ点灯)/NOCAMERA(×点灯)を切り替える。
	//  role::cameraPresence は占有しない(CCAPIセッションを張らない)。不明(-1)は待機のまま(×を出さない)。
	//  取得フェーズ(runner稼働)に入った後は runner が状態を管理するので、ここでは deferred のものだけ触る。
	for (auto& up : g_sessions)
	{
		captureSession* s = up.get();
		if (!s->deferred.load() || s->cancel.load()) { continue; }
		int st = s->state.load();
		if (st != HGE_ST_WAITING && st != HGE_ST_NOCAMERA) { continue; }
		int pres = hge::role::cameraPresence(s->plan.camera);
		int want = (pres == 0) ? HGE_ST_NOCAMERA : HGE_ST_WAITING;	// 0=オフライン→×, 1/-1(オンライン/不明)=待機点灯
		if (st != want) { s->state = want; notifyStateP(s->planId, want); refreshAggregateState(); }
	}
	if (anySeqActive()) { return ERR_HGC_OK; }	// 起動シーケンスは同時1本(直列化)。終わり次第、次のポンプで続きを起動する
	for (size_t i = 0; i < g_sessions.size(); ++i)
	{
		captureSession* s = g_sessions[i].get();
		if (!s->deferred.load() || s->cancel.load()) { continue; }
		if (now < s->armAtEpoch - ARM_LEAD_SEC || now < s->nextTryEpoch) { continue; }
		if (launchStartThread(s)) { s->deferred = false; }
		else
		{
			s->nextTryEpoch = now + 10;
			dataManager::logEvent("ERR", ("開始スレッド生成失敗(10秒後に再試行) 計画=" + s->plan.name).c_str(), true);
		}
		break;	// 1回のポンプで起動するのは1件(直列化)
	}
	return ERR_HGC_OK;
}

// 項目1(エッジ在否モニタ用): いずれかのセッションがカメラI/O中(取得/撮影/停止処理)か。
//  true の間はエッジのプレゼンス探索(M-SEARCH/疎通)を止め、時間厳守のシャッターI/Oと単一netThreadで
//  競合させない。撮影中の在否はランナー(取得フェーズ/接続断検知)が管理するので二重にならない。
bool hge_anyActiveCameraSession(void)
{
	for (auto& up : g_sessions)
	{
		int st = up->state.load();
		if (st == HGE_ST_CAPTURING || st == HGE_ST_SEARCHING || st == HGE_ST_READY || st == HGE_ST_STOPPING) { return true; }
	}
	return false;
}

// バッテリ切れで自ら電源を切る直前に呼ぶ。全セッションを NOCAMERA(✖) にして、
// スマホのポーリング(約10秒間隔)がその状態を1回拾えるようにする。
//  ・撮影は止める(runner停止)が、**撮影の意図(capturing.json)は消さない**。
//    電源を入れ直したとき hge_resumeCapture が再開できるようにするため
//    (ユーザーによる明示停止ではないので setCapturing(false) は呼ばない)。
//  ・状態を ERROR ではなく NOCAMERA にするのは、スマホ側の reconcileEdgePlan が
//    NOCAMERA を disconnectedPlans に入れて ✖ を点灯させるため。ERROR/IDLE では
//    「終了」と解釈されて集合から除去され、✖ が出ない。
void hge_markAllNoCameraForShutdown(void)
{
	for (auto& up : g_sessions)
	{
		if (up->runner) { up->runner->stop(); }	// 撮影ループを止める(フラグのみ。joinはしない)
		up->state = HGE_ST_NOCAMERA;
		notifyStateP(up->planId, HGE_ST_NOCAMERA);
	}
	refreshAggregateState();
}

// 計画id指定で撮影停止(planId 空=編集対象)。
int32_t hge_captureStopPlan(const char* planId_)
{
	std::string planId = (planId_ && planId_[0]) ? std::string(planId_) : g_editId;
	setCapturing(planId, false);	// item2: ユーザーによる明示停止 → 実行意図を消す(再起動で再開しない)
	for (size_t i = 0; i < g_sessions.size(); ++i)
	{
		if (g_sessions[i]->planId == planId) { stopSessionAt(i, "停止要求"); return ERR_HGC_OK; }
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
		hgc::cs cs;
		if (csjson::fromJson(saved, cs) &&
		    static_cast<long long>(std::time(nullptr)) >= hgc::toUnixUtc(cs.end, g_offMin))
		{ setCapturing(id, false); continue; }
		if (hge_captureStartPlan(id.c_str()) == ERR_HGC_OK) { ++n; }
	}
	return n;
}

} // extern "C"
