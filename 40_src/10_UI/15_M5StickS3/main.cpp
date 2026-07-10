// HolyGrail Controller エッジ端末(M5StickC S3 / StickS3) アプリ。
// holyGrailEntity を駆動し、スマホからの ETP(§6)制御・BLE設定(§8.2)を受ける。
// CoreS3版(10_M5Stack)とロジックは共通だが、UIが異なる:
//   ・LCD は小さく(240x135 横向き)タッチ非対応。物理ボタン2つで操作する。
//   ・KEY2(側面/GPIO12)= 計画スクロール(1方向ローリング、末尾で先頭へ戻る)。
//   ・KEY1(正面/GPIO11)= 表示中の計画を開始/停止、長押しで削除(確認あり)。
//   ・端末名称 + 1つの撮影計画(名称は2段折り返し + 日付/時刻)だけ表示する。
// etpEdge.cpp / edgeProv.cpp は表示非依存なので CoreS3版を共有ビルドする(build_src_filter)。

#include <M5Unified.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_ST7789.hpp>	// M5GFX.h は ST7789 パネル型を公開しないため明示include
#include <WiFi.h>
#include <Preferences.h>
#include <esp_random.h>
#include <driver/gpio.h>
#include <json/nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "dataManager.h"
#include "osFile.h"
#include "osClock.h"
#include "debugOut.h"

using json = nlohmann::json;

// loopTask のスタック拡張(天文計算の再帰でオーバーフローするため。CoreS3版と同じ理由)。
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ── StickS3 LCD(ST7789P3 135x240)を明示設定する ──
// M5Unified はこのボードを自動認識しない(汎用 devkit ボードのため)ので、パネルを直接構成する。
// ピン: MOSI=39 SCK=40 DC=45 CS=41 RST=21 BL=38(M5 StickS3 データシート)。
class DisplayS3 : public lgfx::LGFX_Device
{
	lgfx::Panel_ST7789 _st7789;		// 基底の _panel と名前衝突しないよう別名にする
	lgfx::Bus_SPI      _spibus;
	lgfx::Light_PWM    _backlight;
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
			c.offset_x      = 52;	// ST7789 135x240 パネルの表示オフセット(StickC Plus系と同じ既定)
			c.offset_y      = 40;
			c.offset_rotation = 0;
			c.readable      = false;
			c.invert        = true;
			c.rgb_order     = false;
			c.dlen_16bit    = false;
			c.bus_shared    = false;
			_st7789.config(c);
		}
		{
			auto c = _backlight.config();
			c.pin_bl      = 38;
			c.invert      = false;
			c.freq        = 44100;
			c.pwm_channel = 7;
			_backlight.config(c);
			_st7789.setLight(&_backlight);
		}
		setPanel(&_st7789);
	}
};

static DisplayS3        g_lcd;
// 表示はLCDへ直接描画する。全画面スプライト(240x135x2=64KB)は内蔵RAMを圧迫し、
// PSRAMもこのモジュールでは使えない(OPIハング)ため撮影パイプライン(CCAPIパース/測光)が
// メモリ不足で落ちる。g_cv を g_lcd への参照にして 64KB を恒久的に空ける(描画APIは共通基底)。
static lgfx::LovyanGFX& g_cv = g_lcd;
static int              g_scrW = 240, g_scrH = 135;	// 横向きの論理サイズ

// ── 物理ボタン(生GPIO。M5Unified は本ボードのボタンを対応付けないため直接読む) ──
//  KEY1=GPIO11(正面): 開始/停止・長押し削除 / KEY2=GPIO12(側面): スクロール。押下=LOW(内部プルアップ)。
static constexpr gpio_num_t PIN_KEY1 = GPIO_NUM_11;
static constexpr gpio_num_t PIN_KEY2 = GPIO_NUM_12;
static constexpr uint32_t   LONG_PRESS_MS = 800;

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
static bool        g_apQrMode = false;

