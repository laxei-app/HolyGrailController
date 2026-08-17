// HolyGrail Controller エッジ端末(M5StickC S3 / StickS3) アプリ。
// holyGrailEntity を駆動し、スマホからの ETP(§6)制御・BLE設定(§8.2)を受ける。
// CoreS3版(10_M5Stack)とロジックは共通だが、UIが異なる:
//   ・LCD は小さく(240x135 横向き)タッチ非対応。物理ボタン2つで操作する。
//   ・KEY2(側面/GPIO12)= 計画スクロール(1方向ローリング、末尾で先頭へ戻る)。
//   ・KEY1(正面/GPIO11)= 表示中の計画を開始/停止(項目7: エッジ側削除は廃止=長押し削除なし)。
//   ・端末名称 + 1つの撮影計画(名称は2段折り返し + 日付/時刻)だけ表示する。
// etpEdge.cpp / edgeProv.cpp は表示非依存なので CoreS3版を共有ビルドする(build_src_filter)。

#include <M5Unified.h>
#include "edgeVersion.h"	// 自動生成の版数(bump_version.py。2026-08-08 UI依頼)
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>	// ST7789 パネル型を明示include
#include <WiFi.h>
#include <Preferences.h>
#include <esp_random.h>
#include <driver/gpio.h>
#include <driver/i2c.h>	// I2C_NUM_1(M5PM1 の LCD電源ON を lgfx::i2c で叩くため)
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
#include "holyGrailEntity.h"
#include "errorCode.h"
#include "WiFi_Connect.h"
#include "etpEdge.h"
#include "edgeProv.h"
#include "etpBle.h"	// ETP の BLE 経路(受信フレームの処理と応答送信)
#include "edgeIcons.h"	// CoreS3 と共有する状態アイコン(ICON_START/CAPTURING/CAMERA_NG)
#include "dataManager.h"
#include "batteryLevel.h"	// バッテリ残量レベル(実測放電カーブから決めたしきい値)
#include "batteryIcon.h"	// 残量アイコンの描画
#include "edgeBacklight.h"	// バックライト自動消灯(無操作1分。消灯中は電源LEDも消す)

// バックライトの状態(無操作1分で消灯)。実際に消す処理は blApply()。
static edgeBL::state g_bl;
// ワーカースレッド(notifyCb)からの点灯要求。実際の点灯は loop() で行う(I2C/表示を別スレッドから触らない)。
static volatile bool g_blWake = false;
#include "batteryGuard.h"	// 限界での自動シャットダウン
#include "osFile.h"
#include "osClock.h"
#include "debugOut.h"

using json = nlohmann::json;

// loopTask のスタック拡張(天文計算の再帰でオーバーフローするため。CoreS3版と同じ理由)。
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ── StickS3 LCD(ST7789P3 135x240)を手動構成する ──
// M5.begin にボードを StickS3 と認識させると、M5 が StickS3 の電源IC(M5PM1)まで掌握して IRQ/I2C
// ポーリングを始め、WiFi/BLE 稼働時に割り込みWDT(TG1)でブートループした。そのため表示は自前で
// SPI2 に構成し、ちらつき解消に必要な「M5PM1 の LCD電源ON」だけを別途 I2C で手動実行する(m5pm1LcdPowerOn)。
// ピン: MOSI=39 SCK=40 DC=45 CS=41 RST=21 (M5 StickS3 データシート)。
class DisplayS3 : public lgfx::LGFX_Device
{
	lgfx::Panel_ST7789 _st7789;
	lgfx::Bus_SPI      _spibus;
public:
	DisplayS3()
	{
		{
			auto c = _spibus.config();
			c.spi_host   = SPI2_HOST;
			c.spi_mode   = 0;
			c.freq_write = 40000000;
			c.freq_read  = 16000000;
			c.pin_sclk   = 40;
			c.pin_mosi   = 39;
			c.pin_miso   = -1;
			c.pin_dc     = 45;
			_spibus.config(c);
			_st7789.setBus(&_spibus);
		}
		{
			auto c = _st7789.config();
			c.pin_cs        = 41;
			c.pin_rst       = 21;
			c.pin_busy      = -1;
			c.panel_width   = 135;
			c.panel_height  = 240;
			c.offset_x      = 52;
			c.offset_y      = 40;
			c.offset_rotation = 0;
			c.readable      = false;
			c.invert        = true;
			c.rgb_order     = false;
			c.dlen_16bit    = false;
			c.bus_shared    = false;
			_st7789.config(c);
		}
		setPanel(&_st7789);
	}
};

static DisplayS3        g_lcd;
// 表示はスプライト(ダブルバッファ)に描いて一度に転送する。全画面スプライト(240x135x2=64KB)は
// Arduino 3.x で有効化した PSRAM に確保する(内蔵RAMを圧迫しない)。
static lgfx::LGFX_Sprite g_cv(&g_lcd);

// StickS3 の LCD/バックライト電源は M5PM1(PMIC, I2C 0x6E, SDA=47/SCL=48)から供給される。この初期化を
// しないと LCD電源レールが不安定で「画面全体のちらつき」が出る。M5GFX の StickS3 初期化と同じ手順で
// M5PM1 の G2(=LCD Power On)を出力HIGHにし、PMIC のアイドルスリープ(reg 0x09)を無効化して安定化する。
// M5 本体には StickS3 と認識させない(ブートループ回避)ので、この1点だけを lgfx::i2c で直接叩く。
static void m5pm1LcdPowerOn(void)
{
	constexpr uint8_t  ADDR = 0x6E;
	constexpr uint32_t FRQ  = 100000;
	lgfx::i2c::init(I2C_NUM_1, GPIO_NUM_47, GPIO_NUM_48);	// SDA=47, SCL=48
	lgfx::i2c::bitOff(I2C_NUM_1, ADDR, 0x16, 1 << 2, FRQ);	// G2 を GPIO 機能に
	lgfx::i2c::bitOn (I2C_NUM_1, ADDR, 0x10, 1 << 2, FRQ);	// G2 を出力モードに
	lgfx::i2c::bitOff(I2C_NUM_1, ADDR, 0x13, 1 << 2, FRQ);	// G2 プッシュプル
	lgfx::i2c::bitOn (I2C_NUM_1, ADDR, 0x11, 1 << 2, FRQ);	// G2 出力HIGH => LCD 電源 ON
	lgfx::i2c::writeRegister8(I2C_NUM_1, ADDR, 0x09, 0x00, 0, FRQ);	// PMIC アイドルスリープ無効(安定化)
	Serial.println("[PM1] LCD power on (lgfx::i2c 47/48)");
}
static int              g_scrW = 240, g_scrH = 135;	// 横向きの論理サイズ

