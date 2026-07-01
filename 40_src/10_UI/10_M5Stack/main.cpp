// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。
// holyGrailEntity を駆動する。2系統で動作する:
//  - 単独: 固定撮影計画 + タッチの[開始]/[停止]。
//  - スマホ制御: ETP(§6)でスマホから時刻同期・計画転送・開始/停止を受ける(etpEdge)。
// UI は LCD(320x240, タッチ)。スマホと同様に撮影計画+スケジュール+撮影制御方法を
// 縦スクロール表示し、撮影制御方法をタップするとその方法の画面に切り替わる。

#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_random.h>
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include "common.h"
#include "edgeIcons.h"   // スマホUIと同じ 開始/撮影中 アイコン(RGB565)
#include "holyGrailEntity.h"
#include "errorCode.h"
#include "WiFi_Connect.h"
#include "etpEdge.h"
#include "edgeProv.h"
#include "dataManager.h"
#include "osFile.h"
#include "osClock.h"
#include "debugOut.h"

using json = nlohmann::json;

// loopTask(setup/loop)のスタックを拡張する。
// 既定8KBでは天文計算(Astronomy Engine: FindAscent 再帰 + CalcMoon)で
// オーバーフローするため。setup() で hge_loadFixedPlan() が buildSchedule() を同期実行する。
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// WiFi 認証情報のフォールバック(NVS未設定時)。実運用では設定(QR+PoP)でNVSへ保存する。
static const char* WIFI_SSID = "Buffalo-G-D850";
static const char* WIFI_PASS = "rnhcftfbk75tf";

// NVS に保存する接続情報(仕様8.2.1: 端末識別名 / SSID / password)。
static std::string g_ssid    = WIFI_SSID;
static std::string g_pass    = WIFI_PASS;
static std::string g_devName = "エッジ端末";
static bool        g_provMode = false;	// プロビジョニング表示(QR)中か
static std::string g_pop;				// 現在のPoP(乱数)

// NVS(Preferences 名前空間 "hgc")から接続情報を読み込む。
static void loadEdgeCreds(void)
{
	Preferences p;
	if (p.begin("hgc", true))
	{
		String s = p.getString("ssid", ""), w = p.getString("pass", ""), n = p.getString("devname", "");
		if (s.length()) { g_ssid = s.c_str(); }
		if (w.length()) { g_pass = w.c_str(); }
		if (n.length()) { g_devName = n.c_str(); }
		p.end();
	}
}
// 接続情報を NVS へ保存して反映する(プロビジョニング受信時に呼ぶ)。
void saveEdgeCreds(const std::string& ssid, const std::string& pass, const std::string& name)
{
	Preferences p;
	if (p.begin("hgc", false))
	{
		p.putString("ssid", ssid.c_str());
		p.putString("pass", pass.c_str());
		p.putString("devname", name.c_str());
		p.end();
	}
	g_ssid = ssid; g_pass = pass; if (!name.empty()) { g_devName = name; }
}
// 現在の PoP を返す(BLEプロビジョニングの復号鍵導出に使う)。
const std::string& edgePop(void) { return g_pop; }

// ── 画面状態 ──────────────────────────────────────────────
enum { SCR_PLAN = 0, SCR_CCM = 1 };
static int  g_scr      = SCR_PLAN;
static int  g_ccmType  = 1;		// CCM詳細画面で表示中の種別
static int  g_scroll   = 0;		// 計画画面の縦スクロール量[px]
static int  g_maxScroll = 0;	// スクロール上限(描画時に算出)
static bool g_listDirty = true;	// 計画リスト(hge_listPlansJson)の再取得が必要

static volatile int g_state = HGE_ST_IDLE;
static bool  g_blinkOn = true;	// 撮影中(緑カメラ)アイコンの点滅状態。接続断(赤)は点灯のまま。
static char  g_prog[64] = "";
static char  g_shot[64] = "";

// (Phase4: カメラ未検出の赤点灯代替は ICON_CAMERA_NG(✖カメラ)へ置換。旧 capturingRedIcon は廃止)
static char  g_msg[80]  = "";
static bool  g_dirty = true;
static bool  g_edgeUp = false;	// ETPサーバ起動済みか(WiFi接続後に一度)

