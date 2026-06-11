// HolyGrail Controller エッジ端末(M5Stack CoreS3) アプリ。
// holyGrailEntity を駆動する。2系統で動作する:
//  - 単独: 固定撮影計画 + ボタン(A:開始 / B:停止)。
//  - スマホ制御: ETP(§6)でスマホから時刻同期・計画転送・開始/停止を受ける(etpEdge)。
// UI は LCD 表示。

#include <M5Unified.h>
#include <WiFi.h>
#include <cstdio>
#include "common.h"
#include "holyGrailEntity.h"
#include "WiFi_Connect.h"
#include "etpEdge.h"
#include "debugOut.h"

// loopTask(setup/loop)のスタックを拡張する。
// 既定8KBでは天文計算(Astronomy Engine: FindAscent 再帰 + CalcMoon)で
// オーバーフローするため。setup() で hge_loadFixedPlan() が buildSchedule() を同期実行する。
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// 実機接続時に設定する(カメラの AP もしくは同一LANのSSID)。
static const char* WIFI_SSID = "Buffalo-G-D850";
static const char* WIFI_PASS = "rnhcftfbk75tf";

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
		Serial.printf("[EV] STATE: %s\n", stName(s));
		break;
	}
	case HGE_EV_PROGRESS:
		std::snprintf(g_prog, sizeof(g_prog), "%s", json);
		Serial.printf("[EV] PROGRESS: %s\n", json);
		break;
	case HGE_EV_CAPTURED:
		std::snprintf(g_shot, sizeof(g_shot), "%s", json);
		Serial.printf("[EV] CAPTURED: %s\n", json);
		break;
	case HGE_EV_ERROR:
		std::snprintf(g_msg, sizeof(g_msg), "%s", json);
		Serial.printf("[EV] ERROR: %s\n", json);
		break;
	case HGE_EV_DEVICE:
		std::snprintf(g_msg, sizeof(g_msg), "%s", json);
		Serial.printf("[EV] DEVICE: %s\n", json);
		break;
	default:
		break;
	}
	g_dirty = true;
}

static bool g_edgeUp = false;	// ETPサーバ起動済みか(WiFi接続後に一度)

static void draw(void)
{
	M5.Display.fillScreen(TFT_BLACK);
	M5.Display.setCursor(0, 0);
	M5.Display.printf("HolyGrail エッジ端末\n");
	if (WiFi.status() == WL_CONNECTED)
	{
		M5.Display.printf("IP:%s %s\n", WiFi.localIP().toString().c_str(),
		                  g_edgeUp ? "ETP" : "");
	}
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
		Serial.printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
		bool ok = wifiConnect::connect(WIFI_SSID, WIFI_PASS);
		if (ok)
		{
			Serial.printf("[WIFI] connected. IP=%s RSSI=%d\n",
			              WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
			std::snprintf(g_msg, sizeof(g_msg), "WiFi %s",
			              WiFi.localIP().toString().c_str());
			// WiFi 接続後に ETP サーバ(検索応答+制御)を一度だけ起動する
			if (!g_edgeUp)
			{
				etpEdge::setup("エッジ端末");
				g_edgeUp = true;
			}
			g_dirty = true;
		}
		else
		{
			Serial.printf("[WIFI] connect failed.\n");
		}
	}

	// ETP サーバのポーリング(スマホからの検索/制御を処理)
	if (g_edgeUp) { etpEdge::loop(); }

	// タッチボタン: A=開始 / B=停止
	if (M5.BtnA.wasPressed()) { Serial.printf("[BTN] A start\n"); hge_captureStart(); }
	if (M5.BtnB.wasPressed()) { Serial.printf("[BTN] B stop\n");  hge_captureStop();  }

	// シリアルコマンド(検証用): 's'=開始 'x'=停止 'i'=情報
	if (Serial.available() > 0)
	{
		int c = Serial.read();
		if (c == 's') { Serial.printf("[CMD] start\n"); hge_captureStart(); }
		else if (c == 'x') { Serial.printf("[CMD] stop\n");  hge_captureStop();  }
		else if (c == 'i')
		{
			Serial.printf("[INFO] state=%s wifi=%d IP=%s\n",
			              stName(g_state),
			              (int)(WiFi.status() == WL_CONNECTED),
			              WiFi.localIP().toString().c_str());
		}
	}

	if (g_dirty)
	{
		g_dirty = false;
		draw();
	}
	delay(100);
}