// ── 物理ボタン(生GPIO。M5Unified は本ボードのボタンを対応付けないため直接読む) ──
//  KEY1=GPIO11(正面): 開始/停止 / KEY2=GPIO12(側面): スクロール。押下=LOW(内部プルアップ)。
static constexpr gpio_num_t PIN_KEY1 = GPIO_NUM_11;
static constexpr gpio_num_t PIN_KEY2 = GPIO_NUM_12;
static constexpr uint32_t   LONG_PRESS_MS = 800;
// バッテリ残量ログの間隔[ms]。放電カーブを描くのに十分な粒度で、かつ書き込み負荷を抑える。
// 60秒間隔なら 10時間で 600行 = 数十KB程度で LittleFS を圧迫しない。
static constexpr uint32_t   kBattLogIntervalMs = 60000;
// バッテリ残量の監視(表示レベル + 限界での自動シャットダウン)。判定は batteryLevel.h。
static batt::guard          g_batt;
static bool                 g_battBlinkOn = true;	// level2(0/3)の点滅。時計の":"と同じ1秒周期

struct Button
{
	gpio_num_t pin;
	bool       prev = false;		// 前回押下状態
	uint32_t   downAt = 0;			// 押し始め時刻
	bool       longFired = false;	// 長押しイベント発火済み
	void begin(gpio_num_t p)
	{
		pin = p;
		gpio_config_t cfg = {};
		cfg.pin_bit_mask = (1ULL << pin);
		cfg.mode = GPIO_MODE_INPUT;
		cfg.pull_up_en = GPIO_PULLUP_ENABLE;
		cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
		cfg.intr_type = GPIO_INTR_DISABLE;
		gpio_config(&cfg);
	}
	bool pressedNow() const { return gpio_get_level(pin) == 0; }	// アクティブLOW
	// update(): 戻り値ビット 1=短押し(離した瞬間) 2=長押し(押しっぱなしで閾値超え)。
	int update(uint32_t now)
	{
		int ev = 0;
		bool p = pressedNow();
		if (p && !prev) { downAt = now; longFired = false; }
		if (p && prev && !longFired && (now - downAt) >= LONG_PRESS_MS) { longFired = true; ev |= 2; }
		if (!p && prev && !longFired && (now - downAt) >= 30) { ev |= 1; }	// 30ms以上でチャタ除去
		prev = p;
		return ev;
	}
};
static Button g_key1, g_key2;

// ── バックライト/電源LEDの実体(2026-08-17。edgeBacklight.h の説明を参照) ──
// バックライトenable は G38。電源LEDは M5PM1(0x6E) の PWR_CFG(0x06) bit4=LED CONTROL で、
//  データシートどおり **1=点灯 / 0=消灯**(2026-08-17 実機確認)。
//  起動直後は bit4=0 なのに点灯して見えるが、これは PMIC 側の LED 自己制御ロジック
//  (リセット時に1回光る等)によるもので、bit4 の意味とは別物。ここから推測してはいけない。
//  bitOn/bitOff は read-modify-write なので、同じレジスタにある 5V昇圧/3.3V LDO/
//  3.3V DCDC/充電許可のビットには触れない。
static void blApply(bool on)
{
	gpio_set_level(GPIO_NUM_38, on ? 1 : 0);
	if (on) { M5.In_I2C.bitOn (0x6E, 0x06, 1 << 4, 100000); }	// LED点灯
	else    { M5.In_I2C.bitOff(0x6E, 0x06, 1 << 4, 100000); }	// LED消灯
}

// ── NVS 接続情報(CoreS3版と同一。§8.2.1) ──
static const char* WIFI_SSID = "Buffalo-G-D850";
static const char* WIFI_PASS = "rnhcftfbk75tf";
static std::string g_ssid    = WIFI_SSID;
static std::string g_pass    = WIFI_PASS;
static std::string g_devName = "NoName";
static bool        g_provMode = false;
static std::string g_pop;
static std::string g_netMode = "sta";
static std::string g_apSsid;
static std::string g_apPass;
static bool        g_apInfoMode = false;

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
static void saveNetMode(const char* mode)
{
	g_netMode = mode ? mode : "sta";
	Preferences p;
	if (p.begin("hgc", false)) { p.putString("netmode", g_netMode.c_str()); p.end(); }
}
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
const std::string& edgePop(void) { return g_pop; }

// ── 表示状態 ──
static int  g_cur = 0;			// 表示中の計画インデックス(スクロールで進める)
static bool g_listDirty = true;
static volatile int g_state = HGE_ST_IDLE;
static bool g_blinkOn = true;
static bool g_colonOn = true;	// 時計の ":" 点滅(1秒周期)。撮影状態に関係なく常時トグル

// KEY1即時反映用: 保留中の開始/停止操作。押した瞬間にアイコンだけ先に切り替えて描画し、
// 実処理(開始=計画ファイル読込+スケジュール構築で数百ms / 停止=撮影スレッドjoinで数秒)は
// 「描き替えた次のループ」で実行する(従来はキー処理内で同期実行し、完了まで無反応だった)。
struct pendingOp { std::string id; int kind; };		// kind: 1=開始 2=停止
static std::vector<pendingOp>     g_opQueue;		// 実行待ち(通常0〜1件。loop末尾で1件ずつ処理)
static std::map<std::string, int> g_pendingIcon;	// 計画id → kind。renderPlan が保留アイコンを即時反映
static bool g_dirty = true;
static bool g_edgeUp = false;
static bool     g_spriteOk = false;		// createSprite 成否
static bool     g_bandDirty = false;	// 状態帯(下部)だけの部分更新。全画面転送を減らして SPI 負荷を抑える
static const int BAND_H = 58;			// 下部バンド(状態アイコン+最下段キーガイダンス/時刻)の高さ(px)

// ── 受信計画の永続化(CoreS3版と同一) ──
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

// ── 計画名ビットマップ(スマホから受信。CoreS3版と同一の永続化) ──
struct nameBmp { int w = 0, h = 0; uint8_t* px = nullptr; };
static std::map<std::string, nameBmp> g_nameBmps;
static std::string nameBmpPath(const std::string& id) { return osfile::dir("asset") + "/nbmp_" + id + ".bin"; }
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
	slot.px = nb; slot.w = w; slot.h = h; g_dirty = true;
	return true;
}
void edgeSetNameBitmap(const std::string& id, const uint8_t* data, int len)
{
	// ETP末尾の空白/NUL除去から末尾の黒画素(0x00)を守る番兵0x01を除去する(CoreS3版と同じ)。
	if (len > 0 && data[len - 1] == 0x01) { --len; }
	if (!applyNameBitmap(id, data, len)) { return; }
	osfile::writeAll(nameBmpPath(id), reinterpret_cast<const char*>(data), (size_t)len);
}
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
void edgeRemoveReceivedPlan(const std::string& id)
{
	if (id.empty()) { return; }
	if (g_recvPlans.erase(id) > 0) { saveRecvPlans(); }
	hge_deletePlan(id.c_str());
	auto it = g_nameBmps.find(id);
	if (it != g_nameBmps.end()) { if (it->second.px) { free(it->second.px); } g_nameBmps.erase(it); }
	osfile::removeFile("asset", "nbmp_" + id + ".bin");
	g_listDirty = true; g_dirty = true;
}

