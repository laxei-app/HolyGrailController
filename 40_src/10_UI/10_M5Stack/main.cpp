// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。
// holyGrailEntity を駆動する。2系統で動作する:
//  - 単独: 固定撮影計画 + タッチの[開始]/[停止]。
//  - スマホ制御: ETP(§6)でスマホから時刻同期・計画転送・開始/停止を受ける(etpEdge)。
// UI は LCD(320x240, タッチ)。スマホと同様に撮影計画+スケジュール+撮影制御方法を
// 縦スクロール表示し、撮影制御方法をタップするとその方法の画面に切り替わる。

#include <M5Unified.h>
#include <WiFi.h>
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "common.h"
#include "holyGrailEntity.h"
#include "errorCode.h"
#include "WiFi_Connect.h"
#include "etpEdge.h"
#include "dataManager.h"
#include "osFile.h"
#include "debugOut.h"

using json = nlohmann::json;

// loopTask(setup/loop)のスタックを拡張する。
// 既定8KBでは天文計算(Astronomy Engine: FindAscent 再帰 + CalcMoon)で
// オーバーフローするため。setup() で hge_loadFixedPlan() が buildSchedule() を同期実行する。
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// 実機接続時に設定する(カメラの AP もしくは同一LANのSSID)。
static const char* WIFI_SSID = "Buffalo-G-D850";
static const char* WIFI_PASS = "rnhcftfbk75tf";

// ── 画面状態 ──────────────────────────────────────────────
enum { SCR_PLAN = 0, SCR_CCM = 1 };
static int  g_scr      = SCR_PLAN;
static int  g_ccmType  = 1;		// CCM詳細画面で表示中の種別
static int  g_scroll   = 0;		// 計画画面の縦スクロール量[px]
static int  g_maxScroll = 0;	// スクロール上限(描画時に算出)

static volatile int g_state = HGE_ST_IDLE;
static char  g_prog[64] = "";
static char  g_shot[64] = "";
static char  g_msg[80]  = "";
static bool  g_dirty = true;
static bool  g_edgeUp = false;	// ETPサーバ起動済みか(WiFi接続後に一度)

static constexpr int HEAD_H   = 28;
static constexpr int FOOT_H   = 32;
static constexpr int VIEW_TOP = HEAD_H;
static constexpr int VIEW_BOT = 240 - FOOT_H;	// 28..208

static M5Canvas g_cv(&M5.Display);	// ダブルバッファ(ちらつき防止)

// タップ判定用: 撮影制御方法の行(画面座標 + 種別)
struct hitRow { int y0; int y1; int type; };
static std::vector<hitRow> g_hits;

// 撮影中(開始シーケンス〜停止処理中まで)か。フッタのボタン表示と操作の切替に使う。
static bool isCapturing(void)
{
	return g_state == HGE_ST_CAPTURING || g_state == HGE_ST_SEARCHING || g_state == HGE_ST_STOPPING;
}

static const char* stName(int s)
{
	switch (s)
	{
	case HGE_ST_IDLE:      return "IDLE";
	case HGE_ST_SEARCHING: return "SEARCH";
	case HGE_ST_READY:     return "READY";
	case HGE_ST_CAPTURING: return "CAPTURING";
	case HGE_ST_STOPPING:  return "STOPPING";
	case HGE_ST_ERROR:     return "ERROR";
	default:               return "?";
	}
}

// 撮影制御方法の色(スマホUIと同じ配色)。
static uint16_t ccmColor(int t)
{
	switch (t)
	{
	case 1: return M5.Display.color565(0xB3, 0x9D, 0xDB);	// 夜間
	case 2: return M5.Display.color565(0xFF, 0xF5, 0x9D);	// 朝日
	case 3: return M5.Display.color565(0xFF, 0xCC, 0x80);	// 夕日
	case 4: return M5.Display.color565(0x90, 0xCA, 0xF9);	// 日中
	case 5: return M5.Display.color565(0xCE, 0x93, 0xD8);	// 月対処
	case 6: return M5.Display.color565(0xA5, 0xD6, 0xA7);	// 夜間前移行
	case 7: return M5.Display.color565(0x80, 0xCB, 0xC4);	// 夜間後移行
	default: return M5.Display.color565(0xEE, 0xEE, 0xEE);
	}
}

