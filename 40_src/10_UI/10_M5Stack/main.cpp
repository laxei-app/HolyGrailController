// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。
// holyGrailEntity を駆動する。2系統で動作する:
//  - 単独: 固定撮影計画 + タッチの[開始]/[停止]。
//  - スマホ制御: ETP(§6)でスマホから時刻同期・計画転送・開始/停止を受ける(etpEdge)。
// UI は LCD(320x240, タッチ)。スマホと同様に撮影計画+スケジュール+撮影制御方法を
// 縦スクロール表示し、撮影制御方法をタップするとその方法の画面に切り替わる。

#include <M5Unified.h>
#include "edgeVersion.h"	// 自動生成の版数(bump_version.py。2026-08-08 UI依頼)
#include <esp_heap_caps.h>	// heap_caps_malloc_extmem_enable(malloc の PSRAM 閾値を実行時設定)
#include <WiFi.h>
#include <Preferences.h>
#include <esp_random.h>
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>	// 時刻表示(time())
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
#include "etpBle.h"	// ETP の BLE 経路(受信フレームの処理と応答送信)
#include "dataManager.h"
#include "batteryLevel.h"	// バッテリ残量レベル(実測放電カーブから決めたしきい値)
#include "batteryIcon.h"	// 残量アイコンの描画
#include "edgeBoot.h"	// 起動マーカー(リセット要因)
#include "edgeHeap.h"	// 内部RAMの推移(開始/停止で戻らない量を見る)
#include "edgeApEvents.h"	// SoftAP への参加/離脱(理由コード付き)をログへ
#include "edgeBacklight.h"	// バックライト自動消灯(無操作1分。消灯中は電源LEDも消す)

// バックライトの状態(無操作1分で消灯)。実際に消す処理は blApply()。
static edgeBL::state g_bl;
// ワーカースレッド(notifyCb)からの点灯要求。実際の点灯は loop() で行う(I2C/表示を別スレッドから触らない)。
static volatile bool g_blWake = false;
#include "batteryGuard.h"	// 限界での自動シャットダウン
#include "osFile.h"
#include "osClock.h"
#include "net.h"		// 検証用: 限定サブネット :8080 バッチ探索の手動実行(zコマンド)
#include "debugOut.h"
#include "netThread.h"	// STA/APを区別しないIP列挙

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
// APの接続枠。ドライバはこの数だけ局の構造体を先に確保するので、内部RAMに効く。
//  必要数 = カメラ台数 + スマホ1 + 余裕1。
// APの接続枠。ドライバは局の構造体を必要時に確保するため、この値を減らしても
// 内部RAMはほとんど増えない(10->5 で約1〜3KB。2026-08-26 実測)。台数の余裕を優先して10のまま。
static constexpr int kApMaxConn = 10;
static std::string g_apSsid;			// APモードのSSID(初回自動生成しNVS保存)
static std::string g_apPass;			// APモードのパスワード(初回自動生成しNVS保存)
static bool        g_apInfoMode = false;	// AP参加情報(SSID/パス)をLCD表示中か

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
	// 【そのまま使えるか(2026-08-17)】SoftAP は SSID 1〜32文字・パスワード 8〜63文字でないと
	//  WiFi.softAP() が false を返し、**APが一切立たない**。画面も出ずカメラも繋がらず、
	//  BLE以外で到達できなくなる(実機で発生: 8文字未満のパスワードを設定した)。
	//  空・長すぎ・短すぎのいずれでも既定値へ作り直して、必ずAPが立つようにする。
	const size_t apSl = g_apSsid.size(), apPl = g_apPass.size();
	if (apSl >= 1 && apSl <= 32 && apPl >= 8 && apPl <= 63) { return; }
	if (apSl != 0 || apPl != 0)
	{
		Serial.printf("[AP] stored creds unusable (ssid=%u chars, pass=%u chars; need 1-32 / 8-63) -> regenerate\n",
		              (unsigned)apSl, (unsigned)apPl);
	}
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
static bool  g_colonOn = true;	// 時計の ":" 点滅(1秒周期)。撮影状態に関係なく常時トグル(#8)
static batt::guard g_batt;			// バッテリ残量の監視(表示レベル+自動シャットダウン)
static bool  g_battBlinkOn = true;	// level2(0/3)の点滅
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
	g_bl.poke(millis());	// スマホから計画が来た=人が操作している。画面を点ける
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

// 項目2: 終わった撮影計画をエッジから自動削除する。エッジは画面が小さく、終わった計画が溜まると
// 選択の邪魔になるため。撮影終了(state=IDLE)かつ撮影終了時刻が過去(capturable=false)の計画が対象。
// スマホ側には計画が残るので、エッジから消えても再送すれば使える。実行中/待機中の計画は消さない。
// 実装は planList() のキャッシュを見るだけなので軽い。loop から定期的に呼ぶ。
static void pruneFinishedPlans(bool all);

static constexpr int HEAD_H   = 28;
static constexpr int CLOCK_H  = 22;	// 最下段の時計予約帯の高さ(px)。計画表示・スクロール対象外(#8)

// ── バックライト自動消灯(2026-08-17。edgeBacklight.h の説明を参照) ──
// CoreS3 の電源LEDは AXP2101 の reg 0x69 で制御する(M5Unified の setLed は
//  ESP32-S3 では M5PaperS3 しか処理しないため自前で叩く)。Core2 と同じ扱いで
//  0x35=点灯 / 0x05=消灯。消灯中は画面も電源LEDも光らせない。
static void blApply(bool on)
{
	if (!on)
	{	// 消す前にパネルを黒で塗る。心拍で一瞬光らせたときに中身が透けず、光量も最小になる。
		//  点け直したときは g_dirty で描き直されるので、消える情報は無い。
		M5.Display.fillScreen(TFT_BLACK);
	}
	M5.Display.setBrightness(on ? 255 : 0);
}