static const char* stName(int s)
{
	switch (s)
	{
	case HGE_ST_IDLE: return "IDLE"; case HGE_ST_SEARCHING: return "SEARCH";
	case HGE_ST_READY: return "READY"; case HGE_ST_CAPTURING: return "CAPTURING";
	case HGE_ST_STOPPING: return "STOPPING"; case HGE_ST_ERROR: return "ERROR";
	case HGE_ST_DISCONNECTED: return "DISCONN"; default: return "?";
	}
}
// "YYYY-MM-DDThh:mm:ss" → "MM/dd HH:mm"
static std::string mmddhhmm(const std::string& iso)
{
	if (iso.size() >= 16) { return iso.substr(5, 2) + "/" + iso.substr(8, 2) + " " + iso.substr(11, 5); }
	return iso;
}

// 受信した計画だけをリスト化(キャッシュ付き)。CoreS3版と同じ。
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
				{
					for (const auto& p : j)
					{
						if (g_recvPlans.count(p.value("id", std::string())) > 0) { cache.push_back(p); }
					}
				}
			}
		}
		g_listDirty = false;
		if (g_cur >= (int)cache.size()) { g_cur = 0; }	// 削除等で範囲外になったら先頭へ
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

// 項目2: 終わった撮影計画をエッジから自動削除する。エッジは画面が小さく、終わった計画が溜まると
// 選択(KEY2送り)の邪魔になるため。条件は「撮影終了時刻が過去(capturable=false)かつ非実行(IDLE)」。
// スマホ側には計画が残るので、消えても再送すれば使える。実行中/待機中/操作確定待ちの計画は残す。
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
		edgeRemoveReceivedPlan(id);
	}
}

// ── 名前ビットマップを最大幅で2段に折り返して描く。戻り値=消費した高さ[px]。 ──
static int drawNameBitmapWrapped(const nameBmp& nb, int x, int y, int maxW)
{
	const int bpr = (nb.w + 7) / 8;
	int line1W = nb.w > maxW ? maxW : nb.w;
	int line2W = nb.w > maxW ? (nb.w - maxW) : 0;
	if (line2W > maxW) { line2W = maxW; }	// 3段目以降は切り捨て(表示領域が狭いため)
	auto drawSpan = [&](int colStart, int spanW, int dy) {
		for (int yy = 0; yy < nb.h; ++yy)
			for (int xx = 0; xx < spanW; ++xx)
			{
				int col = colStart + xx;
				uint8_t byte = nb.px[yy * bpr + (col >> 3)];
				if (byte & (0x80 >> (col & 7))) { g_cv.drawPixel(x + xx, dy + yy, TFT_WHITE); }
			}
	};
	drawSpan(0, line1W, y);
	if (line2W > 0) { drawSpan(maxW, line2W, y + nb.h + 2); return nb.h * 2 + 2; }
	return nb.h;
}

// ── テキスト名を2段に折り返して描く(ASCII名/ビットマップ無し時)。戻り値=消費した高さ。 ──
static int drawNameTextWrapped(const std::string& name, int x, int y, int maxW)
{
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setFont(&fonts::Font4);	// 名称は大きめ
	// 幅に収まる文字数で1段目/2段目に分割する。
	size_t cut = name.size();
	for (size_t i = 1; i <= name.size(); ++i)
	{
		if ((int)g_cv.textWidth(name.substr(0, i).c_str()) > maxW) { cut = i - 1; break; }
	}
	std::string l1 = name.substr(0, cut);
	std::string l2 = (cut < name.size()) ? name.substr(cut) : std::string();
	if ((int)g_cv.textWidth(l2.c_str()) > maxW)	// 2段目も溢れるなら省略記号
	{
		while (l2.size() > 1 && (int)g_cv.textWidth((l2 + "..").c_str()) > maxW) { l2.pop_back(); }
		l2 += "..";
	}
	int lh = g_cv.fontHeight();
	g_cv.setCursor(x, y); g_cv.print(l1.c_str());
	if (!l2.empty()) { g_cv.setCursor(x, y + lh); g_cv.print(l2.c_str()); g_cv.setFont(&fonts::Font2); return lh * 2; }
	g_cv.setFont(&fonts::Font2);
	return lh;
}

// ── コンテンツの左マージン ──
// 旧レイアウトは画面左に KEY1/KEY2 の縦横ブロックを縦に並べていたが「かっこ悪い」ため廃止し、
// キーガイダンスは最下段(時刻と同じ行)へ矢印付きで移した(drawBottomBar)。コンテンツは全て左寄せ。
static constexpr int LEFT_MARGIN = 4;	// 端末名・計画名・日時・状態アイコンの左端

// ── 最下段キーガイダンス用の矢印(内蔵GFXフォントに矢印字形が無いため自前で描く) ──
// この横向き(rotation 3)では KEY1(正面)が画面の左、KEY2(側面)が画面の下に来るので、
// ← が KEY1(start/stop)を、↓ が KEY2(select)を指す(物理キーの位置を矢印で示す)。
static constexpr int ARROW_W = 11;	// 矢印1つの占有幅[px](矢じり+軸)
static void drawArrowLeft(int x, int cy, uint16_t col)	// ← 左向き(KEY1=画面左)
{
	g_cv.fillTriangle(x, cy, x + 5, cy - 4, x + 5, cy + 4, col);	// 矢じり(左が尖る)
	g_cv.fillRect(x + 5, cy - 1, 5, 2, col);					// 軸
}
static void drawArrowDown(int x, int cy, uint16_t col)	// ↓ 下向き(KEY2=画面下)
{
	const int cx = x + 4;
	g_cv.fillTriangle(cx, cy + 5, cx - 4, cy, cx + 4, cy, col);	// 矢じり(下が尖る)
	g_cv.fillRect(cx - 1, cy - 5, 2, 5, col);					// 軸
}

// 現在時刻 "HH:MM"(: は g_colonOn で点滅)。時計未設定(RTC無し未受信/RTC未初期化)は "--:--"。
//  システム時計は phone の time コマンド or 起動時のRTC復元でUTCがセットされる。未設定なら1970年付近。
// 時計は「桁位置を固定」して描く(#8 再修正)。
//  内蔵フォントはプロポーショナルで数字ごとに幅が違い、さらに ":" を空白へ置き換えると
//  幅が変わるため、点滅のたび・分が変わるたびに表示位置がずれていた。
//  各文字を「最も広い数字の幅」のセルへ中央寄せで描き、点滅はコロンを描くか描かないかで行う。
static int clockCellW(void)	// 数字1桁ぶんのセル幅。事前に setFont しておくこと。
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
	{
		char s[2] = { hh[i], 0 };
		g_cv.setCursor(cx + (dw - g_cv.textWidth(s)) / 2, y); g_cv.print(s); cx += dw;
	}
	if (unset || g_colonOn) { g_cv.setCursor(cx, y); g_cv.print(":"); }
	cx += colw;
	for (int i = 0; i < 2; ++i)
	{
		char s[2] = { mm[i], 0 };
		g_cv.setCursor(cx + (dw - g_cv.textWidth(s)) / 2, y); g_cv.print(s); cx += dw;
	}
}

