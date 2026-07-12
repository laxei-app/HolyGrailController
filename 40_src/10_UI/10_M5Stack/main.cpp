// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。
// holyGrailEntity を駆動する。2系統で動作する:
//  - 単独: 固定撮影計画 + タッチの[開始]/[停止]。
//  - スマホ制御: ETP(§6)でスマホから時刻同期・計画転送・開始/停止を受ける(etpEdge)。
// UI は LCD(320x240, タッチ)。スマホと同様に撮影計画+スケジュール+撮影制御方法を
// 縦スクロール表示し、撮影制御方法をタップするとその方法の画面に切り替わる。

#include <M5Unified.h>
#include <esp_heap_caps.h>	// heap_caps_malloc_extmem_enable(malloc の PSRAM 閾値を実行時設定)
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
#include "net.h"		// 検証用: 限定サブネット :8080 バッチ探索の手動実行(zコマンド)
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
static std::string g_devName = "NoName";	// 端末名。未プロビジョニング(NVS未設定)時はこのまま=NoNameを画面先頭に表示
static bool        g_provMode = false;	// プロビジョニング表示(QR)中か
static std::string g_pop;				// 現在のPoP(乱数)

// ── ネットワークモード(仕様: 屋外でルーター無しでも運用するためエッジ自身をAP化できる) ──
//  "sta" = 既存ネットワークに参加(従来動作) / "ap" = エッジがアクセスポイント。
//  モードは NVS 保持。切替は保存後に ESP.restart() し、setup() で選んだモードを素直に立ち上げる
//  (WiFi netif の張り替え・UDP再bindの複雑さを避けるため)。
static std::string g_netMode = "sta";	// NVS設定済み機の既定。未設定機(出荷時)は loadEdgeCreds が AP へ倒す(Phase5)
static std::string g_apSsid;			// APモードのSSID(初回自動生成しNVS保存)
static std::string g_apPass;			// APモードのパスワード(初回自動生成しNVS保存)
static bool        g_apQrMode = false;	// AP参加用QR(SSID/パス)をLCD表示中か