static const char* ccmName(int t)
{
	switch (t)
	{
	case 1: return "夜間撮影";
	case 2: return "朝日撮影";
	case 3: return "夕日撮影";
	case 4: return "日中撮影";
	case 5: return "月の影響";
	case 6: return "夜間前移行";
	case 7: return "夜間後移行";
	default: return "?";
	}
}

static const char* eventName(int e)
{
	switch (e)
	{
	case 1: return "Start";          case 2: return "日の入り";
	case 3: return "市民薄明(夕)";   case 4: return "航海薄明(夕)";
	case 5: return "天文薄明(夕)";   case 6: return "天文薄明(朝)";
	case 7: return "航海薄明(朝)";   case 8: return "市民薄明(朝)";
	case 9: return "日の出";         case 10: return "月の出";
	case 11: return "月の入り";      case 12: return "End";
	default: return "?";
	}
}

// "YYYY-MM-DDThh:mm:ss" → "MM/dd HH:mm"
static std::string mmddhhmm(const std::string& iso)
{
	if (iso.size() >= 16)
	{
		return iso.substr(5, 2) + "/" + iso.substr(8, 2) + " " + iso.substr(11, 5);
	}
	return iso;
}

// 撮影計画(スケジュール)JSONをキャッシュ付きで取得する(スクロール中の毎フレーム再パースを避ける)。
static bool getSchedule(json*& out)
{
	static std::string cacheStr;
	static json        cacheJson;
	static bool        cacheOk = false;

	int32_t len = 0;
	hge_getScheduleJson(nullptr, &len);
	if (len <= 0) { cacheOk = false; return false; }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getScheduleJson(buf.data(), &len) != ERR_HGC_OK) { cacheOk = false; return false; }
	if (cacheStr != buf.data())
	{
		cacheStr  = buf.data();
		cacheJson = json::parse(cacheStr, nullptr, false);
		cacheOk   = !cacheJson.is_discarded();
	}
	out = &cacheJson;
	return cacheOk;
}

// 計画固有の撮影制御方法 JSON を取得する。
static bool getPlanCcm(json& out)
{
	int32_t len = 0;
	hge_getPlanCcmJson(nullptr, &len);
	if (len <= 0) { return false; }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getPlanCcmJson(buf.data(), &len) != ERR_HGC_OK) { return false; }
	out = json::parse(buf.data(), nullptr, false);
	return !out.is_discarded();
}

// ── 描画ヘルパ(g_cv のカーソルに対して1行描く。戻り値=次のy) ──
static int line(int y, const std::string& s, uint16_t fg = TFT_WHITE)
{
	g_cv.setTextColor(fg);
	g_cv.setCursor(8, y);
	g_cv.print(s.c_str());
	return y + 19;
}
static int head(int y, const char* s)
{
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setCursor(6, y);
	g_cv.print(s);
	return y + 21;
}
static int rowBar(int y, uint16_t bg, uint16_t fg, const std::string& s)
{
	g_cv.fillRect(6, y, 308, 20, bg);
	g_cv.setTextColor(fg);
	g_cv.setCursor(12, y + 2);
	g_cv.print(s.c_str());
	return y + 22;
}