// 最下段の1行(キーガイダンス + 現在時刻)。左から「←start/stop」「↓select」を並べ、
// 右端に現在時刻 "HH:MM"(桁位置固定・":"点滅)を描く。例: "←start/stop ↓select      12:34"。
// band(下部)更新で毎フレーム描かれる位置に置く(点滅・時刻更新もここで反映される)。
static constexpr int BOTTOM_BAR_H = 16;	// 最下段の行高(Font2)
static void drawBottomBar(void)
{
	const int rowY = g_scrH - BOTTOM_BAR_H;		// 行の上端
	const int cy   = rowY + BOTTOM_BAR_H / 2;	// 矢印の縦中心
	const uint16_t green = g_cv.color565(0x66, 0xEE, 0x66);	// 物理キーを指す明るい緑の矢印
	g_cv.fillRect(0, rowY, g_scrW, BOTTOM_BAR_H, TFT_BLACK);	// 前回描画を消す
	g_cv.setFont(&fonts::Font2);

	int x = LEFT_MARGIN;
	// KEY1(画面左)= 開始/停止
	drawArrowLeft(x, cy, green); x += ARROW_W;
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setCursor(x, rowY); g_cv.print("start/stop");
	x += g_cv.textWidth("start/stop") + 8;
	// KEY2(画面下)= 計画送り。240px幅に電池アイコンも収めるため "select"→"sel" と間隔を詰めた。
	drawArrowDown(x, cy, green); x += ARROW_W;
	g_cv.setCursor(x, rowY); g_cv.print("sel");

	// 右端に現在時刻(桁位置固定なので点滅・分更新で位置が動かない)。
	g_cv.setTextColor(TFT_LIGHTGREY);
	const int clockX = g_scrW - clockWidth() - 4;
	drawClockFixed(clockX, rowY);
	g_cv.setTextColor(TFT_WHITE);

	// 時刻の左にバッテリ残量アイコン(左がプラス電極)。幅は残量に依らず固定なので桁位置は動かない。
	//  level2(0/3)は点滅させる。点滅は「枠ごと消す」のではなく描画を飛ばす(位置は保持)。
	if (!(g_batt.lv == batt::level::empty && !g_battBlinkOn))
	{
		batt::drawIcon(g_cv, clockX - batt::kIconW - 5, rowY + (BOTTOM_BAR_H - batt::kIconH) / 2,
		               batt::bars(g_batt.lv), batt::iconColor(g_cv, g_batt.lv));
	}
}

// ── 計画画面(端末名 + 1計画: 名称2段 + 日付/時刻)。 ──
static void renderPlan(void)
{
	const json& arr = planList();
	g_cv.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::Font2);

	// ヘッダ: 端末名(左) + ネットワーク状態(右・色分け)。
	const int headH = 18;
	g_cv.fillRect(0, 0, g_scrW, headH, g_cv.color565(0x15, 0x65, 0xC0));
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setCursor(4, 2); g_cv.print(g_devName.empty() ? "NoName" : g_devName.c_str());
	const char* stz; uint16_t stcol;
	if (wifiConnect::isApActive())          { stz = "AP";      stcol = g_cv.color565(0x66, 0xEE, 0x66); }
	else if (WiFi.status() == WL_CONNECTED) { stz = g_edgeUp ? "ONLINE" : "WiFi"; stcol = g_edgeUp ? g_cv.color565(0x66, 0xEE, 0x66) : g_cv.color565(0xFF, 0xE0, 0x40); }
	else                                    { stz = "OFFLINE"; stcol = g_cv.color565(0xFF, 0x55, 0x55); }
	g_cv.setTextColor(stcol);
	g_cv.setCursor(g_scrW - g_cv.textWidth(stz) - 4, 2); g_cv.print(stz);
	g_cv.setTextColor(TFT_WHITE);

	// 最下段: キーガイダンス(←start/stop ↓select)+ 現在時刻(計画の有無に関わらず表示)。
	drawBottomBar();
	const int cx0  = LEFT_MARGIN;           // コンテンツの左端(全て左寄せ)
	const int maxW = g_scrW - cx0 - 4;      // コンテンツ幅

	if (arr.empty())
	{
		g_cv.setTextColor(TFT_LIGHTGREY);
		g_cv.setCursor(cx0, 44);  g_cv.print("No plans");
		g_cv.setCursor(cx0, 66);  g_cv.print("Send from phone");
		return;
	}
	if (g_cur < 0 || g_cur >= (int)arr.size()) { g_cur = 0; }
	const auto& p = arr[g_cur];
	std::string id   = p.value("id",    std::string());
	std::string name = p.value("name",  std::string());
	std::string st   = p.value("start", std::string());
	std::string en   = p.value("end",   std::string());
	bool capturable  = p.value("capturable", false);
	int  state       = p.value("state", 0);
	bool nocam     = (state == HGE_ST_NOCAMERA || state == HGE_ST_DISCONNECTED);
	bool capturing = (state == HGE_ST_CAPTURING || state == HGE_ST_STOPPING);
	bool waiting   = (state == HGE_ST_WAITING || state == HGE_ST_SEARCHING);
	// KEY1直後の即時反映: 実処理(開始/停止)はまだ完了していないが、アイコンだけ先に切り替える。
	// 開始待ち=点灯(実処理後の SEARCHING と同じ字形で切れ目なく繋がる) / 停止待ち=開始アイコンへ戻す。
	{
		auto po = g_pendingIcon.find(id);
		if (po != g_pendingIcon.end())
		{
			nocam = false; capturing = false;
			waiting = (po->second == 1);
		}
	}

	// 右上: 何番目/全体(スクロール位置)。
	{
		char idx[16]; std::snprintf(idx, sizeof(idx), "%d/%d", g_cur + 1, (int)arr.size());
		g_cv.setTextColor(TFT_DARKGREY);
		g_cv.setCursor(g_scrW - g_cv.textWidth(idx) - 4, headH + 2); g_cv.print(idx);
	}

	// 計画名(2段折り返し)。ビットマップがあればそれを、無ければテキストを折り返す。
	int nameY = headH + 4;
	int used = 0;
	auto it = g_nameBmps.find(id);
	if (it != g_nameBmps.end() && it->second.px) { used = drawNameBitmapWrapped(it->second, cx0, nameY, maxW); }
	else { used = drawNameTextWrapped(name, cx0, nameY, maxW); }

	// 日付/時刻(開始 - 終了を1行・左寄せ)。Start/End の見出しは付けず日付と時刻だけ。
	// 例: "07/22 13:15 - 07/23 07:00"
	int ty = nameY + used + 6;
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_LIGHTGREY);
	g_cv.setCursor(cx0, ty); g_cv.print((mmddhhmm(st) + " - " + mmddhhmm(en)).c_str());

	// 状態(下部): CoreS3 と同じ ICON_* を計画名の状態表示として描画 + テキスト + KEY1操作。
	//   撮影中(CAPTURING)=カメラ点滅 / 待機(WAITING)=カメラ点灯 / 未検出(NOCAMERA)=✖カメラ点灯 / 撮影可=開始アイコン。
	//   点滅は g_blinkOn(CAPTURING のみ)。アイコン背景はデータに焼込済(CAPTURING/NG=黒地でStickS3の黒背景に馴染む)。
	// 状態アイコン+テキストは、開始/終了時刻と最下段キーガイダンスの間に左寄せで置く。
	// アイコンは最下段バーのすぐ上に「底辺」を合わせるので、状態が変わっても縦位置が動かない。
	// KEY1の操作(start/stop)は最下段のキーガイダンスが示すので、状態テキストからは外す。
	const int iconBottom = g_scrH - BOTTOM_BAR_H - 2;	// アイコンの底辺(最下段バーの直上)
	uint16_t scol = TFT_LIGHTGREY; const char* stxt = "";
	bool hasIcon = true; int iconH = ICON_CAPTURING_H;
	if (nocam)
	{
		g_cv.pushImage(LEFT_MARGIN, iconBottom - ICON_CAMERA_NG_H, ICON_CAMERA_NG_W, ICON_CAMERA_NG_H, ICON_CAMERA_NG);
		iconH = ICON_CAMERA_NG_H; scol = g_cv.color565(0xFF, 0x55, 0x55); stxt = "NO CAMERA";
	}
	else if (capturing)
	{
		if (g_blinkOn) { g_cv.pushImage(LEFT_MARGIN, iconBottom - ICON_CAPTURING_H, ICON_CAPTURING_W, ICON_CAPTURING_H, ICON_CAPTURING); }
		iconH = ICON_CAPTURING_H; scol = g_cv.color565(0x66, 0xEE, 0x66); stxt = "CAPTURING";
	}
	else if (waiting)
	{
		g_cv.pushImage(LEFT_MARGIN, iconBottom - ICON_CAPTURING_H, ICON_CAPTURING_W, ICON_CAPTURING_H, ICON_CAPTURING);
		iconH = ICON_CAPTURING_H; scol = g_cv.color565(0x66, 0xEE, 0x66); stxt = "WAITING";
	}
	else if (capturable)
	{
		g_cv.pushImage(LEFT_MARGIN, iconBottom - ICON_START_H, ICON_START_W, ICON_START_H, ICON_START);
		iconH = ICON_START_H; scol = TFT_WHITE; stxt = "READY";
	}
	else { hasIcon = false; scol = TFT_DARKGREY; stxt = "(past)"; }
	// テキストはアイコンの右・縦中心に合わせる(アイコン無し=(past)はアイコン底辺に合わせて左端)。
	int sx = hasIcon ? (LEFT_MARGIN + ICON_START_W + 4) : LEFT_MARGIN;
	int sy = hasIcon ? (iconBottom - iconH) + (iconH - 16) / 2 : (iconBottom - 16);
	g_cv.setTextColor(scol);
	g_cv.setCursor(sx, sy); g_cv.print(stxt);
	// 項目7: エッジ側の削除機能は廃止。削除確認オーバーレイは撤去した。
}