// 消灯中の心拍(2026-08-19)。CoreS3 は電源LEDを持たない(M5Unified の setLed も
//  ESP32-S3 では M5PaperS3 しか扱わない)ので、光らせられるのはバックライトだけ。
//  最低輝度で一瞬だけ点ける。パネルは黒に塗ってあるので、見えるのは「黒画面越しの
//  ごく淡い漏れ」で、生きていることだけが伝わる。
//  kBeatBrightness は実機で決める値(2026-08-20)。1 は真っ暗闇でしか分からず、10 でも
//  見た目が変わらなかった。CoreS3 のバックライトは低い側の刻みが粗いのか、60ms の
//  一瞬では明るさの差として知覚できないかのどちらか。128 まで上げる。
//  明るすぎるようならこの値を下げる。それでも調整しきれない場合は、明るさではなく
//  点灯時間(edgeBL::kBeatMs)で加減する手がある。
static constexpr uint8_t kBeatBrightness = 128;
static void blBeat(bool on)
{
	M5.Display.setBrightness(on ? kBeatBrightness : 0);
}

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
	case HGE_ST_WAITING:   return "WAITING";	// 撮影窓の手前で待機(カメラは在り)
	case HGE_ST_NOCAMERA:  return "NOCAMERA";	// カメラが見つからない
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

// 時計が信用できるか。未設定(1970)や同期前のでたらめな時刻で計画を消してしまわないための門番。
static bool clockUsable(void)
{
	return static_cast<long long>(time(nullptr)) >= 1577836800LL;	// 2020-01-01 より前=未設定
}

// 期限切れの計画を拾う。条件は「終了が過去(capturable=false)かつ非実行(IDLE)かつ操作保留でない」。
static void collectExpiredPlans(const json& arr, std::vector<std::string>& gone)
{
	for (const auto& p : arr)
	{
		const std::string id = p.value("id", std::string());
		if (id.empty()) { continue; }
		if (p.value("capturable", false)) { continue; }			// 終了が未来 → 残す
		if (p.value("state", 0) != HGE_ST_IDLE) { continue; }	// 実行中/待機中など → 残す
		if (g_pendingIcon.count(id) > 0) { continue; }			// 操作の確定待ち → 残す
		gone.push_back(id);
	}
}

// 計画ファイルを全部読んで一覧を作る(受信リストで絞らない)。取り残されたファイルを拾うため。
static json planListAllFromFiles(void)
{
	json out = json::array();
	int32_t len = 0;
	hge_listPlansJson(nullptr, &len);
	if (len <= 0) { return out; }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_listPlansJson(buf.data(), &len) != ERR_HGC_OK) { return out; }
	json j = json::parse(buf.data(), nullptr, false);
	if (!j.is_discarded() && j.is_array()) { out = j; }
	return out;
}

// 項目2: 終わった撮影計画をエッジから自動削除する(宣言は上部)。
// 条件: 撮影終了時刻が過去(capturable=false) かつ 非実行(state=IDLE)。実行中/待機中/未検出は残す。
// 保留操作(g_pendingIcon)中の計画も触らない(押した直後の取り違えを防ぐ)。
//
// 【2026-08-14 修正】8/11 の計画がエッジに残り続けていた。原因は2つ。
//  ① capturable(=終了が未来か)は一覧を作った時点の判定で、planList() はキャッシュされる。
//     一覧を作り直す機会が無いまま日付をまたぐと、期限切れになっても capturable=true のままで
//     いつまでも消えない。→ 掃除の前に必ず取り直す。
//  ② 電源が切れている間に期限が切れた計画は、その場では消せない。起動直後に見直す機会が無く、
//     しかも起動時は時計がまだ来ていないことがある(RTC無し/同期前。実際 1970 年のidの計画が
//     残っていた)。→ **時計が使えるようになった最初の1回**で全件を見直す。
//     このときは受信リスト(g_recvPlans)で絞らず、計画ファイルを直接見る(受信リストとファイルが
//     ずれて取り残されたものも拾うため)。
//
// all=true: 計画ファイル全件が対象(起動後の1回)。false: 受信済みの計画だけ(定期)。
static void pruneFinishedPlans(bool all)
{
	// 時計が来ていないと「終了が過去か」を判断できない。信用できる時刻になるまで何もしない。
	if (!clockUsable()) { return; }

	std::vector<std::string> gone;
	if (all)
	{
		collectExpiredPlans(planListAllFromFiles(), gone);
	}
	else
	{
		g_listDirty = true;			// capturable を取り直してから見る(①)
		collectExpiredPlans(planList(), gone);
	}
	for (const auto& id : gone)
	{
		Serial.printf("[PRUNE] finished plan removed: %s\n", id.c_str());
		edgeRemoveReceivedPlan(id);		// 計画ファイル・名前ビットマップ・受信リストから削除
	}
}

// ── 撮影計画画面(簡素化: 計画名 + 開始/停止のみ。仕様 8.1) ─────────────
// 計画名はスマホからモノクロ2値ビットマップで受信していればそれを、無ければ
// スケジュールJSONの名称テキストを表示する(今後の多言語対応のため §8.2.1)。
// 現在時刻 "HH:MM"(: は g_colonOn で点滅)。時計未設定(RTC無し未受信/RTC未初期化)は "--:--"。
// 時計は「桁位置を固定」して描く(#8 再修正)。
//  内蔵フォントはプロポーショナルで数字ごとに幅が違い、さらに ":" を空白へ置き換えると
//  幅が変わるため、点滅のたび・分が変わるたびに表示位置がずれていた。
//  各文字を「最も広い数字の幅」のセルへ中央寄せで描き、点滅はコロンを描くか描かないかで行う
//  (空白への置換をやめる)。これで桁は一切動かない。
static int clockCellW(void)	// 数字1桁ぶんのセル幅(最も広い字形に合わせる)。事前に setFont しておくこと。
{
	int dw = g_cv.textWidth("-");
	for (char c = '0'; c <= '9'; ++c) { char s[2] = { c, 0 }; int w = g_cv.textWidth(s); if (w > dw) { dw = w; } }
	return dw;
}
static int clockWidth(void) { return clockCellW() * 4 + g_cv.textWidth(":"); }