// ── 撮影計画(スクロール)画面 ─────────────────────────────
static void renderPlan(void)
{
	json* jp = nullptr;
	bool ok = getSchedule(jp);

	g_cv.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::efontJA_16);

	// スクロール領域(ヘッダ/フッタの間)をクリップ
	g_cv.setClipRect(0, VIEW_TOP, 320, VIEW_BOT - VIEW_TOP);
	g_hits.clear();
	int y = VIEW_TOP - g_scroll + 4;
	if (ok)
	{
		const json& j = *jp;
		y = line(y, j.value("name", std::string()));
		y = line(y, mmddhhmm(j.value("start", std::string())) + " → " + mmddhhmm(j.value("end", std::string())));
		char b[96];
		std::snprintf(b, sizeof(b), "%s  標高%dm", j.value("place", std::string()).c_str(), j.value("altitude", 0));
		y = line(y, b, TFT_LIGHTGREY);
		y = line(y, j.value("camera", std::string()) + " / " + j.value("lens", std::string()), TFT_LIGHTGREY);
		std::snprintf(b, sizeof(b), "方位 %.0f°  仰角 %.0f°", j.value("azimuth", 0.0), j.value("elevation", 0.0));
		y = line(y, b, TFT_LIGHTGREY);

		y += 4;
		y = head(y, "スケジュール");
		if (j.contains("events") && j["events"].is_array())
		{
			for (const auto& e : j["events"])
			{
				std::string row = mmddhhmm(e.value("when", std::string())) + "  " + eventName(e.value("event", 0));
				y = rowBar(y, TFT_DARKGREY, TFT_WHITE, row);
			}
		}

		y += 6;
		y = head(y, "撮影制御方法（タップで詳細）");
		// スマホと同様に全種別(夜間/朝日/夕日/日中/月)を時刻なしの色付き行で並べる。
		static const int allTypes[] = { 1, 2, 3, 4, 5 };
		for (int t : allTypes)
		{
			int y0 = y;
			y = rowBar(y, ccmColor(t), TFT_BLACK, ccmName(t));
			g_hits.push_back({ y0, y - 2, t });
		}
	}
	else
	{
		y = line(y, "(撮影計画なし)");
	}
	const int viewH = VIEW_BOT - VIEW_TOP;
	const int contentH = (y + g_scroll) - VIEW_TOP;	// 描画した総高さ
	g_maxScroll = (contentH > viewH) ? (contentH - viewH) : 0;
	g_cv.clearClipRect();

	// ヘッダ(固定)
	g_cv.fillRect(0, 0, 320, HEAD_H, M5.Display.color565(0x15, 0x65, 0xC0));
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setCursor(8, 6);   g_cv.print("撮影計画");
	g_cv.setCursor(140, 6); g_cv.print(stName(g_state));
	if (WiFi.status() == WL_CONNECTED) { g_cv.setCursor(276, 6); g_cv.print(g_edgeUp ? "ETP" : "WiFi"); }

	// フッタ(固定): 撮影中は赤[撮影停止]、それ以外は緑[撮影開始] の単一ボタン。
	bool cap = isCapturing();
	uint16_t fcol = cap ? M5.Display.color565(0xC6, 0x28, 0x28) : M5.Display.color565(0x2E, 0x7D, 0x32);
	g_cv.fillRect(0, VIEW_BOT, 320, FOOT_H, fcol);
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString(cap ? "撮影停止" : "撮影開始", 160, VIEW_BOT + FOOT_H / 2);
	g_cv.setTextDatum(textdatum_t::top_left);	// 以後の print() 用に戻す

	g_cv.pushSprite(0, 0);
}

// ── 撮影制御方法 詳細画面 ────────────────────────────────
static void renderCcm(void)
{
	const char* key = nullptr;
	switch (g_ccmType)
	{
	case 1: key = "night";   break;
	case 2: key = "sunrise"; break;
	case 3: key = "sunset";  break;
	case 4: key = "day";     break;
	case 5: key = "moon";    break;
	default: key = nullptr;  break;	// 6=夜間前移行/7=夜間後移行(編集項目なし)
	}

	g_cv.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::efontJA_16);

	// ヘッダ(撮影制御方法の色, 左に戻る記号)
	g_cv.fillRect(0, 0, 320, HEAD_H, ccmColor(g_ccmType));
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setCursor(8, 6);
	g_cv.printf("< %s", ccmName(g_ccmType));

	int y = VIEW_TOP + 6;
	json j;
	if (key && getPlanCcm(j) && j.contains(key))
	{
		const json& c = j[key];
		char b[96];
		if (g_ccmType == 1)
		{
			std::snprintf(b, sizeof(b), "固定露出 太陽高度 %.0f°", c.value("sunAltitude", 0.0));
			y = line(y, b);
			y = line(y, std::string("画角端で自動: ") + (c.value("autoEdge", false) ? "ON" : "OFF"));
			std::snprintf(b, sizeof(b), "夜間後露出補正 %+.1fev", c.value("postNightEv", 0.0));
			y = line(y, b);
		}
		else if (g_ccmType == 2 || g_ccmType == 3)
		{
			std::snprintf(b, sizeof(b), "太陽高度 %.0f° → %.0f°",
			              c.value("sunAltitude", 0.0), c.value("sunAltitudeEnd", 0.0));
			y = line(y, b);
			std::snprintf(b, sizeof(b), "露出補正 %+.1fev", c.value("ev", 0.0));
			y = line(y, b);
		}
		else if (g_ccmType == 4)
		{
			std::snprintf(b, sizeof(b), "露出補正 %+.1fev", c.value("ev", 0.0));
			y = line(y, b);
		}
		else if (g_ccmType == 5)
		{
			std::snprintf(b, sizeof(b), "対処モード %d", c.value("mode", 0));
			y = line(y, b);
			std::snprintf(b, sizeof(b), "補正開始輝度 %.1fev / 補正 %.1fev",
			              c.value("startLuminance", 0.0), c.value("ev", 0.0));
			y = line(y, b);
		}

		// 露出限界(UIの暗所限界=limitBright / 明所限界=limitDark)
		if (c.contains("limitBright") && c.contains("limitDark"))
		{
			const json& lb = c["limitBright"];
			const json& ld = c["limitDark"];
			y += 6;
			y = head(y, "露出限界");
			y = line(y, "暗所  ISO" + lb.value("iso", std::string()) + "  " +
			            lb.value("ss", std::string()) + "  F" + lb.value("fn", std::string()));
			y = line(y, "明所  ISO" + ld.value("iso", std::string()) + "  " +
			            ld.value("ss", std::string()) + "  F" + ld.value("fn", std::string()));
		}
	}
	else if (g_ccmType == 6)
	{
		y = line(y, "夜間前移行");
		y = line(y, "夜間へ滑らかに露出を変化させます");
	}
	else if (g_ccmType == 7)
	{
		y = line(y, "夜間後移行");
		y = line(y, "夜間から次へ滑らかに露出を戻します");
	}
	else
	{
		y = line(y, "(データなし)");
	}

	g_cv.setTextColor(TFT_LIGHTGREY);
	g_cv.setCursor(8, VIEW_BOT + 8);
	g_cv.print("上の帯をタップで戻る");
	g_cv.pushSprite(0, 0);
}