// ── 起動画面(項目8) ──────────────────────────────────────────────
// 電源投入直後は黒画面で「起動したのか分からない」ため、LCD初期化の直後に即表示する。
// グレー地に「Holy Grail / Time Lapse」を黒文字+白縁取りで描く。
// 機種ごとに画像へ差し替えられるようフックを用意する: SPLASH_PX に RGB565 配列を与えればそれを
// 中央に描画し、nullptr のままならテキストで描く(現状)。
static const uint16_t* SPLASH_PX = nullptr;	// 例: edgeSplashStickS3.h を include して差し替える
static const int       SPLASH_W  = 0;
static const int       SPLASH_H  = 0;

static void renderSplash(void)
{
	if (!g_spriteOk) { return; }
	const uint16_t gray = g_cv.color565(0x9E, 0x9E, 0x9E);
	g_cv.fillScreen(gray);
	if (SPLASH_PX != nullptr && SPLASH_W > 0 && SPLASH_H > 0)
	{
		g_cv.pushImage((g_scrW - SPLASH_W) / 2, (g_scrH - SPLASH_H) / 2, SPLASH_W, SPLASH_H, SPLASH_PX);
		// 版数を右下へ小さく出す(2026-08-08 UI依頼)。どのファームが載っているかを画面だけで確認できる。
		g_cv.setFont(&fonts::Font2);
		g_cv.setTextDatum(textdatum_t::bottom_right);
		g_cv.setTextColor(TFT_BLACK);
		g_cv.drawString(HGC_EDGE_VERSION, g_scrW - 3, g_scrH - 2);
		g_cv.setTextDatum(textdatum_t::top_left);
		g_cv.pushSprite(0, 0);
		return;
	}
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
	outlined("Holy Grail", g_scrW / 2, g_scrH / 2 - 16);
	outlined("Time Lapse", g_scrW / 2, g_scrH / 2 + 16);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.setTextColor(TFT_WHITE);
	g_cv.setFont(&fonts::Font2);
	// 版数を右下へ小さく出す(2026-08-08 UI依頼)。どのファームが載っているかを画面だけで確認できる。
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextDatum(textdatum_t::bottom_right);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.drawString(HGC_EDGE_VERSION, g_scrW - 3, g_scrH - 2);
	g_cv.setTextDatum(textdatum_t::top_left);
	g_cv.pushSprite(0, 0);
}

