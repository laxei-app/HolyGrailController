// エッジ端末(M5Stack)側の ETP サーバ実装(データ構造仕様書43 §6)。
#include "etpEdge.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <json/nlohmann/json.hpp>
#include <sys/time.h>
#include <ctime>
#include <cstdio>
#include <vector>

#include "etp.h"
#include "holyGrailEntity.h"
#include "hgcCommon.h"
#include "errorCode.h"
#include "debugOut.h"

using json = nlohmann::json;

namespace
{
	constexpr uint16_t PORT_DISCOVERY = 50505;
	constexpr uint16_t PORT_CONTROL   = 50506;

	WiFiUDP               g_udp;
	WiFiServer            g_server(PORT_CONTROL);
	WiFiClient            g_client;
	std::vector<uint8_t>  g_rx;		// TCP 受信バッファ(フレーミング用)
	std::string           g_name = "エッジ端末";

	// --- RTC(UTC保持運用) ---
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
		M5.Rtc.setDateTime(dt);
	}

	void setSystemClock(long long utc)
	{
		timeval tv{};
		tv.tv_sec  = static_cast<time_t>(utc);
		tv.tv_usec = 0;
		settimeofday(&tv, nullptr);
	}

	// 起動時: RTC(UTC) からシステム時計を復元する。
	void restoreClockFromRtc(void)
	{
		if (!M5.Rtc.isEnabled()) { return; }
		auto dt = M5.Rtc.getDateTime();
		hgc::dateTime d;
		d.year  = static_cast<uint16_t>(dt.date.year);
		d.month = static_cast<uint16_t>(dt.date.month);
		d.day   = static_cast<uint16_t>(dt.date.date);
		d.hour  = static_cast<uint16_t>(dt.time.hours);
		d.min   = static_cast<uint16_t>(dt.time.minutes);
		d.sec   = static_cast<uint16_t>(dt.time.seconds);
		long long utc = hgc::toUnixUtc(d, 0);	// RTC は UTC
		if (utc < 1577836800LL) { return; }		// 2020-01-01 より前は未設定とみなす
		setSystemClock(utc);
		DBGLN(col::GRN, "etpEdge: clock restored from RTC");
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
		j["ip"]    = std::string(WiFi.localIP().toString().c_str());
		j["port"]  = PORT_CONTROL;
		j["model"] = "エッジ端末";
		j["fw"]    = std::string(hge_version());
		j["state"] = hge_getState();
		return j.dump();
	}

	// 1 つの TCP 要求を処理して応答を返す。
	void handleTcp(const etp::packet& pk)
	{
		uint16_t rm = etp::M_ACK;
		std::string rd;
		switch (pk.cmd)
		{
		case etp::C_TIME:
			if (!applyTime(pk.data)) { rm = etp::M_NAK; }
			break;
		case etp::C_CAPTURE_PLAN:
			if (hge_setPlanJson(pk.data.c_str(), static_cast<int32_t>(pk.data.size())) != ERR_HGC_OK)
			{ rm = etp::M_NAK; }
			else { hge_savePlan(); }	// 受信した計画を永続化(スマホ切断後も単独動作・再起動で復元)
			break;
		case etp::C_CONTROL_METHOD:
			break;	// 将来用。現状は受領のみ(ccmListは計画に内包)
		case etp::C_ACTION:
			hge_captureStart();
			break;
		case etp::C_STOP:
			hge_captureStop();
			break;
		case etp::C_PROGRESS:
		{
			char b[256];
			int32_t len = sizeof(b);
			if (hge_getProgressJson(b, &len) == ERR_HGC_OK) { rd = b; }
			break;
		}
		case etp::C_DIRECTION:
			rd = "{\"azimuth\":0,\"elevation\":0}";
			break;
		default:
			rm = etp::M_NAK;
			break;
		}
		std::vector<uint8_t> out = etp::encode(pk.cmd, rm, rd);
		g_client.write(out.data(), out.size());
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

	// TCP 制御のポーリング
	void pollTcp(void)
	{
		if (!g_client || !g_client.connected())
		{
			g_client = g_server.available();
			if (g_client) { g_rx.clear(); }
			else          { return; }
		}
		while (g_client.available() > 0)
		{
			g_rx.push_back(static_cast<uint8_t>(g_client.read()));
		}
		size_t pos = 0;
		while (pos < g_rx.size())
		{
			etp::packet pk;
			int c = etp::decode(g_rx.data() + pos, g_rx.size() - pos, pk);
			if (c > 0)      { handleTcp(pk); pos += static_cast<size_t>(c); }
			else if (c == 0){ break; }		// データ不足
			else            { pos += 1; }	// 不正: 1バイト進めて再同期
		}
		if (pos > 0) { g_rx.erase(g_rx.begin(), g_rx.begin() + static_cast<long>(pos)); }
	}
}

namespace etpEdge
{
	void setup(const std::string& edgeName)
	{
		if (!edgeName.empty()) { g_name = edgeName; }
		restoreClockFromRtc();
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