// (x,y) を左上として "HH:MM" を桁位置固定で描く。時計未設定は "--:--"(点滅させない)。
static void drawClockFixed(int x, int y)
{
	const time_t now   = time(nullptr);
	const bool   unset = (static_cast<long long>(now) < 1577836800LL);	// 2020-01-01前=未設定
	char hh[3] = { '-', '-', 0 }, mm[3] = { '-', '-', 0 };
	if (!unset)
	{
		hgc::dateTime d = hgc::fromUnixUtc(static_cast<long long>(now), osclock::utcOffsetMin());
		std::snprintf(hh, sizeof(hh), "%02u", (unsigned)d.hour);
		std::snprintf(mm, sizeof(mm), "%02u", (unsigned)d.min);
	}
	const int dw   = clockCellW();
	const int colw = g_cv.textWidth(":");
	int cx = x;
	for (int i = 0; i < 2; ++i)
	{	// 時: セル内で中央寄せ
		char s[2] = { hh[i], 0 };
		g_cv.setCursor(cx + (dw - g_cv.textWidth(s)) / 2, y); g_cv.print(s); cx += dw;
	}
	if (unset || g_colonOn) { g_cv.setCursor(cx, y); g_cv.print(":"); }	// 消える側は描かないだけ(幅は固定)
	cx += colw;
	for (int i = 0; i < 2; ++i)
	{	// 分
		char s[2] = { mm[i], 0 };
		g_cv.setCursor(cx + (dw - g_cv.textWidth(s)) / 2, y); g_cv.print(s); cx += dw;
	}
}