// ── プロビジョニング/AP QR ──
static void enterProv(void)
{
	char pop[9];
	for (int i = 0; i < 8; ++i) { uint32_t r = esp_random() % 36; pop[i] = (r < 10) ? char('0' + r) : char('A' + (r - 10)); }
	pop[8] = 0;
	g_pop = pop; g_provMode = true; g_dirty = true;
}
void edgeProvShowQr(void) { enterProv(); }
void edgeProvApply(const char* name, const char* ssid, const char* pass, const char* mode)
{
	std::string m = (mode && mode[0]) ? mode : "sta";
	if (name && name[0]) { g_devName = name; }
	if (m == "ap")
	{
		// AP資格を受け取ったら保存する(2026-08-08)。従来は端末名だけ保存し SSID/pass を
		//  捨てていたため、APのSSID/パスワードをユーザーが決められなかった。
		//  空で送られたときは今の値を保つ。まだ何も無ければ ensureApCreds が
		//  端末固有の既定値(HGC-Edge-<MAC下2桁> / 8桁乱数)を作る。
		if (ssid && ssid[0]) { g_apSsid = ssid; }
		if (pass && pass[0]) { g_apPass = pass; }
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
	// 【SSIDが空なら「名前だけ変更」(2026-08-08)】従来はここで何もせず戻っていたため、
	//  端末名だけ変えたいときもSSID/passの再入力を強いていた(スマホ側も必須にしていた)。
	//  空のときは今の接続先をそのまま保ち、端末名だけ更新して再起動する。
	if (ssid == nullptr || ssid[0] == 0)
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
	int sz = g_scrH - 24;	// 縦に収める
	g_cv.qrcode(qr.c_str(), (g_scrW - sz) / 2, 2, sz, 6);	// version 6: 内容が増えたため
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::bottom_center);
	g_cv.drawString("Scan with phone", g_scrW / 2, g_scrH - 2);
	g_cv.setTextDatum(textdatum_t::top_left);
}
// AP参加情報。QRは廃止した(理由はCoreS3側 renderApInfo のコメント参照)。カメラへ手入力
// する値なので、狭い画面でも読めるよう画面を分けて大きな文字で出す。
static void renderApInfo(void)
{
	g_cv.fillScreen(TFT_WHITE);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::middle_center);
	g_cv.setFont(&fonts::Font2);
	g_cv.drawString("AP mode: connect to this AP", g_scrW / 2, 12);
	g_cv.setFont(&fonts::Font4);
	g_cv.drawString(g_apSsid.c_str(), g_scrW / 2, 48);
	g_cv.drawString(g_apPass.c_str(), g_scrW / 2, 90);
	g_cv.setFont(&fonts::Font2);
	g_cv.drawString("SSID / password", g_scrW / 2, g_scrH - 12);
	g_cv.setTextDatum(textdatum_t::top_left);
}
static void startApAndEtp(void)
{
	ensureApCreds();
	if (wifiConnect::startAp(g_apSsid.c_str(), g_apPass.c_str(), 10))
	{
		Serial.printf("[AP] SoftAP up ssid=%s pass=%s ip=%s\n", g_apSsid.c_str(), g_apPass.c_str(), wifiConnect::apIp().c_str());
		etpEdge::setup(g_devName);
		g_edgeUp = true; g_apInfoMode = true;
		hge_resumeCapture(); hge_presenceStart(); g_state = hge_getState();	// 項目1: 在否モニタ開始(共通)
	}
	else { Serial.println("[AP] softAP start FAILED"); }
}
// スプライトの一部(y..y+h 行)だけを LCD へ転送する。全画面転送の掃引=ちらつきを避ける確実な部分更新。
// スプライト(16bpp)はメモリ上で幅g_scrW×高さ連続なので、y行目の先頭は buf + y*g_scrW。
static void pushBand(int y, int h)
{
	auto* buf = (const lgfx::rgb565_t*)g_cv.getBuffer();
	if (!buf) { g_cv.pushSprite(0, 0); return; }
	if (y < 0) { h += y; y = 0; }
	if (y + h > g_scrH) { h = g_scrH - y; }
	if (h <= 0) return;
	g_lcd.pushImage(0, y, g_scrW, h, buf + (size_t)y * g_scrW);
}
// bandOnly=true のとき、スプライトへ全画面描画した上で、LCD へは下部の状態帯だけを転送する
// (状態アイコン/点滅など「変わるのは下部だけ」の更新でのちらつきを防ぐ)。計画/一覧変更時は false=全画面。
static void redraw(bool bandOnly)
{
	// AP参加QRは撮影/待機が始まったら自動で閉じ、計画・進捗を見せる。スマホから開始した場合でも
	// エッジのボタンを押さずに切り替わる(QRは「カメラ/スマホを参加させるまで」の初期表示)。
	if (g_apInfoMode && g_state != HGE_ST_IDLE) { g_apInfoMode = false; }
	if (g_apInfoMode) { renderApInfo(); g_cv.pushSprite(0, 0); return; }
	if (g_provMode) { renderProv(); g_cv.pushSprite(0, 0); return; }
	renderPlan();
	if (bandOnly) { pushBand(g_scrH - BAND_H, BAND_H); }
	else          { g_cv.pushSprite(0, 0); }
}

// ── ボタン操作 ──
static void handleButtons(uint32_t now)
{
	int e1 = g_key1.update(now);
	int e2 = g_key2.update(now);

	// 「この押下はもう使い終わった」フラグ。**離すまで**点灯にも操作にも使わない。
	//  ・復帰に使った押下: 離した瞬間の短押しで撮影が始まってしまうのを防ぐ
	//  ・長押し消灯に使った押下: 消した直後もキーは押されたままなので、**この判定を
	//    先に置かないと次のループで即座に点け直してしまう**(2026-08-17 実機で発生)
	static bool swallow = false;
	if (swallow)
	{
		if (!g_key1.pressedNow() && !g_key2.pressedNow()) { swallow = false; }
		return;
	}

	// 消灯中のキー操作は**点けるだけ**(ユーザー指示 2026-08-17)。長押しの800ms待ちを挟まず
	//  即座に点けたいので、update() の結果ではなく「押されている」ことそのものを見る。
	if (!g_bl.isOn())
	{
		if (g_key1.pressedNow() || g_key2.pressedNow())
		{
			g_bl.poke(now);
			g_dirty = true;	// 点けた直後の画面を描き直す
			swallow = true;
		}
		return;
	}

	// KEY1/KEY2 どちらの長押しでも手動消灯(ユーザー指示 2026-08-17)。
	if ((e1 & 2) || (e2 & 2)) { g_bl.off(now); swallow = true; return; }

	// 何か押されていれば無操作タイマを進めない。
	if (g_key1.pressedNow() || g_key2.pressedNow()) { g_bl.poke(now); }

	// 参加情報/プロビジョニング表示中はどちらのキーでも計画画面へ戻る。
	if (g_apInfoMode || g_provMode)
	{
		if ((e1 & 1) || (e2 & 1) || (e1 & 2) || (e2 & 2)) { g_apInfoMode = false; g_provMode = false; g_dirty = true; }
		return;
	}

	const json& arr = planList();

	// 項目7: エッジ側の削除機能は廃止(スマホ管理へ一本化)。KEY1長押し削除・削除確認は撤去した。
	//  終わった計画は pruneFinishedPlans が自動削除する。

	// KEY2 短押し: 次の計画へ(1方向ローリング、末尾で先頭へ)。
	if ((e2 & 1) && !arr.empty()) { g_cur = (g_cur + 1) % (int)arr.size(); g_dirty = true; }

	// KEY1 短押し: 開始/停止。即時反映: アイコンだけ先に切り替えて描画し、実処理は loop 末尾で行う
	// (開始は計画読込で数百ms・停止はスレッドjoinで数秒ブロックするため、先に描く)。処理待ち中の連打は無視。
	if ((e1 & 1) && !arr.empty() && g_cur < (int)arr.size())
	{
		const auto& p = arr[g_cur];
		std::string id = p.value("id", std::string());
		int state = p.value("state", 0);
		bool capturing = (state == HGE_ST_CAPTURING || state == HGE_ST_STOPPING ||
		                  state == HGE_ST_WAITING || state == HGE_ST_SEARCHING ||
		                  state == HGE_ST_NOCAMERA || state == HGE_ST_DISCONNECTED);
		bool capturable = p.value("capturable", false);
		if (g_pendingIcon.count(id) == 0)
		{
			int kind = capturing ? 2 : (capturable ? 1 : 0);
			if (kind != 0)
			{
				g_pendingIcon[id] = kind;
				g_opQueue.push_back({ id, kind });
				g_dirty = true;
			}
		}
	}
}