static void redraw(void)
{
	if (g_scr == SCR_CCM) { renderCcm(); }
	else                  { renderPlan(); }
}

// ── タップ処理 ───────────────────────────────────────────
static void onTap(int x, int y)
{
	if (g_scr == SCR_CCM)
	{
		if (y < HEAD_H) { g_scr = SCR_PLAN; g_dirty = true; }	// ヘッダ帯=戻る
		return;
	}
	// 計画画面
	if (y >= VIEW_BOT)	// フッタ: 撮影中なら停止、それ以外なら開始(単一トグルボタン)
	{
		(void)x;
		if (isCapturing()) { hge_captureStop(); }
		else               { hge_captureStart(); }
		return;
	}
	if (y >= VIEW_TOP && y < VIEW_BOT)	// 撮影制御方法の行をタップ→詳細へ
	{
		for (const auto& h : g_hits)
		{
			if (y >= h.y0 && y <= h.y1)
			{
				g_ccmType = h.type;
				g_scr = SCR_CCM;
				g_dirty = true;
				return;
			}
		}
	}
}

// ドラッグでスクロール、わずかな移動はタップとして扱う。
static void handleTouch(void)
{
	static bool prevPressed = false;
	static int  startX = 0, startY = 0, startScroll = 0;
	static bool moved = false;

	auto t = M5.Touch.getDetail();
	bool pressed = t.isPressed();
	int tx = t.x, ty = t.y;

	if (pressed && !prevPressed)
	{
		startX = tx; startY = ty; startScroll = g_scroll; moved = false;
	}
	else if (pressed && prevPressed)
	{
		if (std::abs(tx - startX) > 8 || std::abs(ty - startY) > 8) { moved = true; }
		if (g_scr == SCR_PLAN && moved)
		{
			int ns = startScroll - (ty - startY);
			if (ns < 0) { ns = 0; }
			if (ns > g_maxScroll) { ns = g_maxScroll; }
			if (ns != g_scroll) { g_scroll = ns; g_dirty = true; }
		}
	}
	else if (!pressed && prevPressed)
	{
		if (!moved) { onTap(startX, startY); }
	}
	prevPressed = pressed;
}

// Entity からの通知(ワーカースレッドから呼ばれる)。表示は loop() 側で行う。
static void notifyCb(int32_t ev, const char* json_, int32_t len, void* user)
{
	(void)len; (void)user;
	if (json_ == nullptr) { json_ = ""; }
	switch (ev)
	{
	case HGE_EV_STATE:
	{
		int s = HGE_ST_IDLE;
		std::sscanf(json_, "{\"state\":%d", &s);
		g_state = s;
		Serial.printf("[EV] STATE: %s\n", stName(s));
		break;
	}
	case HGE_EV_PROGRESS:
		std::snprintf(g_prog, sizeof(g_prog), "%s", json_);
		break;
	case HGE_EV_CAPTURED:
		std::snprintf(g_shot, sizeof(g_shot), "%s", json_);
		break;
	case HGE_EV_ERROR:
		std::snprintf(g_msg, sizeof(g_msg), "%s", json_);
		Serial.printf("[EV] ERROR: %s\n", json_);
		break;
	case HGE_EV_DEVICE:
		std::snprintf(g_msg, sizeof(g_msg), "%s", json_);
		break;
	default:
		break;	// EV_SCHEDULE 等も含め再描画させる
	}
	g_dirty = true;
}