// 最下段に時計予約帯を描く(#8)。計画表示・スクロール対象外の固定エリア。時刻は右下。
static void drawClockBand(void)
{
	const int by = 240 - CLOCK_H;
	g_cv.fillRect(0, by, 320, CLOCK_H, TFT_BLACK);	// 予約帯をクリア(スクロール内容が入らない)
	g_cv.drawFastHLine(0, by, 320, M5.Display.color565(0x33, 0x33, 0x33));	// 計画リストとの区切り線
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_LIGHTGREY);
	const int clockX = 320 - clockWidth() - 8;
	drawClockFixed(clockX, by + 4);	// 桁位置固定(点滅・時刻更新で位置が動かない)
	g_cv.setTextColor(TFT_WHITE);
	// 時刻の左にバッテリ残量アイコン(左がプラス電極)。幅は残量に依らず固定。
	//  level2(0/3)は点滅させる(描画を飛ばすだけ。位置は保持)。
	if (!(g_batt.lv == batt::level::empty && !g_battBlinkOn))
	{
		batt::drawIcon(g_cv, clockX - batt::kIconW - 6, by + 4 + (16 - batt::kIconH) / 2,
		               batt::bars(g_batt.lv), batt::iconColor(g_cv, g_batt.lv));
	}
}

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
	const int bot  = 240 - CLOCK_H;	// 最下段は時計予約帯。計画リストはここまで(#8)
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
			// 項目7: エッジ側の削除は廃止(スマホ管理へ一本化)。ゴミ箱アイコンは撤去。
			//  終わった計画は pruneFinishedPlans が自動削除する。
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
	drawClockBand();	// #8 最下段の時計(計画表示・スクロール対象外の固定帯)

	// 項目7: エッジ側の削除機能は廃止。削除確認ダイアログも撤去(計画管理はスマホから)。

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
// スマホがQRを要求した=人が目の前で操作している。消灯していたら点ける(2026-08-20)。
//  QRは画面を見せるための機能なので、暗いままでは用を成さない。
//  BLEのコールバックから呼ばれるため、点灯そのものはループへ委ねる(g_blWake)。
void edgeProvShowQr(void) { enterProv(); g_blWake = true; }
// 復号できた設定を保存しネットワークへ反映する。mode="ap"=エッジ自身がAP / それ以外=STA参加。
// AP/STA切替は保存→ESP.restart()で反映(setup()が選んだモードを素直に立ち上げ直す。統一)。
// STA資格(自宅ルーターのSSID/パス)は BLE の PoP+AES-GCM 経路でのみ届く=盗聴に強い。
void edgeProvApply(const char* name, const char* ssid, const char* pass, const char* mode)
{
	std::string m = (mode && mode[0]) ? mode : "sta";
	if (name && name[0]) { g_devName = name; }	// 端末名は共通で更新
	if (m == "ap")
	{
		// APモードへ: 端末名と AP資格を保存して netmode=ap で再起動する。
		// AP資格を受け取ったら保存する(2026-08-08)。従来は端末名だけ保存し SSID/pass を
		//  捨てていたため、APのSSID/パスワードをユーザーが決められなかった。
		//  空で送られたときは今の値を保つ。まだ何も無ければ ensureApCreds が
		//  端末固有の既定値(HGC-Edge-<MAC下2桁> / 8桁乱数)を作る。
		// 【使えない値は受け取らない(2026-08-17)】SoftAP は SSID 1〜32文字・パスワード
		//  8〜63文字でないと立たない。短いパスワードをそのまま保存すると再起動後に
		//  APが消え、画面も出ずカメラも繋がらず、BLE以外で到達できなくなる(実機で発生)。
		//  弾いたときは今の値を保つので、端末は必ず繋がる状態のまま残る。
		{
			const size_t sl = (ssid && ssid[0]) ? std::strlen(ssid) : 0;
			const size_t pl = (pass && pass[0]) ? std::strlen(pass) : 0;
			if (sl >= 1 && sl <= 32) { g_apSsid = ssid; }
			else if (sl != 0) { Serial.printf("[PROV] AP ssid rejected (%u chars, need 1-32) -> keep current\n", (unsigned)sl); }
			if (pl >= 8 && pl <= 63) { g_apPass = pass; }
			else if (pl != 0) { Serial.printf("[PROV] AP pass rejected (%u chars, need 8-63) -> keep current\n", (unsigned)pl); }
		}
		ensureApCreds();	// 片方でも空なら既定値を用意(既にあれば何もしない)
		Preferences p;
		if (p.begin("hgc", false))
		{
			p.putString("devname", g_devName.c_str());
			p.putString("apssid",  g_apSsid.c_str());
			p.putString("appass",  g_apPass.c_str());
			p.end();
		}
		saveNetMode("ap");
		Serial.println("[PROV] mode=ap -> restart into AP");
		delay(200); ESP.restart();
		return;
	}
	// STAモードへ: 受信したSSID/passで参加。
	// 【SSIDが空なら「名前だけ変更」(2026-08-08)】従来はここで何もせず戻っていたため、
	//  端末名だけ変えたいときもSSID/passの再入力を強いていた(スマホ側も必須にしていた)。
	//  空のときは今の接続先をそのまま保ち、端末名だけ更新して再起動する。
	const bool nameOnly = (ssid == nullptr || ssid[0] == 0);
	if (nameOnly)
	{
		saveEdgeCreds(g_ssid, g_pass, g_devName);	// 接続先は変えず端末名だけ保存
		saveNetMode("sta");
		Serial.printf("[PROV] mode=sta name-only name=%s (ssid kept=%s) -> restart\n", g_devName.c_str(), g_ssid.c_str());
		delay(200); ESP.restart();
		return;
	}
	saveEdgeCreds(ssid, pass ? pass : "", g_devName);
	saveNetMode("sta");
	Serial.printf("[PROV] mode=sta ssid=%s -> restart into STA\n", g_ssid.c_str());
	delay(200); ESP.restart();
}
static void renderProv(void)
{
	g_cv.fillScreen(TFT_WHITE);
	// QR内容: 端末名 + PoP(スマホはこれを読み、PoP由来鍵で暗号化してBLE送信する)。
	// QR内容(2026-08-08 拡張): 端末名 + PoP に加えて「いまのモード」と「AP資格」を載せる。
	//  スマホは1回のスキャンで現在値を入力欄へ入れ、変えなければそのまま、変えれば
	//  変えた値をエッジへ書き戻す。従来はAP資格を知る手段が無く(AP参加QRにはPoPが無い)、
	//  APのSSID/パスワードをユーザーが確認・変更できなかった。
	//  m=sta/ap, s=AP SSID, p=AP password。内容が増えるので QR は version 6 にする。
	ensureApCreds();	// まだ無ければ既定値を作ってから載せる
	std::string qr = std::string("{\"n\":\"") + g_devName
	                + "\",\"pop\":\"" + g_pop
	                + "\",\"m\":\"" + g_netMode
	                + "\",\"s\":\"" + g_apSsid
	                + "\",\"p\":\"" + g_apPass + "\"}";
	g_cv.qrcode(qr.c_str(), 76, 14, 168, 6);	// version 6: 内容が増えたため
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString("Scan this QR with the phone", 160, 198);
	g_cv.drawString("(tap screen to go back)", 160, 220);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

// ── APモード: 参加情報(SSID/パスワード)の表示 ──
// 【なぜQRを廃止したか】(2026-08-08 依頼) 参加用QRは標準 WIFI: 形式でスマホを即参加
//  させるためのものだったが、スマホとエッジの接続は将来BLEへ移す方針のため、QRで参加
//  させる必要が無くなった。カメラはそもそもQRを読めず手入力なので、要るのは「読み取れる
//  文字」だけである。画面いっぱいを文字に使えるようになり、離れていても読める大きさにした。
static void renderApInfo(void)
{
	g_cv.fillScreen(TFT_WHITE);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(エッジ表示は英語のみ)
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.drawString("AP mode: connect to this AP", 160, 22);
	// SSID/パスワードはカメラへ手入力する値なので、画面で最大の文字で出す。
	g_cv.setFont(&fonts::Font4);
	g_cv.drawString("SSID", 160, 62);
	g_cv.drawString(g_apSsid.c_str(), 160, 92);
	g_cv.drawString("password", 160, 140);
	g_cv.drawString(g_apPass.c_str(), 160, 170);
	g_cv.setFont(&fonts::Font2);
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
	// 【立たなかったら諦めない(2026-08-17)】ここで失敗すると AP が無く、参加情報の画面も出ず
	//  カメラも繋がらない。屋外ではBLE以外の到達手段が消えるので、数回やり直し、それでも
	//  駄目なら資格を端末固有の既定値へ作り直して最後にもう一度試す(必ずAPを立てる)。
	bool apUp = false;
	for (int i = 0; i < 3 && !apUp; ++i)
	{
		if (i) { delay(300); }
		apUp = wifiConnect::startAp(g_apSsid.c_str(), g_apPass.c_str(), kApMaxConn);
		if (!apUp) { Serial.printf("[AP] softAP start failed (try %d/3) ssid=%s (%u chars) pass=%u chars\n",
		                           i + 1, g_apSsid.c_str(), (unsigned)g_apSsid.size(), (unsigned)g_apPass.size()); }
	}
	if (!apUp)
	{
		g_apSsid.clear(); g_apPass.clear(); ensureApCreds();	// 既定値へ作り直す(NVSも更新)
		apUp = wifiConnect::startAp(g_apSsid.c_str(), g_apPass.c_str(), kApMaxConn);
		Serial.printf("[AP] retry with default creds ssid=%s -> %s\n", g_apSsid.c_str(), apUp ? "up" : "still FAILED");
	}
	if (apUp)
	{
		Serial.printf("[AP] SoftAP up ssid=%s pass=%s ip=%s\n",
		              g_apSsid.c_str(), g_apPass.c_str(), wifiConnect::apIp().c_str());
		etpEdge::setup(g_devName);	// スマホは 192.168.4.1 のエッジへ(探索応答IPはAP対応済)
		g_edgeUp = true;
		g_apInfoMode = true;			// LCDに参加情報(SSID/パス)
		hge_resumeCapture();		// AP参加済みカメラがあれば撮影再開(Phase2でstation列挙発見)
		hge_presenceStart();		// 項目1: エッジ自身の在否モニタ(スマホと共通)を開始(APサブネットのカメラを探す)
		g_state = hge_getState();
	}
	else { Serial.printf("[AP] softAP start FAILED ssid=%s (%u chars) pass=%u chars\n",
	                     g_apSsid.c_str(), (unsigned)g_apSsid.size(), (unsigned)g_apPass.size()); }
}

// ── 起動画面(項目8) ──────────────────────────────────────────────
// 電源投入直後は黒画面で「起動したのか分からない」ため、LCDを初期化した直後に即表示する。
// グレー地に「Holy Grail / Time Lapse」を黒文字+白縁取りで描く。
// 機種ごとに画像へ差し替えられるようフックを用意する: SPLASH_PX に RGB565 配列(SPLASH_W×SPLASH_H)を
// 与えればそれを中央に描画し、nullptr のままならテキストで描く(現状)。
static const uint16_t* SPLASH_PX = nullptr;	// 例: edgeSplashCoreS3.h を include して差し替える
static const int       SPLASH_W  = 0;
static const int       SPLASH_H  = 0;

static void renderSplash(void)
{
	if (SPLASH_PX != nullptr && SPLASH_W > 0 && SPLASH_H > 0)
	{
		g_cv.fillScreen(g_cv.color565(0x9E, 0x9E, 0x9E));
		g_cv.pushImage((320 - SPLASH_W) / 2, (240 - SPLASH_H) / 2, SPLASH_W, SPLASH_H, SPLASH_PX);
		// 版数を右下へ小さく出す(2026-08-08 UI依頼)。どのファームが載っているかを画面だけで確認できる。
		g_cv.setFont(&fonts::Font2);
		g_cv.setTextDatum(textdatum_t::bottom_right);
		g_cv.setTextColor(TFT_BLACK);
		g_cv.drawString(HGC_EDGE_VERSION, 320 - 3, 240 - 2);
		g_cv.setTextDatum(textdatum_t::top_left);
		g_cv.pushSprite(0, 0);
		return;
	}
	g_cv.fillScreen(g_cv.color565(0x9E, 0x9E, 0x9E));	// グレー地
	g_cv.setFont(&fonts::Font4);
	g_cv.setTextDatum(textdatum_t::middle_center);
	auto outlined = [&](const char* s, int cx, int cy) {
		g_cv.setTextColor(TFT_WHITE);					// 白の縁取り(8方向へ1pxずらして描く)
		for (int dx = -1; dx <= 1; ++dx)
		{
			for (int dy = -1; dy <= 1; ++dy)
			{
				if (dx != 0 || dy != 0) { g_cv.drawString(s, cx + dx, cy + dy); }
			}
		}
		g_cv.setTextColor(TFT_BLACK);					// 本体は黒文字
		g_cv.drawString(s, cx, cy);
	};
	outlined("Holy Grail", 160, 100);
	outlined("Time Lapse", 160, 140);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setFont(&fonts::Font2);
	// 版数を右下へ小さく出す(2026-08-08 UI依頼)。どのファームが載っているかを画面だけで確認できる。
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextDatum(textdatum_t::bottom_right);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.drawString(HGC_EDGE_VERSION, 320 - 3, 240 - 2);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

static void redraw(void)
{
	// AP参加QRは撮影/待機が始まったら自動で閉じ、計画・進捗を見せる。スマホから開始した場合でも
	// 画面タップを待たずに切り替わる(QRは「カメラ/スマホを参加させるまで」の初期表示)。
	if (g_apInfoMode && g_state != HGE_ST_IDLE) { g_apInfoMode = false; }
	if (g_apInfoMode) { renderApInfo(); }	// APモード: 参加情報
	else if (g_provMode) { renderProv(); }	// 仕様8.2: 設定(QR+PoP)表示
	else            { renderPlan(); }	// 仕様8.1: 計画名+開始/停止のみ
}

// ── タップ処理(計画リストの左アイコンで開始/停止。プロビジョニング表示中は戻る) ──
static void onTap(int x, int y)
{
	if (g_apInfoMode)  { g_apInfoMode = false; g_dirty = true; return; }	// APモード: 参加情報を閉じて計画画面へ
	if (g_provMode) { g_provMode = false; g_dirty = true; return; }
	// 項目7: エッジ側の削除は廃止。削除確認ダイアログ/ゴミ箱タップは撤去した。
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
			// 項目7: 右端ゴミ箱による削除は廃止。
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
	static bool ignoreGesture = false;	// #8 時計帯から始まったジェスチャは丸ごと無視
	static bool clockBand = false;	// このジェスチャが時計帯から始まったか(離したときの手動消灯用)

	auto t = M5.Touch.getDetail();
	bool pressed = t.isPressed();
	int tx = t.x, ty = t.y;

	if (pressed && !prevPressed)
	{
		// 消灯中のタッチは**点けるだけ**。そのジェスチャは丸ごと捨てる。
		//  暗い画面を手探りで触って撮影を開始/停止させてしまうのを防ぐ(ユーザー指示 2026-08-17)。
		if (g_bl.poke(millis()))
		{
			g_dirty = true;	// 点けた直後の画面を描き直す
			ignoreGesture = true; clockBand = false;
			startX = tx; startY = ty; startScroll = g_scroll; moved = false;
			prevPressed = pressed;
			return;
		}
		// #8 の帯はスクロール/タップ対象外のまま。ただしタップ(動かさず離す)だけは**手動消灯**に使う。
		ignoreGesture = (ty >= 240 - CLOCK_H);
		clockBand     = ignoreGesture;
		startX = tx; startY = ty; startScroll = g_scroll; moved = false;
	}
	if (ignoreGesture)
	{
		if (pressed && (std::abs(tx - startX) > 8 || std::abs(ty - startY) > 8)) { moved = true; }
		if (!pressed)
		{
			if (clockBand && !moved) { g_bl.off(millis()); }	// 時計帯のタップ=手動消灯
			ignoreGesture = false; clockBand = false;
		}
		prevPressed = pressed;
		return;
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
		// 時刻になって撮影が始まったら画面を点ける(ユーザー指示 2026-08-17)。
		if (s == HGE_ST_CAPTURING && g_state != HGE_ST_CAPTURING) { g_blWake = true; }
		g_state = s;
		g_dirty = true;	// 状態変化で再描画(APモードのQR自動解除・状態帯更新を確実にする)
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
	g_bl.begin(blApply, millis(), blBeat);	// 自動消灯 + 消灯中の心拍(生存確認)

	// ①内部DRAM節約: malloc の PSRAM 振り分け閾値を実行時に下げる。512B超の確保(ライブビュー生文字列
	// ~14KB・計画JSON・CCAPIパースの中〜大確保等)を PSRAM へ載せ、内部DRAMを空ける。tiny確保(<=512B)は
	// 内部のまま高速維持。CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL のランタイム版でフレームワーク再ビルド不要。
	// 2カメラで minFree≒388B まで落ちる測光スパイクを緩和し、同時制御カメラ数を増やす下地にする。
	heap_caps_malloc_extmem_enable(512);

	// 電源ボタンを取れるようにする(2026-08-20)。M5.BtnPWR は AXP2101 の割り込み状態レジスタ
	//  0x49 の PWRON 短押し/長押しビットを読んでいる(M5Unified: getPekPress)。ところが
	//  M5Unified は割り込み許可レジスタ(0x40〜0x42)をどこにも書いていないため、許可が既定で
	//  下りている個体では状態ビットが立たず、ボタンを押しても M5.BtnPWR が反応しない。
	//  0x41 の bit2(短押し)/bit3(長押し)だけを read-modify-write で立てる。ここは割り込みの
	//  有無を決めるだけのレジスタで、充電や出力には触れない。
	if (M5.Power.getType() == m5::Power_Class::pmic_t::pmic_axp2101)
	{
		const uint8_t en = M5.Power.Axp2101.readRegister8(0x41);
		M5.Power.Axp2101.writeRegister8(0x41, (uint8_t)(en | 0x0C));
	}

	g_cv.setPsram(true);
	g_cv.setColorDepth(16);
	g_cv.createSprite(320, 240);
	// 項目E: 状態アイコン(edgeIcons.h の ICON_START/CAPTURING/CAMERA_NG)は uint16 のリトルエンディアン
	// RGB565配列。CoreS3 の M5Canvas(&M5.Display)はスプライトのネイティブ格納順が StickS3 の素の
	// LGFX_Sprite と逆になり、swapBytes=false のままだと R/B が入れ替わって青系アイコンが赤味に化ける
	// (前回 false へ揃えたが CoreS3 だけ直らなかった)。CoreS3 は true にして LE配列を正しく展開する。
	// ※ 影響するのは pushImage する状態アイコンのみ。計画名は1bitモノクロ(drawPixel)、他の塗りは
	//   color565() でネイティブ生成のため swapBytes は無関係。
	g_cv.setSwapBytes(true);
	g_cv.setFont(&fonts::Font2);	// ASCII専用フォント(日本語フォントefontJA_16は撤去。エッジ表示は英語のみ)

	renderSplash();		// 項目8: 起動画面を即表示(以降の初期化中も出したまま。終わったら計画一覧へ)

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
	// 起動したことと理由を1行残す(2026-08-21)。落ちた原因を後から追えるようにする。
	//  時計が使えるようになってから呼ぶ(APモードは startApAndEtp が RTC を復元済み)。
	// 内部RAMを使う上限を 4KB から 1KB へ下げる(2026-08-23)。
	//  これを超える malloc は PSRAM へ回る。PSRAM は 8.3MB 丸々空いているのに対し、
	//  タスクスタックを置ける内部RAM は確立時に 18KB まで細るため。
	//  ログより先に呼ぶ(以降の確保を全部対象にしたい)。
	edgeHeap::useExternalAbove(256);
	edgeApEvents::start();	// APの参加/離脱を記録(3台目が切れる件の切り分け)
	edgeBoot::logMarker(g_netMode.c_str(), hge_version());
	g_state = hge_getState();
	redraw();
}

// ── バッテリ残量ログ(放電カーブの実測用) ──
// 単独動作(USB非接続)で電池が切れるまでの推移をログに残す。残量表示のしきい値や
// 「自ら電源を切る」限界電圧は、このログの実測から決める(推測で決めない)。
//  ・up= は起動からの経過秒。途中で再起動すると 0 に戻るので、電池切れ以外の再起動を見分けられる。
//  ・osfile::append は都度追記なので、電源が落ちても直前の行までは残る。
// 60秒間隔なら 10時間で 600行 = 数十KB程度でログ領域を圧迫しない。
static constexpr uint32_t kBattLogIntervalMs = 300000;	// 5分ごと(2026-08-17。60秒だと13時間で51KB。放電カーブは5分刻みで足りる)
static void logBatteryPeriodic(void)
{
	static uint32_t last = 0;
	const uint32_t nowMs = millis();
	if (last != 0 && (nowMs - last) < kBattLogIntervalMs) { return; }
	last = nowMs;
	const int pct  = (int)M5.Power.getBatteryLevel();
	const int volt = (int)M5.Power.getBatteryVoltage();
	char d[96];
	std::snprintf(d, sizeof(d), "pct=%d volt=%dmV chg=%d up=%lus",
	              pct, volt, (int)M5.Power.isCharging(), (unsigned long)(nowMs / 1000));
	dataManager::logEvent("BATT", d);

	// 残量レベルの更新(ヒステリシス付き)。変化したら再描画する。
	const batt::level prev = g_batt.lv;
	if (g_batt.update(volt, nowMs))	// true = 限界に達して電源断シーケンス開始
	{
		batt::beginShutdown(volt, pct);	// ログ + 全セッションを✖にしてスマホに拾わせる
		g_dirty = true;
	}
	if (g_batt.lv != prev) { g_dirty = true; }
}

void loop(void)
{
	M5.update();
	logBatteryPeriodic();	// 残量ログ(60秒ごと)+ レベル更新 + 電源断シーケンスの開始
	// 電源断: スマホのポーリング1周期ぶん待ってから切る(✖を1回拾わせるため)。
	if (g_batt.readyToPowerOff(millis()))
	{
		dataManager::logEvent("PWROFF", "power off now", true);
		delay(50);				// ログの書き込みを確実に落とす
		M5.Power.powerOff();
		for (;;) { delay(1000); }	// powerOff が戻る機種でも先へ進ませない
	}

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
				hge_presenceStart();		// 項目1: エッジ自身の在否モニタ(スマホと共通の仕組み)を開始。未検出は×で示す
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
		if (nowMs - lastPump >= 1000) { lastPump = nowMs; hge_pump(); edgeHeap::pump((int)g_state); }
	}

	// 項目2: 終わった撮影計画をエッジから自動削除する。
	//  ・起動後、時計が使えるようになった最初の1回だけ全件(受信リストに無いファイルも含む)を見直す。
	//    電源が切れている間に期限切れになった計画は、その場では消せないのでここで拾う。
	//  ・以後は30秒毎。capturable は一覧を作った時点の判定なので、毎回取り直してから見る。
	{
		static uint32_t lastPrune  = 0;
		static bool     bootPruned = false;
		uint32_t nowMs = millis();
		if (!bootPruned && clockUsable()) { bootPruned = true; lastPrune = nowMs; pruneFinishedPlans(true); }
		else if (nowMs - lastPrune >= 30000) { lastPrune = nowMs; pruneFinishedPlans(false); }
	}

	// BLE 設定プロビジョニングの保留要求処理(仕様8.2.2)
	edgeProv::loop();
	etpBle::loop();		// BLE で届いた ETP フレームを処理する(TCP と同じハンドラ)

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

	// #8 時計の ":" 点滅(1秒周期=500msでトグル)+分の進行。撮影状態に関係なく常時。
	// QR/プロビジョニング表示中は計画画面ではないので更新しない。
	{
		static uint32_t lastColon = 0;
		uint32_t nowMs = millis();
		if (!g_apInfoMode && !g_provMode && (nowMs - lastColon) >= 500)
		{
			lastColon = nowMs; g_colonOn = !g_colonOn;
			// バッテリ残りわずか(level2)のアイコン点滅。1秒周期にするため ":" 2回ぶんで1トグル。
			static int half = 0;
			if (++half >= 2) { half = 0; g_battBlinkOn = !g_battBlinkOn; }
			g_dirty = true;
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
		// 保守用: STA の接続先をファーム既定(このファイル冒頭の WIFI_SSID/WIFI_PASS)へ戻す。
		//  NVS の ssid が AP 側の名前で上書きされてしまうと STA モードでどこにも繋がらなくなる
		//  (実機で発生: SSID=EDGE00 を探し続ける)。BLEプロビジョニングにはQRの読み取りが要り
		//  遠隔では復旧できないので、シリアルから既定へ戻せる口を用意する。端末名は変えない。
		else if (c == 'W')
		{
			saveEdgeCreds(WIFI_SSID, WIFI_PASS, g_devName);
			Serial.printf("[STA] creds reset to default ssid=%s (name=%s). restarting...\n",
			              WIFI_SSID, g_devName.c_str());
			delay(200); ESP.restart();
		}
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
			{	// IPは STA/AP を区別せず net の列挙をそのまま出す(モードで場合分けしない)
				std::string ips; for (const auto& s : netThread::getLocalIpList()) { ips += s + " "; }
				Serial.printf("[INFO] state=%s netmode=%s IP=%s\n",
				              stName(g_state), g_netMode.c_str(), ips.empty() ? "(none)" : ips.c_str());
			}
		}
		else if (c == 'T')	// 検証用: SDカードの読み書き自己診断(挿し替えたカードの合否判定)
		{
			// 一晩ぶん(約1MB)を実際に書いて読み戻し、1バイトずつ照合する。マウントできても
			// 書き続けると落ちるカードがあるため(2026-08-09 の 2GB SD)、量と時間で確かめる。
			// 1レコード=撮影ログ1行と同じ長さ(192B)にし、通しと同じ書き方(1行append+flush)で回す。
			const int  kRec   = 192;	// 1レコード長[B](SHOT/LVHIST 1行相当)
			const int  kCount = 5200;	// 行数(192B×5200 ≒ 1.0MB = 通し1回ぶん)
			std::string dir = osfile::logDir();
			if (dir.empty()) { Serial.println("[SDTEST] no logDir (not mounted)"); }
			else
			{
				unsigned long long tot = 0, usd = 0;
				if (osfile::spaceInfo(tot, usd))
				{
					Serial.printf("[SDTEST] backend=%s total=%lluMB used=%lluMB free=%lluMB\n",
					              osfile::backendName(), tot / (1024ULL * 1024ULL), usd / (1024ULL * 1024ULL),
					              (tot > usd ? (tot - usd) : 0ULL) / (1024ULL * 1024ULL));
				}
				const std::string path = dir + "/sdtest.tmp";
				osfile::removeFile("log", "sdtest.tmp");	// 前回の残りを消してから
				
				// --- 書き込み ---
				char rec[kRec + 1];
				uint32_t t0 = millis();
				int  ngWrite = 0, firstNg = -1;
				uint32_t worstMs = 0; int worstAt = -1;
				for (int i = 0; i < kCount; ++i)
				{
					// 行番号を埋め込む。読み戻しでズレや欠落があれば行番号で場所が分かる。
					int n = std::snprintf(rec, sizeof(rec), "%06d|", i);
					while (n < kRec - 1) { rec[n] = static_cast<char>('A' + ((i + n) % 26)); ++n; }
					rec[kRec - 1] = '\n'; rec[kRec] = '\0';
					uint32_t a = millis();
					if (!osfile::append(path, rec, kRec)) { ++ngWrite; if (firstNg < 0) { firstNg = i; } }
					uint32_t d = millis() - a;
					if (d > worstMs) { worstMs = d; worstAt = i; }
					if ((i % 1000) == 999) { Serial.printf("[SDTEST] write %d/%d\n", i + 1, kCount); }
					delay(0);	// WDT対策(他タスクへ譲る)
				}
				uint32_t wrMs = millis() - t0;
				
				// --- 読み戻して照合 ---
				uint32_t t1 = millis();
				int  ngRead = 0, badAt = -1, got = 0;
				for (int i = 0; i < kCount; ++i)
				{
					std::string out;
					if (!osfile::readRange(path, static_cast<size_t>(i) * kRec, kRec, out) ||
					    out.size() != static_cast<size_t>(kRec))
					{ ++ngRead; if (badAt < 0) { badAt = i; } continue; }
					++got;
					int n = std::snprintf(rec, sizeof(rec), "%06d|", i);
					while (n < kRec - 1) { rec[n] = static_cast<char>('A' + ((i + n) % 26)); ++n; }
					rec[kRec - 1] = '\n';
					if (std::memcmp(out.data(), rec, kRec) != 0) { ++ngRead; if (badAt < 0) { badAt = i; } }
					delay(0);
				}
				uint32_t rdMs = millis() - t1;
				
				const unsigned long kb = static_cast<unsigned long>((static_cast<long>(kCount) * kRec) / 1024);
				Serial.printf("[SDTEST] write %lukB %ums (%.0fkB/s) fail=%d(first=%d) worstLine=%ums(line %d)\n",
				              kb, wrMs, wrMs ? (kb * 1000.0 / wrMs) : 0.0, ngWrite, firstNg, worstMs, worstAt);
				Serial.printf("[SDTEST] read %lukB %ums (%.0fkB/s) mismatch=%d(first=%d) lines=%d/%d\n",
				              kb, rdMs, rdMs ? (kb * 1000.0 / rdMs) : 0.0, ngRead, badAt, got, kCount);
				Serial.printf("[SDTEST] verdict: %s\n", (ngWrite == 0 && ngRead == 0) ? "PASS" : "FAIL");
				osfile::removeFile("log", "sdtest.tmp");
			}
		}
		else if (c == 'F')	// 検証用: 現在のログ保存先(SD/LittleFS)を表示
		{
			Serial.printf("[FS] backend=%s\n", osfile::backendName());
		}
		else if (c == 'R')	// 保守用: 旧形式(テキスト)の撮影レポートを削除する
		{
			// 撮影レポートは 2026-08-05 に JSON へ移行した。旧 .txt はスマホの回収対象外
			// (C_REPORT_* は .json のみ扱う)なので、エッジに残ったままになる。消す手段がここだけ。
			int n = 0, ng = 0;
			for (const auto& nm : osfile::listFiles("log", "report_", ".txt"))
			{
				if (osfile::removeFile("log", nm)) { ++n; Serial.printf("[REPT] removed %s\n", nm.c_str()); }
				else                               { ++ng; Serial.printf("[REPT] FAILED  %s\n", nm.c_str()); }
			}
			Serial.printf("[REPT] old(.txt) removed=%d failed=%d\n", n, ng);
		}
		else if (c == 'b')	// 検証用: 電源(バッテリ)の読み値を確認する
		{
			Serial.printf("[BATT] pct=%d volt=%dmV chg=%d\n",
			              (int)M5.Power.getBatteryLevel(),
			              (int)M5.Power.getBatteryVoltage(),
			              (int)M5.Power.isCharging());
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
		else if (c == 'P')	// 診断: 電源ボタンが取れているか(押しながら数回叩いて見る)
		{
			uint8_t irqEn = 0, irqSt = 0;
			if (M5.Power.getType() == m5::Power_Class::pmic_t::pmic_axp2101)
			{
				irqEn = M5.Power.Axp2101.readRegister8(0x41);
				irqSt = M5.Power.Axp2101.readRegister8(0x49);	// 読むだけ(クリアしない)
			}
			Serial.printf("[PWRBTN] irqEn(0x41)=0x%02X irqSt(0x49)=0x%02X pressed=%d wasPressed=%d clicked=%d hold=%d\n",
			              irqEn, irqSt, (int)M5.BtnPWR.isPressed(), (int)M5.BtnPWR.wasPressed(),
			              (int)M5.BtnPWR.wasClicked(), (int)M5.BtnPWR.wasHold());
		}
		else if (c == 'n')	// 診断: 自分のAPに繋がっている局のIP一覧(DHCPの貸出先)
		{	// カメラがAPに居るのにSSDPへ答えないのか、そもそも居ないのかを切り分ける。
			auto ips = netThread::neighborHostIps();
			Serial.printf("[NEIGHBOR] %u client(s):", (unsigned)ips.size());
			for (auto& h : ips) { Serial.printf(" %s", h.c_str()); }
			Serial.println();
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
		else if (c == 't')	// 診断: 今日のログの**末尾だけ**を吸い出す
		{	// ログが大きくなると 'l' の全文転送は USB-CDC が追いつかず最新側が落ちる。
			std::string path = dataManager::currentLogPath();
			std::string body;
			if (osfile::readAll(path, body))
			{
				const size_t kTail = 12288;
				const size_t from  = (body.size() > kTail) ? (body.size() - kTail) : 0;
				Serial.printf("[TAIL] %s (%u of %u bytes)\n", path.c_str(), (unsigned)(body.size() - from), (unsigned)body.size());
				Serial.write(reinterpret_cast<const uint8_t*>(body.data() + from), body.size() - from);
				Serial.printf("[TAIL] end\n");
			}
			else { Serial.printf("[TAIL] read failed: %s\n", path.c_str()); }
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

	// バックライト: 通知スレッドからの点灯要求を反映し、無操作が続いたら消す。
	// 電源ボタンを押したら点ける(2026-08-20)。電源を切るのは長押しだが、画面が真っ暗だと
	//  いつまで押していればよいのか分からない。押した時点で表示を戻して手掛かりにする。
	//  CoreS3 の電源ボタンは AXP2101 の PEK なので M5.BtnPWR で取れる(M5.update() が更新する)。
	//  点けるだけで、他の操作は起こさない(消灯中のタッチと同じ扱い)。
	//  短押しは一瞬で終わるので isPressed だけでは取りこぼす(2026-08-20)。押し始め・クリック・
	//  長押しのどれでも拾う。電源を切るための長押しでも、押し始めた時点で点く。
	if (M5.BtnPWR.isPressed() || M5.BtnPWR.wasPressed() ||
	    M5.BtnPWR.wasClicked() || M5.BtnPWR.wasHold()) { g_blWake = true; }
	if (g_blWake) { g_blWake = false; if (g_bl.poke(millis())) { g_dirty = true; } }
	g_bl.update(millis());

	if (g_dirty && g_bl.isOn())	// 消灯中は描かない(点けたときに描き直す)
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