// ── Entity 通知 ──
static char g_prog[64] = "";
static char g_shot[64] = "";
static char g_msg[80]  = "";
static void notifyCb(int32_t ev, const char* json_, int32_t len, void* user)
{
	(void)len; (void)user;
	if (json_ == nullptr) { json_ = ""; }
	switch (ev)
	{
	case HGE_EV_STATE:
	{
		int s = HGE_ST_IDLE;
		const char* p = std::strstr(json_, "\"state\":");
		if (p) { std::sscanf(p, "\"state\":%d", &s); }
		// 時刻になって撮影が始まったら画面を点ける(ユーザー指示 2026-08-17)。
		if (s == HGE_ST_CAPTURING && g_state != HGE_ST_CAPTURING) { g_blWake = true; }
		g_state = s;
		if (g_apInfoMode && s != HGE_ST_IDLE) { g_dirty = true; }	// 参加情報表示中に撮影開始→閉じて計画画面へ(全画面再描画)
		Serial.printf("[EV] STATE: %s\n", stName(s));
		break;
	}
	case HGE_EV_PROGRESS: std::snprintf(g_prog, sizeof(g_prog), "%s", json_); break;
	case HGE_EV_CAPTURED: std::snprintf(g_shot, sizeof(g_shot), "%s", json_); break;
	case HGE_EV_ERROR:    std::snprintf(g_msg, sizeof(g_msg), "%s", json_); Serial.printf("[EV] ERROR: %s\n", json_); break;
	case HGE_EV_DEVICE:   std::snprintf(g_msg, sizeof(g_msg), "%s", json_); break;
	default: break;
	}
	g_listDirty = true;
	// 状態/進捗/撮影完了は画面下部の状態帯(アイコン/文字)しか変えない → バンドのみ部分更新でちらつき回避。
	// それ以外(計画一覧の入替やエラー等・レイアウトが変わりうる)は全画面更新にする。
	if (ev == HGE_EV_STATE || ev == HGE_EV_PROGRESS || ev == HGE_EV_CAPTURED) { g_bandDirty = true; }
	else { g_dirty = true; Serial.printf("[EV] full-redraw ev=%ld\n", (long)ev); }
}