static void loadEdgeCreds(void)
{
	Preferences p;
	if (p.begin("hgc", true))
	{
		String s = p.getString("ssid", ""), w = p.getString("pass", ""), n = p.getString("devname", "");
		if (s.length()) { g_ssid = s.c_str(); }
		if (w.length()) { g_pass = w.c_str(); }
		if (n.length()) { g_devName = n.c_str(); }
		String nm = p.getString("netmode", ""); if (nm.length()) { g_netMode = nm.c_str(); }
		String as = p.getString("apssid", "");  if (as.length()) { g_apSsid  = as.c_str(); }
		String ap = p.getString("appass", "");  if (ap.length()) { g_apPass  = ap.c_str(); }
		p.end();
	}
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
static bool g_confirmDel = false;	// 表示中計画の削除確認中か
static bool g_listDirty = true;
static volatile int g_state = HGE_ST_IDLE;
static bool g_blinkOn = true;
static bool g_dirty = true;
static bool g_edgeUp = false;

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

	if (arr.empty())
	{
		g_cv.setTextColor(TFT_LIGHTGREY);
		g_cv.setCursor(8, 44);  g_cv.print("No plans");
		g_cv.setCursor(8, 66);  g_cv.print("Send from phone");
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

	// 右上: 何番目/全体(スクロール位置)。
	{
		char idx[16]; std::snprintf(idx, sizeof(idx), "%d/%d", g_cur + 1, (int)arr.size());
		g_cv.setTextColor(TFT_DARKGREY);
		g_cv.setCursor(g_scrW - g_cv.textWidth(idx) - 4, headH + 2); g_cv.print(idx);
	}

	// 計画名(2段折り返し)。ビットマップがあればそれを、無ければテキストを折り返す。
	int nameY = headH + 4;
	int used = 0;
	const int maxW = g_scrW - 8;
	auto it = g_nameBmps.find(id);
	if (it != g_nameBmps.end() && it->second.px) { used = drawNameBitmapWrapped(it->second, 4, nameY, maxW); }
	else { used = drawNameTextWrapped(name, 4, nameY, maxW); }

	// 日付/時刻(開始・終了を2行)。
	int ty = nameY + used + 6;
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_LIGHTGREY);
	g_cv.setCursor(4, ty);      g_cv.print(("Start " + mmddhhmm(st)).c_str());
	g_cv.setCursor(4, ty + 18); g_cv.print(("End   " + mmddhhmm(en)).c_str());

	// 状態行(下部) + KEY1で何ができるか。
	int sy = g_scrH - 16;
	uint16_t scol = TFT_LIGHTGREY; const char* stxt = "";
	if (nocam)          { scol = g_cv.color565(0xFF, 0x55, 0x55); stxt = "NO CAMERA  KEY1:Stop"; }
	else if (capturing) { scol = g_blinkOn ? g_cv.color565(0x66, 0xEE, 0x66) : TFT_DARKGREY; stxt = "CAPTURING  KEY1:Stop"; }
	else if (waiting)   { scol = g_cv.color565(0x66, 0xEE, 0x66); stxt = "WAITING    KEY1:Stop"; }
	else if (capturable){ scol = TFT_WHITE; stxt = "READY      KEY1:Start"; }
	else                { scol = TFT_DARKGREY; stxt = "(past)"; }
	g_cv.setTextColor(scol);
	g_cv.setCursor(4, sy); g_cv.print(stxt);

	// 削除確認オーバーレイ。
	if (g_confirmDel)
	{
		g_cv.fillRoundRect(20, 34, g_scrW - 40, 66, 6, g_cv.color565(0x22, 0x22, 0x22));
		g_cv.drawRoundRect(20, 34, g_scrW - 40, 66, 6, TFT_WHITE);
		g_cv.setTextColor(TFT_WHITE);
		g_cv.setCursor(32, 44); g_cv.print("Delete this plan?");
		g_cv.setTextColor(g_cv.color565(0xFF, 0x88, 0x88));
		g_cv.setCursor(32, 70); g_cv.print("KEY1:Yes   KEY2:No");
	}
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
		Preferences p; if (p.begin("hgc", false)) { p.putString("devname", g_devName.c_str()); p.end(); }
		saveNetMode("ap");
		Serial.println("[PROV] mode=ap -> restart into AP");
		delay(200); ESP.restart();
		return;
	}
	if (ssid == nullptr || ssid[0] == 0) { Serial.println("[PROV] empty ssid, skip"); return; }
	saveEdgeCreds(ssid, pass ? pass : "", g_devName);
	saveNetMode("sta");
	Serial.printf("[PROV] mode=sta ssid=%s -> restart into STA\n", g_ssid.c_str());
	delay(200); ESP.restart();
}
static void renderProv(void)
{
	g_cv.fillScreen(TFT_WHITE);
	std::string qr = std::string("{\"n\":\"") + g_devName + "\",\"pop\":\"" + g_pop + "\"}";
	int sz = g_scrH - 24;	// 縦に収める
	g_cv.qrcode(qr.c_str(), (g_scrW - sz) / 2, 2, sz, 4);
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::bottom_center);
	g_cv.drawString("Scan with phone", g_scrW / 2, g_scrH - 2);
	g_cv.setTextDatum(textdatum_t::top_left);
}
static void renderApQr(void)
{
	g_cv.fillScreen(TFT_WHITE);
	std::string qr = std::string("WIFI:T:WPA;S:") + g_apSsid + ";P:" + g_apPass + ";;";
	int sz = g_scrH - 24;
	g_cv.qrcode(qr.c_str(), (g_scrW - sz) / 2, 2, sz, 4);
	g_cv.setFont(&fonts::Font2);
	g_cv.setTextColor(TFT_BLACK);
	g_cv.setTextDatum(textdatum_t::bottom_center);
	g_cv.drawString((g_apSsid).c_str(), g_scrW / 2, g_scrH - 2);
	g_cv.setTextDatum(textdatum_t::top_left);
}
static void startApAndEtp(void)
{
	ensureApCreds();
	if (wifiConnect::startAp(g_apSsid.c_str(), g_apPass.c_str(), 10))
	{
		Serial.printf("[AP] SoftAP up ssid=%s pass=%s ip=%s\n", g_apSsid.c_str(), g_apPass.c_str(), wifiConnect::apIp().c_str());
		etpEdge::setup(g_devName);
		g_edgeUp = true; g_apQrMode = true;
		hge_resumeCapture(); g_state = hge_getState();
	}
	else { Serial.println("[AP] softAP start FAILED"); }
}
static void redraw(void)
{
	if (g_apQrMode) { renderApQr(); }
	else if (g_provMode) { renderProv(); }
	else { renderPlan(); }
}