// スマホから受信した計画 id のみリスト表示する(エッジ自身の FixedPlan 等は出さない)。
// 再起動後も受信済み計画を表示できるよう /asset/recvPlans.json に永続化する(item1)。
static std::set<std::string> g_recvPlans;
static std::string recvPlansPath(void) { return osfile::dir("asset") + "/recvPlans.json"; }
static void saveRecvPlans(void)
{
	json j = json::array();
	for (const auto& id : g_recvPlans) { j.push_back(id); }
	std::string s = j.dump();
	osfile::writeAll(recvPlansPath(), s.data(), s.size());
}
static void loadRecvPlans(void)
{
	std::string s;
	if (!osfile::readAll(recvPlansPath(), s) || s.empty()) { return; }
	json j = json::parse(s, nullptr, false);
	if (!j.is_array()) { return; }
	for (const auto& e : j) { if (e.is_string()) { g_recvPlans.insert(e.get<std::string>()); } }
	g_listDirty = true; g_dirty = true;
}
void edgeAddReceivedPlan(const std::string& id)
{
	if (!id.empty()) { g_recvPlans.insert(id); saveRecvPlans(); g_listDirty = true; g_dirty = true; }
}

// 計画名ビットマップ(スマホから ETP C_NAME_BMP で計画idごとに受信。1bpp MSB先頭, 1=白)。
struct nameBmp { int w = 0, h = 0; uint8_t* px = nullptr; };
static std::map<std::string, nameBmp> g_nameBmps;	// 計画id -> 名前ビットマップ

// 受信データ width(u16LE) height(u16LE) pixels[ceil(w/8)*h] を計画 id に紐づけて取り込む。
void edgeSetNameBitmap(const std::string& id, const uint8_t* data, int len)
{
	if (data == nullptr || len < 4) { return; }
	int w = data[0] | (data[1] << 8);
	int h = data[2] | (data[3] << 8);
	if (w <= 0 || h <= 0 || w > 320 || h > 120) { return; }
	int bpr = (w + 7) / 8; int need = 4 + bpr * h;
	if (len < need) { return; }
	uint8_t* nb = (uint8_t*)malloc((size_t)(bpr * h));
	if (!nb) { return; }
	memcpy(nb, data + 4, (size_t)(bpr * h));
	nameBmp& slot = g_nameBmps[id];
	if (slot.px) { free(slot.px); }
	slot.px = nb; slot.w = w; slot.h = h;
	g_dirty = true;
}

// 受信計画をエッジから削除する(item4)。受信リスト・プランファイル・名前ビットマップを消す。
void edgeRemoveReceivedPlan(const std::string& id)
{
	if (id.empty()) { return; }
	if (g_recvPlans.erase(id) > 0) { saveRecvPlans(); }
	hge_deletePlan(id.c_str());
	auto it = g_nameBmps.find(id);
	if (it != g_nameBmps.end()) { if (it->second.px) { free(it->second.px); } g_nameBmps.erase(it); }
	g_listDirty = true; g_dirty = true;
}
// 削除確認ダイアログ対象の計画id(空=非表示)。item4。
static std::string g_confirmDelId;

static constexpr int HEAD_H   = 28;
static constexpr int FOOT_H   = 32;
static constexpr int VIEW_TOP = HEAD_H;
static constexpr int VIEW_BOT = 240 - FOOT_H;	// 28..208

static M5Canvas g_cv(&M5.Display);	// ダブルバッファ(ちらつき防止)

// タップ判定用: 撮影制御方法の行(画面座標 + 種別)
struct hitRow { int y0; int y1; int type; };
static std::vector<hitRow> g_hits;

// タップ判定用: 撮影計画リストの行(画面座標 + 計画id + 状態)
struct planHit { int y0; int y1; std::string id; bool capturing; bool capturable; };
static std::vector<planHit> g_planHits;