// NVS(Preferences 名前空間 "hgc")から接続情報を読み込む。
static void loadEdgeCreds(void)
{
	bool hasSta = false, hasMode = false;
	Preferences p;
	if (p.begin("hgc", true))
	{
		String s = p.getString("ssid", ""), w = p.getString("pass", ""), n = p.getString("devname", "");
		if (s.length()) { g_ssid = s.c_str(); hasSta = true; }
		if (w.length()) { g_pass = w.c_str(); }
		if (n.length()) { g_devName = n.c_str(); }
		String nm = p.getString("netmode", ""); if (nm.length()) { g_netMode = nm.c_str(); hasMode = true; }
		String as = p.getString("apssid", "");  if (as.length()) { g_apSsid  = as.c_str(); }
		String ap = p.getString("appass", "");  if (ap.length()) { g_apPass  = ap.c_str(); }
		p.end();
	}
	// 出荷時既定=APモード(Phase5)。一度も設定されていない端末(STA資格もモード指定もNVSに無い)は、
	// 箱出しで自分のAP(HGC-Edge-xxxx+QR)を立てて屋外ルーター無しでも使えるようにする。
	// プロビジョニング済み/モード切替済みの端末はNVSの値が優先され従来どおり(開発機のSTAも維持)。
	if (!hasMode && !hasSta) { g_netMode = "ap"; }
}
// ネットワークモード("sta"/"ap")をNVSへ保存する。切替後は ESP.restart() で反映する。
static void saveNetMode(const char* mode)
{
	g_netMode = mode ? mode : "sta";
	Preferences p;
	if (p.begin("hgc", false)) { p.putString("netmode", g_netMode.c_str()); p.end(); }
}
// APモードのSSID/パスワードを用意する(無ければ端末固有に自動生成しNVS保存)。
// SSID=HGC-Edge-<MAC下2桁>、パス=8桁の乱数(英数)。焼き込み共有秘密を避け端末ごとに異なる。
static void ensureApCreds(void)
{
	if (!g_apSsid.empty() && !g_apPass.empty()) { return; }
	uint8_t mac[6] = {0};
	WiFi.macAddress(mac);
	char ss[24]; std::snprintf(ss, sizeof(ss), "HGC-Edge-%02X%02X", mac[4], mac[5]);
	char pw[9];
	for (int i = 0; i < 8; ++i) { uint32_t r = esp_random() % 36; pw[i] = (r < 10) ? char('0' + r) : char('A' + (r - 10)); }
	pw[8] = 0;
	g_apSsid = ss; g_apPass = pw;
	Preferences p;
	if (p.begin("hgc", false)) { p.putString("apssid", g_apSsid.c_str()); p.putString("appass", g_apPass.c_str()); p.end(); }
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

// 名前ビットマップの永続化パス(/asset/nbmp_<id>.bin。id は yyyyMMdd-HHmmss で安全)。
static std::string nameBmpPath(const std::string& id) { return osfile::dir("asset") + "/nbmp_" + id + ".bin"; }

// 生バイト width(u16LE) height(u16LE) pixels[ceil(w/8)*h] を解析して RAM(g_nameBmps)へ格納する。
// return: 格納できた(=有効なビットマップ)。永続化はしない(呼び手が判断)。
static bool applyNameBitmap(const std::string& id, const uint8_t* data, int len)
{
	if (data == nullptr || len < 4) { return false; }
	int w = data[0] | (data[1] << 8);
	int h = data[2] | (data[3] << 8);
	if (w <= 0 || h <= 0 || w > 320 || h > 120) { return false; }
	int bpr = (w + 7) / 8; int need = 4 + bpr * h;
	if (len < need) { return false; }
	uint8_t* nb = (uint8_t*)malloc((size_t)(bpr * h));
	if (!nb) { return false; }
	memcpy(nb, data + 4, (size_t)(bpr * h));
	nameBmp& slot = g_nameBmps[id];
	if (slot.px) { free(slot.px); }
	slot.px = nb; slot.w = w; slot.h = h;
	g_dirty = true;
	return true;
}

// スマホから ETP C_NAME_BMP で受信したビットマップを格納し、再起動後も表示できるよう永続化する。
// (エッジの日本語フォント撤去後、計画名は必ずこのビットマップで描くため、RAMだけだと再起動で名前が消える)
void edgeSetNameBitmap(const std::string& id, const uint8_t* data, int len)
{
	if (!applyNameBitmap(id, data, len)) { return; }
	osfile::writeAll(nameBmpPath(id), reinterpret_cast<const char*>(data), (size_t)len);	// 受信バイトをそのまま保存
}

// 起動時: 受信済み計画(g_recvPlans)の名前ビットマップをファイルから復元する。
static void loadNameBitmaps(void)
{
	for (const auto& id : g_recvPlans)
	{
		std::string body;
		if (osfile::readAll(nameBmpPath(id), body) && body.size() >= 4)
		{
			applyNameBitmap(id, reinterpret_cast<const uint8_t*>(body.data()), (int)body.size());
		}
	}
}

// 受信計画をエッジから削除する(item4)。受信リスト・プランファイル・名前ビットマップを消す。
void edgeRemoveReceivedPlan(const std::string& id)
{
	if (id.empty()) { return; }
	if (g_recvPlans.erase(id) > 0) { saveRecvPlans(); }
	hge_deletePlan(id.c_str());
	auto it = g_nameBmps.find(id);
	if (it != g_nameBmps.end()) { if (it->second.px) { free(it->second.px); } g_nameBmps.erase(it); }
	osfile::removeFile("asset", "nbmp_" + id + ".bin");	// 永続化したビットマップも削除
	g_listDirty = true; g_dirty = true;
}
// 削除確認ダイアログ対象の計画id(空=非表示)。item4。
static std::string g_confirmDelId;

static constexpr int HEAD_H   = 28;

static M5Canvas g_cv(&M5.Display);	// ダブルバッファ(ちらつき防止)

// タップ判定用: 撮影計画リストの行(画面座標 + 計画id + 状態)
struct planHit { int y0; int y1; std::string id; bool capturing; bool capturable; };
static std::vector<planHit> g_planHits;

// タップ即時反映用: 保留中の開始/停止操作。タップの瞬間にアイコンだけ先に切り替えて描画し、
// 実処理(開始=計画ファイル読込+スケジュール構築で数百ms / 停止=撮影スレッドjoinで数秒)は
// 「描き替えた次のループ」で実行する(従来はタップ処理内で同期実行し、完了まで無反応だった)。
struct pendingOp { std::string id; int kind; };		// kind: 1=開始 2=停止
static std::vector<pendingOp>       g_opQueue;		// 実行待ち(通常0〜1件。loop末尾で1件ずつ処理)
static std::map<std::string, int>   g_pendingIcon;	// 計画id → kind。renderPlan が保留アイコンを即時反映

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

static const char* eventName(int e)
{
	switch (e)
	{
	case 1: return "Start";          case 2: return "Sunset";
	case 3: return "Civil dusk";     case 4: return "Nautical dusk";
	case 5: return "Astro dusk";     case 6: return "Astro dawn";
	case 7: return "Nautical dawn";  case 8: return "Civil dawn";
	case 9: return "Sunrise";        case 10: return "Moonrise";
	case 11: return "Moonset";       case 12: return "End";
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

// ── 撮影計画画面(簡素化: 計画名 + 開始/停止のみ。仕様 8.1) ─────────────
// 計画名はスマホからモノクロ2値ビットマップで受信していればそれを、無ければ
// スケジュールJSONの名称テキストを表示する(今後の多言語対応のため §8.2.1)。
// 撮影計画リスト画面。各行に名称+開始/終了時刻と、左に開始/停止アイコン(終了>現在 の計画のみ)。
// 左アイコンをタップでその計画を開始/停止する(スマホUIと同様。指示5)。
static void renderPlan(void)
{
	const json& arr = planList();

	g_cv.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)

	// ヘッダ
	g_cv.fillRect(0, 0, 320, HEAD_H, M5.Display.color565(0x15, 0x65, 0xC0));
	g_cv.setTextColor(TFT_WHITE);
	// 画面先頭に端末名称を表示(どのエッジか区別できるように)。未定義なら "NoName"。
	g_cv.setCursor(8, 6);   g_cv.print(g_devName.empty() ? "NoName" : g_devName.c_str());
	// 右上にネットワーク状態を色分けで常時表示(スマホから発見できる状態か一目で分かる)。
	//  ONLINE(緑)=STA接続+ETP稼働 / AP(緑)=APモードで待受 / WiFi(黄)=接続直後でETP未起動 /
	//  OFFLINE(赤)=WiFi未接続(SSID誤り等でネットワークに居ない)。
	const char* stz; uint16_t stcol;
	if (wifiConnect::isApActive())          { stz = "AP";      stcol = M5.Display.color565(0x66, 0xEE, 0x66); }
	else if (WiFi.status() == WL_CONNECTED) { stz = g_edgeUp ? "ONLINE" : "WiFi"; stcol = g_edgeUp ? M5.Display.color565(0x66, 0xEE, 0x66) : M5.Display.color565(0xFF, 0xE0, 0x40); }
	else                                    { stz = "OFFLINE"; stcol = M5.Display.color565(0xFF, 0x55, 0x55); }
	g_cv.setTextColor(stcol);
	g_cv.setCursor(320 - g_cv.textWidth(stz) - 6, 6);
	g_cv.print(stz);
	g_cv.setTextColor(TFT_WHITE);

	const int rowH = 50;
	const int top  = HEAD_H;
	const int bot  = 240;
	int total = static_cast<int>(arr.size()) * rowH;
	g_maxScroll = (total > (bot - top)) ? (total - (bot - top)) : 0;
	if (g_scroll > g_maxScroll) { g_scroll = g_maxScroll; }

	g_planHits.clear();
	// リスト描画をヘッダ帯の下(top..bot)にクリップする。スクロール時に行の名称/ビットマップが
	// タイトル行へ食い込むのを防ぐ(ヘッダはこの上で既に描画済み)。
	g_cv.setClipRect(0, top, 320, bot - top);
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
		// タップ直後の即時反映: 実処理(開始/停止)はまだ完了していないが、アイコンだけ先に切り替える。
		// 開始待ち=点灯(実処理後の SEARCHING と同じ字形で切れ目なく繋がる) / 停止待ち=開始アイコンへ戻す。
		{
			auto po = g_pendingIcon.find(id);
			if (po != g_pendingIcon.end())
			{
				nocam = false; capturing = false;
				waiting = (po->second == 1);
			}
		}
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
	g_cv.clearClipRect();	// 以降のダイアログ等は全画面へ描く
	if (arr.empty())
	{
		g_cv.setTextColor(TFT_LIGHTGREY);
		g_cv.setCursor(16, top + 24); g_cv.print("No plans");
		g_cv.setCursor(16, top + 48); g_cv.print("Transfer from phone");
	}

	// 削除確認ダイアログ(item4)。はい=削除 / いいえ=取消。ボタン座標は onTap と一致させる。
	if (!g_confirmDelId.empty())
	{
		int bx = 30, by = 84;
		g_cv.fillRoundRect(bx, by, 260, 92, 6, M5.Display.color565(0x22, 0x22, 0x22));
		g_cv.drawRoundRect(bx, by, 260, 92, 6, TFT_WHITE);
		g_cv.setTextColor(TFT_WHITE);
		g_cv.setCursor(bx + 16, by + 14); g_cv.print("Delete?");
		g_cv.fillRoundRect(bx + 20,  by + 52, 90, 30, 4, M5.Display.color565(0xCC, 0x44, 0x44));
		g_cv.setCursor(bx + 50,  by + 60); g_cv.print("Yes");
		g_cv.fillRoundRect(bx + 150, by + 52, 90, 30, 4, M5.Display.color565(0x55, 0x55, 0x55));
		g_cv.setCursor(bx + 172, by + 60); g_cv.print("No");
	}

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
// 復号できた設定を保存しネットワークへ反映する。mode="ap"=エッジ自身がAP / それ以外=STA参加。
// AP/STA切替は保存→ESP.restart()で反映(setup()が選んだモードを素直に立ち上げ直す。統一)。
// STA資格(自宅ルーターのSSID/パス)は BLE の PoP+AES-GCM 経路でのみ届く=盗聴に強い。
void edgeProvApply(const char* name, const char* ssid, const char* pass, const char* mode)
{
	std::string m = (mode && mode[0]) ? mode : "sta";
	if (name && name[0]) { g_devName = name; }	// 端末名は共通で更新
	if (m == "ap")
	{
		// APモードへ: SSID/passは受け取らない(エッジ自身のAP資格を使う)。端末名だけ保存し netmode=ap で再起動。
		Preferences p; if (p.begin("hgc", false)) { p.putString("devname", g_devName.c_str()); p.end(); }
		saveNetMode("ap");
		Serial.println("[PROV] mode=ap -> restart into AP");
		delay(200); ESP.restart();
		return;
	}
	// STAモードへ: 受信したSSID/passで参加。
	if (ssid == nullptr || ssid[0] == 0) { Serial.println("[PROV] empty ssid, skip"); return; }
	saveEdgeCreds(ssid, pass ? pass : "", g_devName);
	saveNetMode("sta");
	Serial.printf("[PROV] mode=sta ssid=%s -> restart into STA\n", g_ssid.c_str());
	delay(200); ESP.restart();
}
static void renderProv(void)
{
	g_cv.fillScreen(TFT_WHITE);
	// QR内容: 端末名 + PoP(スマホはこれを読み、PoP由来鍵で暗号化してBLE送信する)。
	std::string qr = std::string("{\"n\":\"") + g_devName + "\",\"pop\":\"" + g_pop + "\"}";
	g_cv.qrcode(qr.c_str(), 76, 14, 168, 4);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString("Scan this QR with the phone", 160, 198);
	g_cv.drawString("(tap screen to go back)", 160, 220);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

// ── APモード: 参加用QR(標準 WIFI: 形式)。スマホはスキャンで即参加、カメラは手設定 ──
static void renderApQr(void)
{
	g_cv.fillScreen(TFT_WHITE);
	// WIFI:T:WPA;S:<ssid>;P:<pass>;; (SSID/パスは英数のみなのでエスケープ不要)
	std::string qr = std::string("WIFI:T:WPA;S:") + g_apSsid + ";P:" + g_apPass + ";;";
	g_cv.qrcode(qr.c_str(), 76, 6, 168, 4);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString("AP mode: connect to this AP", 160, 184);
	g_cv.drawString((g_apSsid + " / " + g_apPass).c_str(), 160, 204);
	g_cv.drawString("(tap screen for plans)", 160, 224);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

// APモードでWiFi(SoftAP)とETPを立ち上げる。setup()/切替後の起動で呼ぶ。
static void startApAndEtp(void)
{
	ensureApCreds();
	// 同時接続上限=ESP32 SoftAPの最大(10)。スマホ+カメラ複数台+2台目エッジ(Edje01)+再接続churn分の余裕を確保。
	// 4だと「スマホ+R10+R100+Edje01=4」でちょうど埋まり、再接続時に弾かれてEdje01がAPに入れなくなる不具合が出た。
	if (wifiConnect::startAp(g_apSsid.c_str(), g_apPass.c_str(), 10))
	{
		Serial.printf("[AP] SoftAP up ssid=%s pass=%s ip=%s\n",
		              g_apSsid.c_str(), g_apPass.c_str(), wifiConnect::apIp().c_str());
		etpEdge::setup(g_devName);	// スマホは 192.168.4.1 のエッジへ(探索応答IPはAP対応済)
		g_edgeUp = true;
		g_apQrMode = true;			// LCDに参加用QR
		hge_resumeCapture();		// AP参加済みカメラがあれば撮影再開(Phase2でstation列挙発見)
		g_state = hge_getState();
	}
	else { Serial.println("[AP] softAP start FAILED"); }
}

static void redraw(void)
{
	if (g_apQrMode) { renderApQr(); }	// APモード: 参加用QR
	else if (g_provMode) { renderProv(); }	// 仕様8.2: 設定(QR+PoP)表示
	else            { renderPlan(); }	// 仕様8.1: 計画名+開始/停止のみ
}

// ── タップ処理(計画リストの左アイコンで開始/停止。プロビジョニング表示中は戻る) ──
static void onTap(int x, int y)
{
	if (g_apQrMode)  { g_apQrMode = false; g_dirty = true; return; }	// APモード: QRを閉じて計画画面へ
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
				// 即時反映: アイコンだけ先に切り替えて描画し、実処理は loop 末尾で行う
				// (開始は計画読込で数百ms・停止はスレッドjoinで数秒ブロックするため、先に描く)。
				// 同じ計画の処理待ち中の連打は無視する。
				if (g_pendingIcon.count(h.id) == 0)
				{
					int kind = h.capturing ? 2 : (h.capturable ? 1 : 0);
					if (kind != 0)
					{
						g_pendingIcon[h.id] = kind;
						g_opQueue.push_back({ h.id, kind });
						g_dirty = true;
					}
				}
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
		if (moved)
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

	// ①内部DRAM節約: malloc の PSRAM 振り分け閾値を実行時に下げる。512B超の確保(ライブビュー生文字列
	// ~14KB・計画JSON・CCAPIパースの中〜大確保等)を PSRAM へ載せ、内部DRAMを空ける。tiny確保(<=512B)は
	// 内部のまま高速維持。CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL のランタイム版でフレームワーク再ビルド不要。
	// 2カメラで minFree≒388B まで落ちる測光スパイクを緩和し、同時制御カメラ数を増やす下地にする。
	heap_caps_malloc_extmem_enable(512);

	g_cv.setPsram(true);
	g_cv.setColorDepth(16);
	g_cv.createSprite(320, 240);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)

	loadEdgeCreds();	// NVS から SSID/password/端末名(無ければフォールバック。完全未設定=出荷時はAP既定)
	Serial.printf("[NET] mode=%s devname=%s\n", g_netMode.c_str(), g_devName.c_str());
	wifiConnect::setup();
	edgeProv::begin(g_devName);	// 設定プロビジョニング BLE GATT(仕様8.2.2)

	hge_init();
	hge_setNotify(notifyCb, nullptr);
	// 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。永続化したTZで「当日」を判定。
	hge_pruneOldLogs(osclock::utcOffsetMin());
	hge_loadFixedPlan();		// 出荷時設定(dataManager)から固定撮影計画を生成
	loadRecvPlans();			// 受信済み計画id を復元(item1。再起動後も表示する)
	loadNameBitmaps();			// 受信済み計画の名前ビットマップを復元(再起動後も計画名を表示するため)
	// 注意: STA時の撮影再開(hge_resumeCapture)は カメラを探すため WiFi 接続後に行う(loop内)。
	if (g_netMode == "ap") { startApAndEtp(); }	// APモード: この時点でSoftAP+ETP+QRを立ち上げる
	g_state = hge_getState();
	redraw();
}

void loop(void)
{
	M5.update();

	// WiFi 切断時は再接続を試みる(実機運用時に SSID/PASS を設定する)。APモードではSTA再接続しない。
	if (g_netMode != "ap" && wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
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

	// 遅延アームのポンプ(§7.4): 予約計画の開始スレッドを期日(窓90秒前)に生成する。毎秒1回で十分。
	{
		static uint32_t lastPump = 0;
		uint32_t nowMs = millis();
		if (nowMs - lastPump >= 1000) { lastPump = nowMs; hge_pump(); }
	}

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
		// 検証用: ネットワークモード切替。保存して再起動し、選んだモードで素直に立ち上げ直す。
		else if (c == 'A') { Serial.println("[AP] switch to AP mode, restarting..."); saveNetMode("ap");  delay(200); ESP.restart(); }
		else if (c == 'S') { Serial.println("[STA] switch to STA mode, restarting..."); saveNetMode("sta"); delay(200); ESP.restart(); }
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
		else if (c == 'N')	// 検証用: 名前ビットマップの保持状況(RAM件数 + 永続ファイル)を表示
		{
			Serial.printf("[NBMP] ram=%d recvPlans=%d\n", (int)g_nameBmps.size(), (int)g_recvPlans.size());
			for (const auto& id : g_recvPlans)
			{
				std::string body; bool ok = osfile::readAll(nameBmpPath(id), body);
				Serial.printf("  id=%s ramHas=%d file=%s(%u B)\n", id.c_str(),
				              (int)(g_nameBmps.count(id) > 0), ok ? "yes" : "no", (unsigned)body.size());
			}
		}
		else if (c == 'z')	// 検証用: 限定サブネット :8080 バッチ探索(§3.3 tier3)を手動実行し応答IPを表示
		{
			uint32_t t0 = millis();
			std::vector<std::string> hosts = net::scanSubnetPort(8080, 250, 254);
			Serial.printf("[SWEEP] :8080 hosts=%d in %lums:", (int)hosts.size(), (unsigned long)(millis() - t0));
			for (auto& h : hosts) { Serial.printf(" %s", h.c_str()); }
			Serial.println();
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
		// (廃止 2026-07-05) 'H'=直近カメラIPシードは既知IP直結の廃止に伴い削除。
	}

	if (g_dirty)
	{
		g_dirty = false;
		redraw();
	}

	// 保留中の開始/停止を実行(タップ時のアイコン切替は上の redraw で反映済み)。
	// 開始=計画読込+スケジュール構築で数百ms、停止=撮影スレッドjoinで数秒ブロックし得るが、
	// ユーザーへの応答(アイコン)は既に返っている。1ループ1件ずつ処理する。
	if (!g_opQueue.empty())
	{
		pendingOp op = g_opQueue.front();
		g_opQueue.erase(g_opQueue.begin());
		if (op.kind == 1) { hge_captureStartPlan(op.id.c_str()); }
		else              { hge_captureStopPlan(op.id.c_str()); }
		g_pendingIcon.erase(op.id);
		g_state = hge_getState();
		g_listDirty = true; g_dirty = true;	// 実状態で描き直す(開始=SEARCHING点灯で切れ目なし/停止=開始アイコン)
	}
	delay(16);
}