// StickS3 の LCD/バックライト電源は M5PM1(PMIC, I2C 0x6E, SDA=47/SCL=48)から供給される。
// 汎用ボード指定で M5.begin すると StickS3 として自動認識されず、この PMIC 初期化が走らないため
// LCD 電源レールが不安定になり「画面全体のちらつき」が出る。M5GFX の StickS3 初期化と同じ手順で
// G2(=LCD Power On)を出力HIGHにし、PMIC のアイドルスリープ(reg 0x09)を無効化して安定化する。
void setup(void)
{
	m5pm1LcdPowerOn();	// ★ちらつき対策: LCD電源(M5PM1 G2)を安定ONにする(M5.begin より前・自前I2C)
	auto cfg = M5.config();
	// StickS3 に内蔵RTCは無い。外付けRTCユニット(Port A の I2C)を接続したときだけ使う。
	// M5Unified は external_rtc=true のとき Ex_I2C を走査し、居れば M5.Rtc を有効化して
	// システム時計を RTC から復元する(居なければ isEnabled()==false のままで無害)。
	// これにより電源断→復帰でもスマホ無しで時刻が戻り、単独運用ができる。
	cfg.external_rtc = true;
	M5.begin(cfg);		// board 認識は既定のまま。StickS3電源管理はさせない(ブートループ回避)
	dbg::init();
	Serial.printf("[M5] board=%d rtc=%d ex_i2c=%d\n",
	              (int)M5.getBoard(), (int)M5.Rtc.isEnabled(), (int)M5.Ex_I2C.isEnabled());

	g_lcd.init();			// 自前パネル(SPI2)を初期化
	g_lcd.setRotation(3);	// 横向き(240x135)。上下逆なら 1 に。
	gpio_reset_pin(GPIO_NUM_38);
	gpio_set_direction(GPIO_NUM_38, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_NUM_38, 1);	// バックライトenable(実体の電源はM5PM1で安定化済み)
	g_bl.begin(blApply, millis());	// バックライト自動消灯を開始(点灯状態から)
	g_scrW = g_lcd.width(); g_scrH = g_lcd.height();
	// 全画面スプライト(240x135x2=64KB)を PSRAM に確保しダブルバッファ描画する。
	// Arduino 3.x で Octal PSRAM が有効なので PSRAM に載る。失敗時は内蔵RAM(setPsram(false))へフォールバック。
	g_lcd.fillScreen(TFT_BLACK);
	g_cv.setColorDepth(16);
	g_cv.setPsram(true);
	void* sb = g_cv.createSprite(g_scrW, g_scrH);
	if (sb == nullptr)
	{
		Serial.println("[LCD] PSRAM sprite failed -> internal RAM");
		g_cv.setPsram(false);
		sb = g_cv.createSprite(g_scrW, g_scrH);
		if (sb == nullptr) { Serial.println("[LCD] createSprite FAILED"); }
	}
	g_spriteOk = (sb != nullptr);
	Serial.printf("[LCD] spriteOk=%d buf=%p psramFree=%u internalFree=%u\n",
	              (int)g_spriteOk, g_cv.getBuffer(),
	              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
	              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
	g_cv.setFont(&fonts::Font2);

	renderSplash();		// 項目8: 起動画面を即表示(以降の初期化中も出したまま。終わったら計画一覧へ)

	g_key1.begin(PIN_KEY1);
	g_key2.begin(PIN_KEY2);

	loadEdgeCreds();	// 完全未設定(出荷時)はAP既定
	Serial.printf("[NET] mode=%s devname=%s\n", g_netMode.c_str(), g_devName.c_str());
	wifiConnect::setup();
	edgeProv::begin(g_devName);

	hge_init();
	hge_setNotify(notifyCb, nullptr);
	hge_pruneOldLogs(osclock::utcOffsetMin());
	hge_loadFixedPlan();
	loadRecvPlans();
	loadNameBitmaps();
	if (g_netMode == "ap") { startApAndEtp(); }
	g_state = hge_getState();
	redraw(false);
}

// ── バッテリ残量ログ(放電カーブの実測用) ──
// 単独動作(USB非接続)で電池が切れるまでの推移をログに残す。残量表示のしきい値や
// 「自ら電源を切る」限界電圧は、このログの実測から決める(推測で決めない)。
//  ・M5.Power は StickS3(ボード未認識)でも有効な値を返すことを実測で確認済み(2026-07-22)。
//  ・up= は起動からの経過秒。途中で再起動すると 0 に戻るので、電池切れ以外の再起動を見分けられる。
//  ・osfile::append は都度追記なので、電源が落ちても直前の行までは残る。
static void logBatteryPeriodic(uint32_t nowMs)
{
	static uint32_t last = 0;
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
	uint32_t now = millis();
	logBatteryPeriodic(now);	// 残量ログ(60秒ごと)+ レベル更新 + 電源断シーケンスの開始
	// 電源断: スマホのポーリング1周期ぶん待ってから切る(✖を1回拾わせるため)。
	if (g_batt.readyToPowerOff(now))
	{
		dataManager::logEvent("PWROFF", "power off now", true);
		delay(50);				// ログの書き込みを確実に落とす
		M5.Power.powerOff();
		for (;;) { delay(1000); }	// powerOff が戻る機種でも先へ進ませない
	}

	// WiFi 再接続(STA)。
	if (g_netMode != "ap" && wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
	{
		Serial.printf("[WIFI] connecting to %s ...\n", g_ssid.c_str());
		bool ok = wifiConnect::connect(g_ssid.c_str(), g_pass.c_str());
		if (ok)
		{
			Serial.printf("[WIFI] connected. IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
			// StickS3 は RTC 無し=再起動で時計(1970)がリセットされ、撮影窓(2026)と一致せず WAITING のまま
			// になる。WiFi接続時に NTP で時刻(UTC)を取得する。取得後は ESP32 内部タイマがシステム時計を
			// 進める(settimeofday 相当)ので、次ループ以降 time() が正しくなり WAITING→CAPTURING に自動遷移。
			// ネット非到達時はスマホの時刻同期(C_TIME)に頼る。UTCオフセットはNVS保存値(前回C_TIME)を使用。
			configTime(0, 0, "ntp.nict.jp", "pool.ntp.org", "time.google.com");
			if (!g_edgeUp)
			{
				etpEdge::setup(g_devName);
				g_edgeUp = true;
				hge_resumeCapture();	// PSRAM有効化で測光/CCAPIがPSRAMに載りOOMしない。無人再起動の撮影再開を復活。
				hge_presenceStart();	// 項目1: エッジ自身の在否モニタ(スマホと共通)を開始。未検出は×で示す
				g_state = hge_getState();
			}
			g_dirty = true;
		}
		else { Serial.printf("[WIFI] connect failed.\n"); }
	}

	if (g_edgeUp) { etpEdge::loop(); }

	// 遅延アームのポンプ(§7.4): 予約計画の開始スレッドを期日(窓90秒前)に生成する。毎秒1回で十分。
	{
		static uint32_t lastPump = 0;
		if (now - lastPump >= 1000) { lastPump = now; hge_pump(); }
	}

	// 項目2: 終わった撮影計画をエッジから自動削除する。
	//  ・起動後、時計が使えるようになった最初の1回だけ全件(受信リストに無いファイルも含む)を見直す。
	//    電源が切れている間に期限切れになった計画は、その場では消せないのでここで拾う。
	//  ・以後は30秒毎。capturable は一覧を作った時点の判定なので、毎回取り直してから見る。
	{
		static uint32_t lastPrune  = 0;
		static bool     bootPruned = false;
		if (!bootPruned && clockUsable()) { bootPruned = true; lastPrune = now; pruneFinishedPlans(true); }
		else if (now - lastPrune >= 30000) { lastPrune = now; pruneFinishedPlans(false); }
	}

	edgeProv::loop();
	etpBle::loop();		// BLE で届いた ETP フレームを処理する(TCP と同じハンドラ)
	handleButtons(now);

	// 撮影中アイコンの点滅。
	{
		static uint32_t lastBlink = 0;
		// 点滅は状態帯だけの部分更新(g_dirty=全画面 とは別経路)。全画面 pushSprite の掃引が
		// 500ms ごとに画面全体のちらつきとして見えていた問題を回避する。
		if (g_state == HGE_ST_CAPTURING && (now - lastBlink) >= 500) { lastBlink = now; g_blinkOn = !g_blinkOn; g_bandDirty = true; }
	}

	// #8 時計の ":" 点滅(1秒周期=500msでトグル)+分の進行。撮影状態に関係なく常時。
	// QR/プロビジョニング表示中は下部バンド更新をしない(それらは全画面で別描画のため)。
	{
		static uint32_t lastColon = 0;
		if (!g_apInfoMode && !g_provMode && (now - lastColon) >= 500)
		{
			lastColon = now; g_colonOn = !g_colonOn;
			// バッテリ残りわずか(level2)のアイコン点滅。1秒周期にするため ":" 2回ぶんで1トグル。
			static int half = 0;
			if (++half >= 2) { half = 0; g_battBlinkOn = !g_battBlinkOn; }
			g_bandDirty = true;
		}
	}

	// シリアルコマンド(検証用。CoreS3版と同じ主要コマンド)。
	if (Serial.available() > 0)
	{
		int c = Serial.read();
		if (c == 's') { hge_captureStart(); }
		else if (c == 'x') { hge_captureStop(); }
		else if (c == 'p') { enterProv(); }
		else if (c == 'A') { Serial.println("[AP] switch to AP, restart"); saveNetMode("ap");  delay(200); ESP.restart(); }
		else if (c == 'S') { Serial.println("[STA] switch to STA, restart"); saveNetMode("sta"); delay(200); ESP.restart(); }
		else if (c == 'j') { hge_setUtcOffset(540); hge_loadFixedPlan(); g_state = hge_getState(); g_dirty = true; }
		else if (c == 'u') { hge_setUtcOffset(0);   hge_loadFixedPlan(); g_state = hge_getState(); g_dirty = true; }
		else if (c == 'i')
		{
			Serial.printf("[INFO] dev=%s state=%s wifi=%d IP=%s edgeUp=%d LCD=%dx%d spriteOk=%d\n",
			              g_devName.c_str(), stName(g_state), (int)(WiFi.status() == WL_CONNECTED),
			              WiFi.localIP().toString().c_str(), (int)g_edgeUp, g_scrW, g_scrH, (int)g_spriteOk);
		}
		else if (c == 'F') { Serial.printf("[FS] backend=%s\n", osfile::backendName()); }
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
		else if (c == 'b')	// 検証用: 電源(バッテリ)の読み値を確認する。
		{
			// StickS3 は M5.begin にボードを認識させていない(ブートループ回避)ので M5.Power が
			// 使えるか不明だったが、実測で有効な値を返すことを確認した
			// (2026-07-22: CoreS3 4135mV / StickS3 4148mV と機体別の実値)。
			// 電源IC(M5PM1 0x6E)の直読みも試したが全レジスタ応答なしで、そもそも不要だった。
			Serial.printf("[BATT] pct=%d volt=%dmV chg=%d\n",
			              (int)M5.Power.getBatteryLevel(),
			              (int)M5.Power.getBatteryVoltage(),
			              (int)M5.Power.isCharging());
		}
		else if (c == 'k') { Serial.printf("[KEY] k1=%d k2=%d\n", (int)g_key1.pressedNow(), (int)g_key2.pressedNow()); }
		else if (c == 'N')
		{
			Serial.printf("[NBMP] ram=%d recvPlans=%d\n", (int)g_nameBmps.size(), (int)g_recvPlans.size());
			for (const auto& id : g_recvPlans)
			{
				std::string body; bool ok = osfile::readAll(nameBmpPath(id), body);
				Serial.printf("  id=%s ramHas=%d file=%s(%u B)\n", id.c_str(), (int)(g_nameBmps.count(id) > 0), ok ? "yes" : "no", (unsigned)body.size());
			}
		}
	}

	// 計画/一覧の変化(g_dirty)は全画面更新を優先。状態/進捗/点滅(g_bandDirty)は下部の状態帯だけ部分転送。
	// バックライト: 通知スレッドからの点灯要求を反映し、無操作が続いたら消す。
	if (g_blWake) { g_blWake = false; if (g_bl.poke(millis())) { g_dirty = true; } }
	g_bl.update(millis());

	// 消灯中は描かない(点けたときに描き直す)。
	if (!g_bl.isOn()) { /* 何も描かない */ }
	else if (g_dirty) { redraw(false); g_dirty = false; g_bandDirty = false; }
	else if (g_bandDirty) { redraw(true); g_bandDirty = false; }

	// 保留中の開始/停止を実行(KEY1時のアイコン切替は上の redraw で反映済み)。
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
	delay(10);
}