// ── ボタン操作 ──
static void handleButtons(uint32_t now)
{
	int e1 = g_key1.update(now);
	int e2 = g_key2.update(now);

	// QR表示中はどちらのキーでも計画画面へ戻る。
	if (g_apQrMode || g_provMode)
	{
		if ((e1 & 1) || (e2 & 1) || (e1 & 2) || (e2 & 2)) { g_apQrMode = false; g_provMode = false; g_dirty = true; }
		return;
	}

	const json& arr = planList();

	// 削除確認中: KEY1=はい / KEY2=いいえ。
	if (g_confirmDel)
	{
		if (e1 & 1)
		{
			if (g_cur >= 0 && g_cur < (int)arr.size()) { edgeRemoveReceivedPlan(arr[g_cur].value("id", std::string())); }
			g_confirmDel = false; g_dirty = true;
		}
		else if (e2 & 1) { g_confirmDel = false; g_dirty = true; }
		return;
	}

	// KEY2 短押し: 次の計画へ(1方向ローリング、末尾で先頭へ)。
	if ((e2 & 1) && !arr.empty()) { g_cur = (g_cur + 1) % (int)arr.size(); g_dirty = true; }

	// KEY1 長押し: 表示中の計画を削除(確認へ)。
	if ((e1 & 2) && !arr.empty()) { g_confirmDel = true; g_dirty = true; return; }

	// KEY1 短押し: 開始/停止。
	if ((e1 & 1) && !arr.empty() && g_cur < (int)arr.size())
	{
		const auto& p = arr[g_cur];
		std::string id = p.value("id", std::string());
		int state = p.value("state", 0);
		bool capturing = (state == HGE_ST_CAPTURING || state == HGE_ST_STOPPING ||
		                  state == HGE_ST_WAITING || state == HGE_ST_SEARCHING ||
		                  state == HGE_ST_NOCAMERA || state == HGE_ST_DISCONNECTED);
		bool capturable = p.value("capturable", false);
		if (capturing)       { hge_captureStopPlan(id.c_str()); }
		else if (capturable) { hge_captureStartPlan(id.c_str()); }
		g_listDirty = true; g_dirty = true;
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
		g_state = s;
		Serial.printf("[EV] STATE: %s\n", stName(s));
		break;
	}
	case HGE_EV_PROGRESS: std::snprintf(g_prog, sizeof(g_prog), "%s", json_); break;
	case HGE_EV_CAPTURED: std::snprintf(g_shot, sizeof(g_shot), "%s", json_); break;
	case HGE_EV_ERROR:    std::snprintf(g_msg, sizeof(g_msg), "%s", json_); Serial.printf("[EV] ERROR: %s\n", json_); break;
	case HGE_EV_DEVICE:   std::snprintf(g_msg, sizeof(g_msg), "%s", json_); break;
	default: break;
	}
	g_listDirty = true; g_dirty = true;
}

