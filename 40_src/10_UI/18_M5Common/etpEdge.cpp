// エッジ端末側の ETP サーバ実装(データ構造仕様書43 §6)。
// **このファイルに機種による分岐は無い。** RTC の違い(内蔵/外付け/無し)は
// edgeRtc のインターフェース越しに扱い、実装は各機種フォルダが持つ。
#include "etpEdge.h"
#include <M5Unified.h>
#include "edgeRtc.h"	// RTC は機種ごとの実装へ委譲する(内蔵/外付け/無し)
#include <WiFi.h>
#include <WiFiUdp.h>
#include <json/nlohmann/json.hpp>
#include <sys/time.h>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "etp.h"
#include "holyGrailEntity.h"
#include "hgcCommon.h"
#include "errorCode.h"
#include "osFile.h"		// ログ一覧/分割読み出し(C_LOG_LIST / C_LOG_READ)
#include "debugOut.h"
#include "netThread.h"	// STA/APを区別しないIP列挙

using json = nlohmann::json;

// 計画名ビットマップの取り込み(実体は main.cpp)。計画 id ごとに保持する。
extern void edgeSetNameBitmap(const std::string& id, const uint8_t* data, int len);
// スマホから受信した計画 id を登録する(これだけリスト表示する。実体は main.cpp)。
extern void edgeAddReceivedPlan(const std::string& id);
extern void edgeRemoveReceivedPlan(const std::string& id);	// 項目6: C_DELETE_PLAN で計画をエッジから削除(撮影中は停止してから)

namespace
{
	constexpr uint16_t PORT_DISCOVERY = 50505;
	constexpr uint16_t PORT_CONTROL   = 50506;

	WiFiUDP               g_udp;
	WiFiServer            g_server(PORT_CONTROL);
	WiFiClient            g_client;
	std::vector<uint8_t>  g_rx;		// TCP 受信バッファ(フレーミング用)
	std::string           g_name = "エッジ端末";
	uint32_t              g_lastRx = 0;			// g_client の最終受信時刻(ms)。stale接続の回収に使う
	// これ以上無通信の接続は回収してスロットを空ける。スマホの状態ポーリングは約10秒間隔なので、
	// それを十分上回る値にして永続接続がポール間で切れないようにする(案B)。half-open滞留は
	// 新接続の優先受理(下記 hasClient 分岐)が即座に解消するため、この値は長めでも安全。
	constexpr uint32_t    TCP_IDLE_MS = 30000;

	// --- RTC(UTC保持運用) ---
	// 内蔵/外付け/無しの違いは edgeRtc(機種別実装)が吸収する。ここでは分岐しない。

	void setRtcFromUtc(long long utc)
	{
		time_t t = static_cast<time_t>(utc);
		std::tm g{};
		gmtime_r(&t, &g);
		m5::rtc_datetime_t dt;
		dt.date.year    = g.tm_year + 1900;
		dt.date.month   = static_cast<int8_t>(g.tm_mon + 1);
		dt.date.date    = static_cast<int8_t>(g.tm_mday);
		dt.date.weekDay = static_cast<int8_t>(g.tm_wday);
		dt.time.hours   = static_cast<int8_t>(g.tm_hour);
		dt.time.minutes = static_cast<int8_t>(g.tm_min);
		dt.time.seconds = static_cast<int8_t>(g.tm_sec);
		edgeRtc::set(dt);	// RTCが無い機種/構成では何もしない(機種別実装が判断する)
	}

	void setSystemClock(long long utc)
	{
		timeval tv{};
		tv.tv_sec  = static_cast<time_t>(utc);
		tv.tv_usec = 0;
		settimeofday(&tv, nullptr);
	}

	// 起動時: RTC(UTC) からシステム時計を復元する。
	// RTC が無い/読めない構成では何もしない(従来どおりスマホからの time コマンドを待つ)。
	void restoreClockFromRtc(void)
	{
		m5::rtc_datetime_t dt;
		if (!edgeRtc::get(dt)) { return; }
		hgc::dateTime d;
		d.year  = static_cast<uint16_t>(dt.date.year);
		d.month = static_cast<uint16_t>(dt.date.month);
		d.day   = static_cast<uint16_t>(dt.date.date);
		d.hour  = static_cast<uint16_t>(dt.time.hours);
		d.min   = static_cast<uint16_t>(dt.time.minutes);
		d.sec   = static_cast<uint16_t>(dt.time.seconds);
		long long utc = hgc::toUnixUtc(d, 0);	// RTC は UTC
		if (utc < 1577836800LL)
		{	// 2020-01-01 より前は未設定(電池切れ/初回)とみなす。スマホからの time を待つ。
			DBGLN(col::YEL, "etpEdge: RTC not set (%04u-%02u-%02u %02u:%02u:%02u UTC) -> wait time cmd",
			      d.year, d.month, d.day, d.hour, d.min, d.sec);
			return;
		}
		setSystemClock(utc);
		DBGLN(col::GRN, "etpEdge: clock restored from RTC %04u-%02u-%02u %02u:%02u:%02u UTC",
		      d.year, d.month, d.day, d.hour, d.min, d.sec);
	}