// 撮影中(開始シーケンス〜停止処理中まで)か。フッタのボタン表示と操作の切替に使う。
static bool isCapturing(void)
{
	return g_state == HGE_ST_CAPTURING || g_state == HGE_ST_SEARCHING || g_state == HGE_ST_STOPPING || g_state == HGE_ST_DISCONNECTED;
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
	case HGE_ST_DISCONNECTED: return "DISCONN";
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

// 撮影計画リスト(hge_listPlansJson)をキャッシュ付きで取得する。g_listDirty が立つと再取得する
// (スクロール中の毎フレーム再読込=全計画ファイル読み直しを避ける)。
static const json& planList(void)
{
	static json cache = json::array();
	if (g_listDirty)
	{
		cache = json::array();
		int32_t len = 0;
		hge_listPlansJson(nullptr, &len);
		if (len > 0)
		{
			std::vector<char> buf(static_cast<size_t>(len));
			if (hge_listPlansJson(buf.data(), &len) == ERR_HGC_OK)
			{
				json j = json::parse(buf.data(), nullptr, false);
				if (!j.is_discarded() && j.is_array())
				{	// 受信した計画(g_recvPlans)だけ残す。エッジ自身の FixedPlan 等は除外。
					for (const auto& p : j)
					{
						if (g_recvPlans.count(p.value("id", std::string())) > 0) { cache.push_back(p); }
					}
				}
			}
		}
		g_listDirty = false;
	}
	return cache;
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

// ── 撮影計画画面(簡素化: 計画名 + 開始/停止のみ。仕様 8.1) ─────────────
// 計画名はスマホからモノクロ2値ビットマップで受信していればそれを、無ければ
// スケジュールJSONの名称テキストを表示する(今後の多言語対応のため §8.2.1)。
// 撮影計画リスト画面。各行に名称+開始/終了時刻と、左に開始/停止アイコン(終了>現在 の計画のみ)。
// 左アイコンをタップでその計画を開始/停止する(スマホUIと同様。指示5)。
static void renderPlan(void)
{
	const json& arr = planList();

	g_cv.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::efontJA_16);

	// ヘッダ
	g_cv.fillRect(0, 0, 320, HEAD_H, M5.Display.color565(0x15, 0x65, 0xC0));
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setCursor(8, 6);   g_cv.print("撮影計画");
	if (WiFi.status() == WL_CONNECTED) { g_cv.setCursor(276, 6); g_cv.print(g_edgeUp ? "ETP" : "WiFi"); }

	const int rowH = 50;
	const int top  = HEAD_H;
	const int bot  = 240;
	int total = static_cast<int>(arr.size()) * rowH;
	g_maxScroll = (total > (bot - top)) ? (total - (bot - top)) : 0;
	if (g_scroll > g_maxScroll) { g_scroll = g_maxScroll; }

	g_planHits.clear();
	int y = top - g_scroll;
	for (const auto& p : arr)
	{
		std::string id   = p.value("id",    std::string());
		std::string name = p.value("name",  std::string());
		std::string st   = p.value("start", std::string());
		std::string en   = p.value("end",   std::string());
		bool capturable  = p.value("capturable", false);
		int  state       = p.value("state", 0);
		// 状態→表示(指示1/2): 未検出(NOCAMERA/旧DISCONNECTED)=✖点灯 / 撮影中=点滅 /
		// 待機(撮影窓前)・探索中=点灯 / 撮影可=開始(クラッパーREC)。
		bool nocam     = (state == HGE_ST_NOCAMERA || state == HGE_ST_DISCONNECTED);
		bool capturing = (state == HGE_ST_CAPTURING || state == HGE_ST_STOPPING);	// 実撮影中=点滅
		bool waiting   = (state == HGE_ST_WAITING || state == HGE_ST_SEARCHING);		// 待機/探索=点灯
		bool active    = (nocam || capturing || waiting);	// 何らか実行中(タップで中止可)
		int ry0 = y, ry1 = y + rowH;
		if (ry1 > top && ry0 < bot)
		{
			if (nocam)
			{	// カメラ未検出(✖カメラ)=点灯。Phase4: ICOカメラNon.png 由来の ICON_CAMERA_NG。
				g_cv.pushImage(6, ry0 + (rowH - ICON_CAMERA_NG_H) / 2, ICON_CAMERA_NG_W, ICON_CAMERA_NG_H, ICON_CAMERA_NG);
			}
			else if (capturing)
			{	// 撮影中=点滅
				if (g_blinkOn) { g_cv.pushImage(6, ry0 + (rowH - ICON_CAPTURING_H) / 2, ICON_CAPTURING_W, ICON_CAPTURING_H, ICON_CAPTURING); }
			}
			else if (waiting)
			{	// 待機(撮影窓前)=点灯
				g_cv.pushImage(6, ry0 + (rowH - ICON_CAPTURING_H) / 2, ICON_CAPTURING_W, ICON_CAPTURING_H, ICON_CAPTURING);
			}
			else if (capturable)
			{
				g_cv.pushImage(6, ry0 + (rowH - ICON_START_H) / 2, ICON_START_W, ICON_START_H, ICON_START);
			}
			// 計画名: スマホから受信したモノクロ2値ビットマップで描画(無ければテキストにフォールバック)。
			auto it = g_nameBmps.find(id);
			if (it != g_nameBmps.end() && it->second.px)
			{
				const nameBmp& nb = it->second;
				int dx = 52, dy = ry0 + 4;
				int dw = nb.w > 260 ? 260 : nb.w;
				int dh = nb.h > (rowH - 22) ? (rowH - 22) : nb.h;	// 時刻行と重ならない高さに制限
				const int bpr = (nb.w + 7) / 8;
				for (int yy = 0; yy < dh; ++yy)
					for (int xx = 0; xx < dw; ++xx)
					{
						uint8_t byte = nb.px[yy * bpr + (xx >> 3)];
						if (byte & (0x80 >> (xx & 7))) { g_cv.drawPixel(dx + xx, dy + yy, TFT_WHITE); }
					}
			}
			else
			{
				g_cv.setTextColor(TFT_WHITE);
				g_cv.setCursor(52, ry0 + 4); g_cv.print(name.c_str());
			}
			g_cv.setTextColor(TFT_LIGHTGREY);
			g_cv.setCursor(52, ry0 + 30); g_cv.print((mmddhhmm(st) + " -> " + mmddhhmm(en)).c_str());
			// 右端: ゴミ箱アイコン(タップで削除確認。item4)。
			{
				int tx0 = 290, tyc = ry0 + rowH / 2;
				uint16_t tcol = M5.Display.color565(0xCC, 0x44, 0x44);
				g_cv.fillRect(tx0 - 2, tyc - 8, 24, 3, tcol);		// ふた
				g_cv.fillRect(tx0 + 6, tyc - 11, 6, 3, tcol);		// 取っ手
				g_cv.fillRect(tx0, tyc - 4, 20, 14, tcol);			// 本体
				g_cv.drawFastVLine(tx0 + 6,  tyc - 1, 8, TFT_BLACK);	// 溝
				g_cv.drawFastVLine(tx0 + 13, tyc - 1, 8, TFT_BLACK);
			}
			g_cv.drawFastHLine(0, ry1 - 1, 320, M5.Display.color565(0x33, 0x33, 0x33));
		}
		g_planHits.push_back({ ry0, ry1, id, active, capturable });
		y += rowH;
	}
	if (arr.empty())
	{
		g_cv.setTextColor(TFT_LIGHTGREY);
		g_cv.setCursor(16, top + 24); g_cv.print("計画なし");
		g_cv.setCursor(16, top + 48); g_cv.print("スマホから転送してください");
	}

	// 削除確認ダイアログ(item4)。はい=削除 / いいえ=取消。ボタン座標は onTap と一致させる。
	if (!g_confirmDelId.empty())
	{
		int bx = 30, by = 84;
		g_cv.fillRoundRect(bx, by, 260, 92, 6, M5.Display.color565(0x22, 0x22, 0x22));
		g_cv.drawRoundRect(bx, by, 260, 92, 6, TFT_WHITE);
		g_cv.setTextColor(TFT_WHITE);
		g_cv.setCursor(bx + 16, by + 14); g_cv.print("削除しますか？");
		g_cv.fillRoundRect(bx + 20,  by + 52, 90, 30, 4, M5.Display.color565(0xCC, 0x44, 0x44));
		g_cv.setCursor(bx + 50,  by + 60); g_cv.print("はい");
		g_cv.fillRoundRect(bx + 150, by + 52, 90, 30, 4, M5.Display.color565(0x55, 0x55, 0x55));
		g_cv.setCursor(bx + 172, by + 60); g_cv.print("いいえ");
	}

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

// ── プロビジョニング(QR+PoP)表示。仕様8.2.2 ─────────────────
static void enterProv(void)
{
	char pop[9];
	for (int i = 0; i < 8; ++i) { uint32_t r = esp_random() % 36; pop[i] = (r < 10) ? char('0' + r) : char('A' + (r - 10)); }
	pop[8] = 0;
	g_pop = pop; g_provMode = true; g_dirty = true;
}

// ── BLEプロビジョニング(edgeProv)との連携(仕様8.2.2) ──
// BLEの "start" 受信時: PoP生成+QR表示。
void edgeProvShowQr(void) { enterProv(); }
// 復号できた認証情報を保存し、新しいSSID/passwordでWiFi再接続する。
void edgeProvApply(const char* name, const char* ssid, const char* pass)
{
	if (ssid == nullptr || ssid[0] == 0) { Serial.println("[PROV] empty ssid, skip"); return; }
	saveEdgeCreds(ssid, pass ? pass : "", name ? name : "");
	g_provMode = false; g_dirty = true;
	bool ok = wifiConnect::connect(g_ssid.c_str(), g_pass.c_str());
	Serial.printf("[PROV] applied creds. wifi reconnect=%d ssid=%s ip=%s\n",
	              (int)ok, g_ssid.c_str(), WiFi.localIP().toString().c_str());
}
static void renderProv(void)
{
	g_cv.fillScreen(TFT_WHITE);
	// QR内容: 端末名 + PoP(スマホはこれを読み、PoP由来鍵で暗号化してBLE送信する)。
	std::string qr = std::string("{\"n\":\"") + g_devName + "\",\"pop\":\"" + g_pop + "\"}";
	g_cv.qrcode(qr.c_str(), 76, 14, 168, 4);
	g_cv.setFont(&fonts::efontJA_16);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString("スマホでQRを読み取り設定", 160, 198);
	g_cv.drawString("(画面タップで戻る)", 160, 220);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

static void redraw(void)
{
	if (g_provMode) { renderProv(); }	// 仕様8.2: 設定(QR+PoP)表示
	else            { renderPlan(); }	// 仕様8.1: 計画名+開始/停止のみ
}

// ── タップ処理(計画リストの左アイコンで開始/停止。プロビジョニング表示中は戻る) ──
static void onTap(int x, int y)
{
	if (g_provMode) { g_provMode = false; g_dirty = true; return; }
	if (!g_confirmDelId.empty())	// 削除確認ダイアログ表示中(item4): はい/いいえ のみ受ける
	{
		if (y >= 136 && y < 166)
		{
			if (x >= 50 && x < 140)       { edgeRemoveReceivedPlan(g_confirmDelId); g_confirmDelId.clear(); }
			else if (x >= 180 && x < 270) { g_confirmDelId.clear(); g_dirty = true; }
		}
		return;
	}
	for (const auto& h : g_planHits)
	{
		if (y >= h.y0 && y < h.y1)
		{
			if (x < 48)	// 左の開始/停止アイコン領域のタップ
			{
				if (h.capturing)       { hge_captureStopPlan(h.id.c_str()); }
				else if (h.capturable) { hge_captureStartPlan(h.id.c_str()); }
				g_listDirty = true; g_dirty = true;
			}
			else if (x >= 280)	// 右端のゴミ箱: 削除確認ダイアログを出す(item4)
			{
				g_confirmDelId = h.id; g_dirty = true;
			}
			break;
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
		const char* p = std::strstr(json_, "\"state\":");	// {"planId":..,"state":d} 形式に対応
		if (p) { std::sscanf(p, "\"state\":%d", &s); }
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
	g_listDirty = true;	// 状態/計画変化を計画リストへ反映(状態列・撮影可否アイコン)
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

	loadEdgeCreds();	// NVS から SSID/password/端末名(無ければフォールバック)
	wifiConnect::setup();
	edgeProv::begin(g_devName);	// 設定プロビジョニング BLE GATT(仕様8.2.2)

	hge_init();
	hge_setNotify(notifyCb, nullptr);
	// 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。永続化したTZで「当日」を判定。
	hge_pruneOldLogs(osclock::utcOffsetMin());
	hge_loadFixedPlan();		// 出荷時設定(dataManager)から固定撮影計画を生成
	loadRecvPlans();			// 受信済み計画id を復元(item1。再起動後も表示する)
	// 注意: 撮影再開(hge_resumeCapture)は SSDP でカメラを探すため WiFi 接続後に行う(loop内)。
	g_state = hge_getState();
	redraw();
}

void loop(void)
{
	M5.update();

	// WiFi 切断時は再接続を試みる(実機運用時に SSID/PASS を設定する)
	if (wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
	{
		Serial.printf("[WIFI] connecting to %s ...\n", g_ssid.c_str());
		bool ok = wifiConnect::connect(g_ssid.c_str(), g_pass.c_str());
		if (ok)
		{
			Serial.printf("[WIFI] connected. IP=%s RSSI=%d\n",
			              WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
			std::snprintf(g_msg, sizeof(g_msg), "WiFi %s", WiFi.localIP().toString().c_str());
			if (!g_edgeUp)
			{
				etpEdge::setup(g_devName);	// プロビジョニングで設定した端末名で検索応答する(スマホは名称で探す)
				g_edgeUp = true;
				hge_resumeCapture();		// item2: WiFi接続後に撮影再開(SSDPでカメラを探せる)
				g_state = hge_getState();
			}
			g_dirty = true;
		}
		else { Serial.printf("[WIFI] connect failed.\n"); }
	}

	// ETP サーバのポーリング(スマホからの検索/制御を処理)
	if (g_edgeUp) { etpEdge::loop(); }

	// BLE 設定プロビジョニングの保留要求処理(仕様8.2.2)
	edgeProv::loop();

	// タッチ操作(スクロール/タップ)
	handleTouch();

	// 撮影中(緑カメラ)アイコンの点滅。接続断(赤)は点灯のまま点滅させない。
	{
		static uint32_t lastBlink = 0;
		uint32_t nowMs = millis();
		if (g_state == HGE_ST_CAPTURING && (nowMs - lastBlink) >= 500)
		{
			lastBlink = nowMs; g_blinkOn = !g_blinkOn; g_dirty = true;
		}
	}

	// シリアルコマンド(検証用): 's'=開始 'x'=停止 'i'=情報 'l'=ログ 'F'=保存先 'D'=内蔵ログ削除
	if (Serial.available() > 0)
	{
		int c = Serial.read();
		if (c == 's') { hge_captureStart(); }
		else if (c == 'x') { hge_captureStop(); }
		else if (c == 'p') { enterProv(); }	// 検証用: プロビジョニングQR表示(本番はBLEから)
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
		else if (c == 'G')	// 検証用: /log 内のログファイル名一覧
		{
			auto names = osfile::logFileNames();
			Serial.printf("[LOGS] %u file(s)\n", (unsigned)names.size());
			for (auto& n : names) { Serial.printf("  %s\n", n.c_str()); }
			Serial.printf("[LOGS] end\n");
		}
		else if (c == 'R')	// 検証用: R に続けて改行までをファイル名として /log 配下を読み出す
		{
			String name = Serial.readStringUntil('\n');
			name.trim();
			std::string path = std::string("/log/") + name.c_str();
			std::string body;
			if (osfile::readAll(path, body))
			{
				Serial.printf("[LOG] %s (%u bytes)\n", path.c_str(), (unsigned)body.size());
				Serial.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
				Serial.printf("[LOG] end\n");
			}
			else { Serial.printf("[LOG] read failed: %s\n", path.c_str()); }
		}
		else if (c == 'H')	// 検証用: 直近カメラIPキャッシュを手動設定(H<ip>\n)。SSDP広告停止対策の初回シード。
		{
			String ip = Serial.readStringUntil('\n');
			ip.trim();
			bool ok = dataManager::saveCameraHost(std::string(), std::string(ip.c_str()));
			Serial.printf("[HOST] save \"*\"=%s -> %d\n", ip.c_str(), (int)ok);
		}
	}

	if (g_dirty)
	{
		g_dirty = false;
		redraw();
	}
	delay(16);
}