void setup(void)
{
	auto cfg = M5.config();
	M5.begin(cfg);		// Rtc/Power 等(ディスプレイは本ボード非対応のため下で明示初期化)
	dbg::init();

	g_lcd.init();
	g_lcd.setRotation(3);	// 横向き(240x135)。上下逆なら 1 に。
	g_lcd.setBrightness(200);
	g_scrW = g_lcd.width(); g_scrH = g_lcd.height();
	// スプライト(全画面64KB)は廃止し LCD へ直接描画する(内蔵RAM節約)。g_cv は g_lcd の別名。
	g_lcd.fillScreen(TFT_BLACK);
	g_cv.setFont(&fonts::Font2);

	g_key1.begin(PIN_KEY1);
	g_key2.begin(PIN_KEY2);

	loadEdgeCreds();
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
	redraw();
}

void loop(void)
{
	M5.update();
	uint32_t now = millis();

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
				g_state = hge_getState();
			}
			g_dirty = true;
		}
		else { Serial.printf("[WIFI] connect failed.\n"); }
	}

	if (g_edgeUp) { etpEdge::loop(); }
	edgeProv::loop();
	handleButtons(now);

	// 撮影中アイコンの点滅。
	{
		static uint32_t lastBlink = 0;
		if (g_state == HGE_ST_CAPTURING && (now - lastBlink) >= 500) { lastBlink = now; g_blinkOn = !g_blinkOn; g_dirty = true; }
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
			Serial.printf("[INFO] dev=%s state=%s wifi=%d IP=%s edgeUp=%d LCD=%dx%d\n",
			              g_devName.c_str(), stName(g_state), (int)(WiFi.status() == WL_CONNECTED),
			              WiFi.localIP().toString().c_str(), (int)g_edgeUp, g_scrW, g_scrH);
		}
		else if (c == 'F') { Serial.printf("[FS] backend=%s\n", osfile::backendName()); }
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

	if (g_dirty) { redraw(); g_dirty = false; }
	delay(10);
}