	// time コマンド(RTC/時計同期)を適用する。return: 成功
	bool applyTime(const std::string& data)
	{
		json j = json::parse(data, nullptr, false);
		if (j.is_discarded() || !j.is_object()) { return false; }
		std::string dts = j.value("datetime", std::string());
		int off = j.value("utcOffsetMin", 0);

		hgc::dateTime d{};
		if (std::sscanf(dts.c_str(), "%hu-%hu-%huT%hu:%hu:%hu",
		                &d.year, &d.month, &d.day, &d.hour, &d.min, &d.sec) != 6)
		{
			return false;
		}
		long long utc = hgc::toUnixUtc(d, off);	// ローカル+オフセット → UTC
		setSystemClock(utc);
		setRtcFromUtc(utc);
		hge_setUtcOffset(off);
		DBGLN(col::GRN, "etpEdge: time set off=%d", off);
		return true;
	}

	// 検索応答 edgeInfo を作る。
	std::string edgeInfoJson(void)
	{
		json j;
		j["name"]  = g_name;
		// 自分のIP。**モードで場合分けしない**(2026-08-19)。net が STA/AP を問わず
		//  「今使えるIPv4」を返すので、その先頭をそのまま通知する。
		//  (以前は WiFi.localIP() が AP時 0.0.0.0 になるため自前で分岐していた)
		{
			std::vector<std::string> ips = netThread::getLocalIpList();
			j["ip"] = ips.empty() ? std::string() : ips.front();
		}
		j["port"]  = PORT_CONTROL;
		j["model"] = "エッジ端末";
		j["fw"]    = std::string(hge_version());
		j["state"] = hge_getState();
		// 実行中セッション一覧。スマホの常時スイープが「このエッジで何が走っているか」を検索応答1発で
		// 把握し、エッジ側ローカル開始/再起動後の自動再開/停止をスマホ表示へ同期する(§6.2.1)。
		{
			char sb[512];
			int32_t sl = sizeof(sb);
			if (hge_getSessionsJson(sb, &sl) == ERR_HGC_OK)
			{
				json s = json::parse(sb, nullptr, false);
				if (!s.is_discarded()) { j["sessions"] = s; }
			}
		}
		// 溜まっている撮影レポートの件数(2026-08-05)。スマホの常時スイープはこれが1件以上のときだけ
		// C_REPORT_LIST/READ を投げる。件数はファイル名を数えるだけで中身は読まないので、
		// 30秒ごとに毎回聞かれても負荷にならない。
		{
			const int32_t rc = hge_reportCount();
			if (rc > 0) { j["reports"] = rc; }
		}
		// 項目6: 保有計画ロスター(走行中に限らずエッジが持つ全計画id)。スマホは自分のエッジ担当割り当てと
		//  突き合わせ、ここに無い=エッジ側で削除された計画のロックを解除する。
		{
			char hb[768];
			int32_t hl = sizeof(hb);
			if (hge_getHeldPlansJson(hb, &hl) == ERR_HGC_OK)
			{
				json h = json::parse(hb, nullptr, false);
				if (!h.is_discarded()) { j["heldPlans"] = h; }
			}
		}
		return j.dump();
	}

