// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。開発ステップ2.2 MVP。
// 固定データの撮影計画で holyGrailEntity を駆動し、開始/停止のみ行う最小実装。
// UI は LCD 表示 + ボタン(A:開始 / B:停止)。

#include <M5Unified.h>
#include <cstdio>
#include "common.h"
#include "holyGrailEntity.h"
#include "WiFi_Connect.h"
#include "debugOut.h"

// 実機接続時に設定する(カメラの AP もしくは同一LANのSSID)。
static const char* WIFI_SSID = "your-ssid";
static const char* WIFI_PASS = "your-pass";

static volatile int g_state = HGE_ST_IDLE;
static char g_prog[64] = "";
static char g_shot[64] = "";
static char g_msg[80]  = "";
static bool g_dirty = true;

static const char* stName(int s)
{
	switch (s)
	{
	case HGE_ST_IDLE:      return "IDLE";
	case HGE_ST_SEARCHING: return "SEARCHING";
	case HGE_ST_READY:     return "READY";
	case HGE_ST_CAPTURING: return "CAPTURING";
	case HGE_ST_STOPPING:  return "STOPPING";
	case HGE_ST_ERROR:     return "ERROR";
	default:               return "?";
	}
}

// Entity からの通知(ワーカースレッドから呼ばれる)。表示は loop() 側で行う。
static void notifyCb(int32_t ev, const char* json, int32_t len, void* user)
{
	(void)len; (void)user;
	if (json == nullptr) { json = ""; }
	switch (ev)
	{
	case HGE_EV_STATE:
	{
		int s = HGE_ST_IDLE;
		std::sscanf(json, "{\"state\":%d", &s);
		g_state = s;
		break;
	}
	case HGE_EV_PROGRESS:
		std::snprintf(g_prog, sizeof(g_prog), "%s", json);
		break;
	case HGE_EV_CAPTURED:
		std::snprintf(g_shot, sizeof(g_shot), "%s", json);
		break;
	case HGE_EV_ERROR:
	case HGE_EV_DEVICE:
		std::snprintf(g_msg, sizeof(g_msg), "%s", json);
		break;
	default:
		break;
	}
	g_dirty = true;
}

static void draw(void)
{
	M5.Display.fillScreen(TFT_BLACK);
	M5.Display.setCursor(0, 0);
	M5.Display.printf("HolyGrail Edge MVP\n");
	M5.Display.printf("state: %s\n", stName(g_state));
	if (g_prog[0]) { M5.Display.printf("%s\n", g_prog); }
	if (g_shot[0]) { M5.Display.printf("%s\n", g_shot); }
	if (g_msg[0])  { M5.Display.printf("%s\n", g_msg); }
	M5.Display.printf("\nA:start  B:stop");
}

void setup(void)
{
	auto cfg = M5.config();
	M5.begin(cfg);
	M5.Display.setTextSize(2);
	dbg::init();

	wifiConnect::setup();

	hge_init();
	hge_setNotify(notifyCb, nullptr);
	hge_loadFixedPlan();		// 出荷時設定(dataManager)から固定撮影計画を生成
	g_state = hge_getState();
	draw();
}

void loop(void)
{
	M5.update();

	// WiFi 切断時は再接続を試みる(実機運用時に SSID/PASS を設定する)
	if (wifiConnect::getStatus() == wifiConnect::wifiStatus::cuttingOff)
	{
		wifiConnect::connect(WIFI_SSID, WIFI_PASS);
	}

	if (M5.BtnA.wasPressed()) { hge_captureStart(); }
	if (M5.BtnB.wasPressed()) { hge_captureStop(); }

	if (g_dirty)
	{
		g_dirty = false;
		draw();
	}
	delay(100);
}
