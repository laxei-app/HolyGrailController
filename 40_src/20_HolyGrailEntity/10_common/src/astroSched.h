#ifndef _ASTRO_SCHED_H_
#define _ASTRO_SCHED_H_
// 仕様書(10) 2.2 撮影計画自動生成。
// 撮影開始日時・場所・撮影方向(方位/仰角)・カメラ/レンズから、
// 日の出/日没/各薄明の時刻と「太陽が画角に入る時刻」を計算し、
// 撮影制御方法(夜間/朝日/夕日/日中/夜間前移行/夜間後移行)のスケジュールを生成する。
// 天文計算は Astronomy Engine をラップする。

#include "hgcCommon.h"
#include "ccm.h"
#include "cs.h"
#include "errorCode.h"
#include <memory>

namespace astro
{
	// 太陽の地平座標
	struct horiz
	{
		double azimuth  = 0.0;	// 方位[°] 0=北,90=東,180=南,270=西
		double altitude = 0.0;	// 高度[°] 地平線上が正
	};

	// 画角(度)
	struct fov
	{
		double h = 0.0;	// 水平画角[°]
		double v = 0.0;	// 垂直画角[°]
	};

	// 撮影制御方法のプロトタイプ一式(初期値/プリセットの受け渡しに使う)。
	// 【注意】buildSchedule はこれを受け取らない。区間に入れる実体は計画自身が所有する
	//  hgc::cs::ccm から取る(2026-08-11 改定)。外部の一式を注入できる口を残すと、別計画の
	//  設定が紛れ込む事故が起きるため(2026-08-08 実害)。
	struct ccmSet
	{
		std::shared_ptr<hgc::ccmNight>   night;
		std::shared_ptr<hgc::ccmSunrise> sunrise;
		std::shared_ptr<hgc::ccmSunset>  sunset;
		std::shared_ptr<hgc::ccmDay>     day;
	};

	// 経度から概算のUTCオフセット[分]を求める(政治的TZが不明な場合のフォールバック)。
	int defaultUtcOffsetMin(double longitude);

	// 指定ローカル時刻の太陽の地平座標を求める。
	horiz sunHoriz(const hgc::dateTime& localTime, int utcOffsetMin, const hgc::place& p);

	// 指定ローカル時刻の月の地平座標を求める(方位磁石の月マーカー用)。
	horiz moonHoriz(const hgc::dateTime& localTime, int utcOffsetMin, const hgc::place& p);

	// 太陽が指定高度になる時刻を求める。rising=false で日没側(下降中)、true で日の出側(上昇中)。
	// baseDate の日付の正午を起点に最大2日先まで探す。見つからなければ valid=false。
	// 固定露出太陽高度の start(日没側)/end(日の出側) 時刻計算に使う。
	struct altTime { bool valid; hgc::dateTime when; };
	altTime sunAltitudeTime(const hgc::place& p, const hgc::dateTime& baseDate,
	                        double altitude, bool rising, int utcOffsetMin);

	// カメラ/レンズと向き(landscape)から画角を求める。
	fov calcFov(const hgc::camera& cam, const hgc::lens& lens, bool landscape);

	// 撮影計画のスケジュールを生成する。
	//  plan       : start/end/place/azimuth/elevation/camera/lens/landscape と **plan.ccm** を
	//               入力とし、events と ccmList を生成して書き込む。窓に入れる撮影制御方法は
	//               plan.ccm が所有する実体を**複製せずに指す**。
	//  utcOffsetMin : ローカル時刻のUTCオフセット[分]。
	//  戻り値は必ず確認すること。終了≤開始のときは何も作らずに ERR_HGC_INVALID_ARG を返す。
	// 【オフセットは受け取らない(2026-09-03)】計画の時刻は plan.place.tzOffMin で解釈する。
	//  引数で渡せるようにしておくと、呼ぶ側が端末のタイムゾーンを渡してしまう(実際そうなっていた)。
	errCode buildSchedule(hgc::cs& plan);
}

#endif // _ASTRO_SCHED_H_