	// 1 つの要求を処理して応答フレームを組み立てて返す。
	//
	// 【トランスポートに依存しない(2026-08-14)】従来は最後に g_client.write() まで行っていたので
	//  TCP 専用だった。BLE からも同じ処理を通すため、応答は「返す」だけにして、どこへ送るかは
	//  呼んだ側(pollTcp / etpBle)に任せる。cmd の処理内容は一切変えていない。
	std::vector<uint8_t> buildReply(const etp::packet& pk)
	{
		DBGLN(col::YEL, "etpEdge: rx cmd=%u m=%u len=%u", (unsigned)pk.cmd, (unsigned)pk.method, (unsigned)pk.data.size());
		uint16_t rm = etp::M_ACK;
		std::string rd;
		switch (pk.cmd)
		{
		case etp::C_SEARCH:
			// 検索応答。TCP では UDP 側(pollUdp)が受けるのでここへは来ないが、BLE には
			// ブロードキャストが無いので、スマホは接続してから C_SEARCH で edgeInfo を取る。
			rd = edgeInfoJson();
			break;
		case etp::C_TIME:
			if (!applyTime(pk.data)) { rm = etp::M_NAK; }
			break;
		case etp::C_CAPTURE_PLAN:
		{
			// data = "id\t{plan json}"(id 空可)。エッジは id ごとに計画を蓄積する(複数計画をリスト表示)。
			std::string id, body;
			size_t tab = pk.data.find('\t');
			if (tab != std::string::npos) { id = pk.data.substr(0, tab); body = pk.data.substr(tab + 1); }
			else { body = pk.data; }
			int32_t ir = hge_importPlan(id.c_str(), body.c_str(), static_cast<int32_t>(body.size()));
			DBGLN(col::YEL, "etpEdge: CAPTURE_PLAN id='%s' bodyLen=%u import=%ld", id.c_str(), (unsigned)body.size(), (long)ir);
			if (ir != ERR_HGC_OK)
			{ rm = etp::M_NAK; }
			else { edgeAddReceivedPlan(id); }	// 受信した計画だけリスト表示する
			break;
		}
		case etp::C_NAME_BMP:	// data = "id\t<bitmap>\x01"。計画 id ごとに名前ビットマップを保持する
		{
			size_t tab = pk.data.find('\t');
			std::string id  = (tab != std::string::npos) ? pk.data.substr(0, tab) : std::string();
			const char* p   = (tab != std::string::npos) ? pk.data.data() + tab + 1 : pk.data.data();
			int         pl  = (tab != std::string::npos) ? static_cast<int>(pk.data.size() - tab - 1) : static_cast<int>(pk.data.size());
			// ETPの末尾空白/NUL除去(etp::decode)でビットマップ末尾の黒画素(0x00)が欠落するのを防ぐ番兵を除去する。
			if (pl > 0 && p[pl - 1] == '\x01') { --pl; }
			edgeSetNameBitmap(id, reinterpret_cast<const uint8_t*>(p), pl);
			break;
		}
		case etp::C_CONTROL_METHOD:
			break;	// 将来用。現状は受領のみ(ccmListは計画に内包)
		case etp::C_ACTION:	// data に計画 id があればその計画を開始(無ければ従来の単一開始)
		{
			int32_t ar = (!pk.data.empty()) ? hge_captureStartPlan(pk.data.c_str()) : hge_captureStart();
			DBGLN(col::YEL, "etpEdge: ACTION planId='%s' start=%ld state=%d", pk.data.c_str(), (long)ar, (int)hge_getState());
			if (ar != ERR_HGC_OK) { rm = etp::M_NAK; }	// 診断+改善: 開始失敗をスマホへ返す(従来は常にACKだった)
			break;
		}
		case etp::C_STOP:
			if (!pk.data.empty()) { hge_captureStopPlan(pk.data.c_str()); }
			else                  { hge_captureStop(); }
			break;
		case etp::C_DELETE_PLAN:	// 項目6: スマホで停止した計画をエッジからも削除(撮影中なら停止してから消す=項目9)
			if (!pk.data.empty())
			{
				DBGLN(col::YEL, "etpEdge: DELETE_PLAN planId='%s'", pk.data.c_str());
				edgeRemoveReceivedPlan(pk.data);	// g_recvPlans/計画ファイル/名前ビットマップ削除 + hge_deletePlan で走行中セッション停止
			}
			break;
		case etp::C_RESEARCH:	// 継続: カメラ未検出時の即再探索(取得フェーズの60秒待ちを前倒し)
			hge_pokeAcquire(pk.data.empty() ? nullptr : pk.data.c_str());
			break;
		case etp::C_CAMERA_INFO:	// スマホが発見中のオンラインカメラ情報。既知IPテーブルを更新(発見のIP直結ヒント)
			if (!pk.data.empty()) { hge_setKnownCameras(pk.data.c_str(), (int32_t)pk.data.size()); }
			break;
		case etp::C_PROGRESS:
		{
			// data=planId なら計画別の状態/進捗を返す(1エッジ複数カメラで誤NOCAMERAポップを防ぐ)。
			// data 空は従来の集約スナップショット(復元/生存確認用)。
			char b[512];	// serial/assignedName/model(書き戻し用)を含むため 256→512
			int32_t len = sizeof(b);
			const char* pid = pk.data.empty() ? nullptr : pk.data.c_str();
			if (hge_getProgressJsonFor(pid, b, &len) == ERR_HGC_OK) { rd = b; }
			break;
		}
		case etp::C_DIRECTION:
			rd = "{\"azimuth\":0,\"elevation\":0}";
			break;
		case etp::C_LOG_LIST:	// ログファイル名一覧を JSON 配列で返す(スマホがログ取得メニューで使用)
		{
			json arr = json::array();
			for (auto& n : osfile::logFileNames()) { arr.push_back(n); }
			rd = arr.dump();
			break;
		}
		case etp::C_LOG_READ:	// data="name\t<offset>"。offset から最大 CHUNK バイトを生バイトで返す(分割転送)
		{
			constexpr size_t CHUNK = 4096;
			size_t tab = pk.data.find('\t');
			std::string name = (tab != std::string::npos) ? pk.data.substr(0, tab) : pk.data;
			size_t off = (tab != std::string::npos) ? (size_t)strtoul(pk.data.c_str() + tab + 1, nullptr, 10) : 0;
			// パストラバーサル防止: 区切り文字を含む名前は拒否(logDir 直下のみ許可)。
			if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos) { rm = etp::M_NAK; break; }
			std::string chunk;
			if (!osfile::readRange(osfile::logDir() + "/" + name, off, CHUNK, chunk)) { rm = etp::M_NAK; break; }
			// ETPフレームは末尾の空白/NULを除去する(etp::decode)。ログは固定長128Bの空白詰めのため、
			// チャンク末尾の実データ空白が失われないよう番兵 0x01 を付ける(スマホ側で1バイト除去)。
			rd = chunk; rd.push_back('\x01');	// 空チャンク(=EOF)でも rd="\x01" となり ACK で返る
			break;
		}
		// --- 撮影レポートの回収(2026-08-05) ---
		// エッジで撮った撮影のレポートを、スマホの30秒スイープが吸い上げて持って行く。
		// 削除はスマホが「保存できた」と言ってきたとき(C_REPORT_DELETE)だけ行う。取得しただけで
		// 消すと、途中で通信が切れたぶんが永久に失われるため。
		case etp::C_REPORT_LIST:
		{
			char b[1024];
			int32_t len = sizeof(b);
			if (hge_reportListJson(b, &len) == ERR_HGC_OK) { rd = b; }
			else                                           { rd = "[]"; }
			break;
		}
		case etp::C_REPORT_READ:	// data=ファイル名。中身(JSON)をそのまま返す
		{
			// レポート1件は約1KB。想定外に大きいものはエッジのRAMを守るため読まずに断る
			// (スマホ側は受け取れなかった=削除しないので、後からログ取得で回収できる)。
			constexpr size_t MAX_REPORT = 8192;
			// パストラバーサル防止: 区切り文字を含む名前は拒否(logDir 直下のみ許可。C_LOG_READ と同じ)。
			if (pk.data.empty() || pk.data.find('/') != std::string::npos ||
			    pk.data.find("..") != std::string::npos) { rm = etp::M_NAK; break; }
			std::string body;
			if (!osfile::readRange(osfile::logDir() + "/" + pk.data, 0, MAX_REPORT, body) || body.empty())
			{ rm = etp::M_NAK; break; }
			if (body.size() >= MAX_REPORT) { rm = etp::M_NAK; break; }	// 途中で切れている疑い
			rd = body;
			break;
		}
		case etp::C_REPORT_DELETE:	// data=ファイル名。スマホが保存できたものだけを消す
			if (pk.data.empty() || hge_removeReport(pk.data.c_str()) != ERR_HGC_OK) { rm = etp::M_NAK; }
			break;
		default:
			rm = etp::M_NAK;
			break;
		}
		return etp::encode(pk.cmd, rm, rd);
	}

	// UDP 検索のポーリング
	void pollUdp(void)
	{
		int sz = g_udp.parsePacket();
		if (sz <= 0) { return; }
		uint8_t buf[512];
		int n = g_udp.read(buf, sizeof(buf));
		if (n <= 0) { return; }
		etp::packet pk;
		int c = etp::decode(buf, static_cast<size_t>(n), pk);
		if (c > 0 && pk.cmd == etp::C_SEARCH && pk.method == etp::M_GET)
		{
			std::vector<uint8_t> out = etp::encode(etp::C_SEARCH, etp::M_ACK, edgeInfoJson());
			g_udp.beginPacket(g_udp.remoteIP(), g_udp.remotePort());
			g_udp.write(out.data(), out.size());
			g_udp.endPacket();
			DBGLN(col::CYN, "etpEdge: search reply");
		}
	}

	// TCP 制御のポーリング。
	// 単一クライアント方式のため、half-open接続が居座ると新規接続がバックログで待たされ
	// 「C_TIMEは通るのに後続が処理されない」スタックに陥る。防御として次の2点を入れる:
	//  (1) 新規接続が来ていて現接続がアイドル(未読バイト無し/フレーム途中でない)なら差し替える。
	//  (2) 一定時間(TCP_IDLE_MS)無通信の接続は stop() して回収し、スロットを空ける。
	void pollTcp(void)
	{
		// (1) 新規接続を優先受理: 現接続がアイドル(処理中でない)なら古い方を切って差し替える。
		if (g_server.hasClient())
		{
			bool curActive = g_client && g_client.connected() && (g_client.available() > 0 || !g_rx.empty());
			if (!curActive)
			{
				WiFiClient nc = g_server.available();
				if (nc)
				{
					if (g_client) { g_client.stop(); }
					g_client = nc; g_rx.clear(); g_lastRx = millis();
					DBGLN(col::CYN, "etpEdge: client connected (accepted newer)");
				}
			}
		}

		if (!g_client || !g_client.connected())
		{
			g_client = g_server.available();
			if (g_client) { g_rx.clear(); g_lastRx = millis(); DBGLN(col::CYN, "etpEdge: client connected"); }
			else          { return; }
		}
		size_t got = 0;
		while (g_client.available() > 0)
		{
			g_rx.push_back(static_cast<uint8_t>(g_client.read()));
			++got;
		}
		if (got > 0) { g_lastRx = millis(); DBGLN(col::CYN, "etpEdge: rx +%u (rxLen=%u)", (unsigned)got, (unsigned)g_rx.size()); }
		size_t pos = 0;
		while (pos < g_rx.size())
		{
			etp::packet pk;
			int c = etp::decode(g_rx.data() + pos, g_rx.size() - pos, pk);
			if (c > 0)
			{
				std::vector<uint8_t> out = buildReply(pk);
				g_client.write(out.data(), out.size());
				pos += static_cast<size_t>(c);
			}
			else if (c == 0){ break; }		// データ不足
			else            { DBGLN(col::RED, "etpEdge: decode resync (bad byte, rxLen=%u)", (unsigned)(g_rx.size() - pos)); pos += 1; }	// 不正: 1バイト進めて再同期
		}
		if (pos > 0) { g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<long>(pos)); }

		// (2) アイドルが続く接続(half-open含む)を回収してスロットを空ける。
		//     フレーム途中(g_rx非空)は保護。スマホは永続接続で ~2秒周期に通信するので通常は回収されない。
		if (g_client && g_client.connected() && g_rx.empty() && (millis() - g_lastRx) > TCP_IDLE_MS)
		{
			DBGLN(col::CYN, "etpEdge: recycle idle client (%ums)", (unsigned)(millis() - g_lastRx));
			g_client.stop();
			g_rx.clear();
		}
	}
}

namespace etpEdge
{
	// BLE 経路から呼ぶ。1フレームを処理して応答フレームを返す(トランスポート非依存)。
	std::vector<uint8_t> handleFrame(const etp::packet& pk)
	{
		return buildReply(pk);
	}

	// 検索応答と同じ edgeInfo。BLE でも同じものを返すので公開する。
	std::string infoJson(void)
	{
		return edgeInfoJson();
	}

	void setup(const std::string& edgeName)
	{
		if (!edgeName.empty()) { g_name = edgeName; }
		edgeRtc::begin();			// RTCの初期化(実装は機種ごと)
		restoreClockFromRtc();		// RTC(内蔵/外付け)があればシステム時計を復元する
		g_udp.begin(PORT_DISCOVERY);
		g_server.begin();
		g_server.setNoDelay(true);
		DBGLN(col::GRN, "etpEdge: servers up (udp %u / tcp %u)", PORT_DISCOVERY, PORT_CONTROL);
	}

	void loop(void)
	{
		pollUdp();
		pollTcp();
	}
}