void setup(void)
{
	auto cfg = M5.config();
	M5.begin(cfg);
	dbg::init();

	g_cv.setPsram(true);
	g_cv.setColorDepth(16);
	g_cv.createSprite(320, 240);
	g_cv.setFont(&fonts::efontJA_16);

	wifiConnect::setup();

	hge_init();
	hge_setNotify(notifyCb, nullptr);
	hge_loadFixedPlan();		// 出荷時設定(dataManager)から固定撮影計画を生成
	g_state = hge_getState();
	redraw();
}

void loop(void)
{
	M5.update();

	// WiFi 切断時は再接続を試みる(実機運用時に SSID/PASS を設定する)
	if (wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
	{
		Serial.printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
		bool ok = wifiConnect::connect(WIFI_SSID, WIFI_PASS);
		if (ok)
		{
			Serial.printf("[WIFI] connected. IP=%s RSSI=%d\n",
			              WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
			std::snprintf(g_msg, sizeof(g_msg), "WiFi %s", WiFi.localIP().toString().c_str());
			if (!g_edgeUp)
			{
				etpEdge::setup("エッジ端末");
				g_edgeUp = true;
			}
			g_dirty = true;
		}
		else { Serial.printf("[WIFI] connect failed.\n"); }
	}

	// ETP サーバのポーリング(スマホからの検索/制御を処理)
	if (g_edgeUp) { etpEdge::loop(); }

	// タッチ操作(スクロール/タップ)
	handleTouch();

	// シリアルコマンド(検証用): 's'=開始 'x'=停止 'i'=情報 'l'=ログ 'F'=保存先 'D'=内蔵ログ削除
	if (Serial.available() > 0)
	{
		int c = Serial.read();
		if (c == 's') { hge_captureStart(); }
		else if (c == 'x') { hge_captureStop(); }
		// 検証用: timeコマンド受信を模擬してUTCオフセットを設定・永続化し固定計画を再生成。
		else if (c == 'j') { hge_setUtcOffset(540); hge_loadFixedPlan(); g_state = hge_getState(); g_dirty = true; }	// JST(+9h)
		else if (c == 'u') { hge_setUtcOffset(0);   hge_loadFixedPlan(); g_state = hge_getState(); g_dirty = true; }	// UTC
		else if (c == 'a')	// 検証用: カメラが実際に受け付ける iso/ss/fn の一覧をダンプ
		{
			hge_captureStop();
			delay(1000);
			static char b[4096]; int32_t len = sizeof(b);
			int32_t r = hge_getCameraAbilityJson(b, &len);
			if (r == 0) { Serial.printf("[ABILITY] %s\n", b); }
			else        { Serial.printf("[ABILITY] failed code=%ld\n", (long)r); }
		}
		else if (c == 'i')
		{
			Serial.printf("[INFO] state=%s wifi=%d IP=%s\n", stName(g_state),
			              (int)(WiFi.status() == WL_CONNECTED), WiFi.localIP().toString().c_str());
		}
		else if (c == 'F')	// 検証用: 現在のログ保存先(SD/LittleFS)を表示
		{
			Serial.printf("[FS] backend=%s\n", osfile::backendName());
		}
		else if (c == 'D')	// 検証用: 内蔵フラッシュ(LittleFS)の /log を全削除
		{
			int n = osfile::removeInternalLogs();
			if (n >= 0) { Serial.printf("[LOGCLR] removed %d internal log file(s)\n", n); }
			else        { Serial.printf("[LOGCLR] failed (LittleFS mount)\n"); }
		}
		else if (c == 'l')
		{
			std::string path = dataManager::currentLogPath();
			std::string body;
			if (osfile::readAll(path, body))
			{
				Serial.printf("[LOG] %s (%u bytes)\n", path.c_str(), (unsigned)body.size());
				Serial.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
				Serial.printf("[LOG] end\n");
			}
			else { Serial.printf("[LOG] read failed: %s\n", path.c_str()); }
		}
	}

	if (g_dirty)
	{
		g_dirty = false;
		redraw();
	}
	delay(16);
}
