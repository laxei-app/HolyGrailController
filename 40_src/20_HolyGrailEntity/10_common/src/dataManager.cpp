#include "common.h"
#include "dataManager.h"
#include "osFile.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>

// 出荷時設定の撮影制御方法一式(データ構造仕様書43 §3 / §7.2)。
astro::ccmSet dataManager::factoryCcmSet(void)
{
	astro::ccmSet set;

	auto night = std::make_shared<hgc::ccmNight>();
	night->name = "night";
	night->sunAltitude = -18.0;
	night->autoEdge = true;
	night->limitBright = night->limitDark = hgc::exposure{ 1600, 8.0, 1.4 };	// 固定露出(3.2)
	set.night = night;

	auto sunrise = std::make_shared<hgc::ccmSunrise>();
	sunrise->name = "sunrise";
	sunrise->sunAltitude = -6.0;
	sunrise->ev = -3.0;
	sunrise->limitBright = hgc::exposure{ 3200, 8.0,      1.4 };
	sunrise->limitDark   = hgc::exposure{ 100,  1.0 / 4000, 16.0 };
	set.sunrise = sunrise;

	auto sunset = std::make_shared<hgc::ccmSunset>();
	sunset->name = "sunset";
	sunset->sunAltitude = -6.0;
	sunset->ev = -3.0;
	sunset->limitBright = hgc::exposure{ 3200, 8.0,      1.4 };
	sunset->limitDark   = hgc::exposure{ 100,  1.0 / 4000, 16.0 };
	set.sunset = sunset;

	auto day = std::make_shared<hgc::ccmDay>();
	day->name = "day";
	day->ev = 0.0;
	day->limitBright = hgc::exposure{ 3200, 8.0,      1.4 };
	day->limitDark   = hgc::exposure{ 100,  1.0 / 4000, 16.0 };
	set.day = day;

	return set;
}

// 出荷時設定の露出平滑化(データ構造仕様書43 §5.10 の出荷時設定)。
hgc::exposureSmoothing dataManager::factorySmoothing(void)
{
	hgc::exposureSmoothing s;	// 既定値がそのまま出荷時設定(hysteresis=1.0, movingAverage=5)
	return s;
}

// 固定撮影計画の出荷時設定部分(場所=東京・機材=EOS R10 + 16mm 等)。
void dataManager::factoryFixedPlan(hgc::cs& plan)
{
	plan.name = "FixedPlan";

	plan.place.name = "Tokyo";
	plan.place.latitude  = 35.681;
	plan.place.longitude = 139.767;
	plan.place.altitude  = 40.0;

	plan.camera.maker = "Canon";
	plan.camera.model = "EOS R10";
	plan.camera.name  = "EOS R10";
	plan.camera.sensorSize  = 22.3;
	plan.camera.sensorPixel = 6000;

	plan.lens.maker = "Sigma";
	plan.lens.name  = "16mm F1.4 DC DN";
	plan.lens.focalLength = 16.0;
	plan.lens.fn = 1.4;

	plan.interval  = 15.0;	// 撮影周期[秒](EOS最小)
	plan.azimuth   = 90.0;	// 東向き
	plan.elevation = 10.0;
	plan.landscape = true;
}

// ============================================================================
//  動作ログ(データ構造仕様書43 §8)。固定長128Bテキストレコード/日次ファイル。
// ============================================================================
namespace
{
	int g_logOff = 0;	// ログのタイムスタンプ用 UTCオフセット[分]

	// rec の off から幅 w に s を詰める(left=左詰/右詰)。超過は切り捨て、余白は空白のまま。
	void putField(char* rec, int off, int w, const char* s, bool left)
	{
		int n = static_cast<int>(std::strlen(s));
		if (n > w) { n = w; }
		int dst = left ? off : (off + (w - n));
		std::memcpy(rec + dst, s, static_cast<size_t>(n));
	}

	// 現在のローカル時刻文字列(time 用19文字 と date 用10文字)を作る。
	// std::time(UTC秒) + オフセットを gmtime することでタイムゾーン非依存にローカル化する。
	void nowLocal(char timeStr[20], char dateStr[11])
	{
		std::time_t lt = std::time(nullptr) + static_cast<std::time_t>(g_logOff) * 60;
		std::tm g{};
#if defined(_WIN32)
		gmtime_s(&g, &lt);
#else
		gmtime_r(&lt, &g);
#endif
		std::snprintf(timeStr, 20, "%04d-%02d-%02d %02d:%02d:%02d",
		              g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
		std::snprintf(dateStr, 11, "%04d-%02d-%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
	}

	// 1レコード(128B固定長)を組み立てて日付ファイルへ追記する。
	// 各列の配置は §8.3 のとおり。空欄は空文字列を渡す。
	void writeRecord(const char* lvl, const char* event, const char* frame,
	                 const char* iso, const char* ss, const char* fn,
	                 const char* lum, const char* detail)
	{
		char rec[128];
		std::memset(rec, ' ', sizeof(rec));

		char timeStr[20], dateStr[11];
		nowLocal(timeStr, dateStr);

		std::memcpy(rec + 0, timeStr, 19);	rec[19] = '|';
		putField(rec, 20, 3,  lvl,    true);	rec[23] = '|';
		putField(rec, 24, 6,  event,  true);	rec[30] = '|';
		putField(rec, 31, 6,  frame,  false);	rec[37] = '|';
		putField(rec, 38, 5,  iso,    false);	rec[43] = '|';
		putField(rec, 44, 11, ss,     true);	rec[55] = '|';
		putField(rec, 56, 6,  fn,     true);	rec[62] = '|';
		putField(rec, 63, 8,  lum,    false);	rec[71] = '|';
		putField(rec, 72, 55, detail, true);
		rec[127] = '\n';

		std::string dir = osfile::logDir();
		if (dir.empty()) { return; }
		std::string path = dir + "/hg_" + dateStr + ".log";
		osfile::append(path, rec, sizeof(rec));
	}

	// シャッター速度を読みやすい固定幅向け文字列にする(1未満は 1/N 表記)。
	void formatSs(double ss, char* buf, size_t n)
	{
		if (ss > 0.0 && ss < 1.0) { std::snprintf(buf, n, "1/%ld", std::lround(1.0 / ss)); }
		else                      { std::snprintf(buf, n, "%.6f", ss); }
	}
}

void dataManager::setLogOffset(int utcOffsetMin)
{
	g_logOff = utcOffsetMin;
}

void dataManager::logEvent(const char* event, const char* detail, bool error)
{
	writeRecord(error ? "ERR" : "INF", event ? event : "", "", "", "", "", "",
	            detail ? detail : "");
}

void dataManager::logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName)
{
	char frameStr[8], isoStr[8], ssStr[16], fnStr[8], lumStr[12];
	std::snprintf(frameStr, sizeof(frameStr), "%d", frame);
	std::snprintf(isoStr,   sizeof(isoStr),   "%u", e.iso);
	formatSs(e.ss, ssStr, sizeof(ssStr));
	std::snprintf(fnStr,    sizeof(fnStr),    "%.1f", e.fn);
	std::snprintf(lumStr,   sizeof(lumStr),   "%+.3f", lumStops);
	writeRecord("INF", "SHOT", frameStr, isoStr, ssStr, fnStr, lumStr, ccmName ? ccmName : "");
}
