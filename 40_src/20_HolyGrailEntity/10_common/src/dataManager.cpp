#include "common.h"
#include "dataManager.h"

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
