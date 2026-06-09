#ifndef _HGC_COMMON_H_
#define _HGC_COMMON_H_
// データ構造仕様書(43) 第2,5章の共通データ型。
// namespace は仕様 1.1 に従い hgc。json の key は構造体メンバー名と同じとする。

#include <cstdint>
#include <string>
#include <vector>

namespace hgc
{
	// 2.1 日時
	struct dateTime
	{
		uint16_t year  = 0;	// 年
		uint16_t month = 0;	// 月
		uint16_t day   = 0;	// 日
		uint16_t hour  = 0;	// 時
		uint16_t min   = 0;	// 分
		uint16_t sec   = 0;	// 秒
	};

	// 3.1.1 露出設定 (iso/ss/fn の三つ組)
	struct exposure
	{
		uint16_t iso = 0;	// iso感度
		double   ss  = 0.0;	// シャッター速度[秒]
		double   fn  = 0.0;	// F値
	};

	// 3.1.2 露出設定項目。優先度の指定に使用する。
	enum class exposureType : uint8_t
	{
		iso = 0,	// iso感度
		ss,			// シャッター速度
		fn,			// F値
		NUM			// 項目数(=3)
	};
	inline constexpr int exposureTypeNum = 3;

	// 5.1 場所
	struct place
	{
		std::string name;			// 場所の名称
		double latitude   = 0.0;	// 緯度[°]
		double longitude  = 0.0;	// 経度[°]
		double altitude   = 0.0;	// 標高[m]
		bool   autoInsert = false;	// 撮影計画に自動挿入する
	};

	// 5.2 カメラ
	struct camera
	{
		std::string maker;				// メーカー名
		std::string model;				// モデル名
		std::string name;				// 表示名称
		double   sensorSize  = 0.0;		// センサー横[mm]
		uint32_t sensorPixel = 0;		// センサー横[pixel]
		std::vector<uint16_t> isoList;	// 設定可能iso感度
		std::vector<double>   ssList;	// 設定可能シャッター速度
	};

	// 5.3 レンズ
	struct lens
	{
		std::string maker;			// メーカー名
		std::string name;			// レンズ名称
		double focalLength = 0.0;	// 焦点距離[mm]
		double fn = 0.0;			// 開放F値
		bool   hasContact = true;	// 電子接点有無
	};

	// 5.10 露出平滑化
	struct exposureSmoothing
	{
		double   hysteresis    = 1.0;	// ヒステリシス[段]。範囲 1/3～3
		uint16_t movingAverage = 5;		// 移動平均フレーム数。範囲 1～10
	};
}

#endif // _HGC_COMMON_H_
