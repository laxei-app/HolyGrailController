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
	// 値はカメラから取得した「設定値の文字列」で保持・通信・保存・カメラ指示する。
	// 露出計算には文字列を直接使わず、設定可能値テーブル(expoTable)で APEX/実数へ変換する。
	//  iso 例 "100","3200" / ss 例 "1/4000","8","Bulb" / fn 例 "1.4","16"
	struct exposure
	{
		std::string iso;	// iso感度のカメラ設定値
		std::string ss;		// シャッター速度のカメラ設定値
		std::string fn;		// F値のカメラ設定値
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
		std::string memo;			// メモ(説明。撮影計画リストでは使わない補足)
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
		std::string serial;				// シリアルNo.(接続時に自動取得・保存。識別用)
		std::string friendly;			// フレンドリ名(愛称。接続時に自動取得)
		double   sensorSize  = 0.0;		// センサー横[mm]
		double   sensorSizeV = 0.0;		// センサー縦[mm]
		uint32_t sensorPixel = 0;		// センサー横[pixel]
		std::vector<std::string> isoList;	// 設定可能iso感度(カメラ設定値の文字列)
		std::vector<std::string> ssList;	// 設定可能シャッター速度(カメラ設定値の文字列)
		// 測光にライブビューを主体で使う機種か。既定(false)は「サムネイルだけ」。
		//  撮影済みサムネイルの取得は最も正確だが、機種によっては取得回数に上限があり
		//  (EOS R10 は電源投入あたり 200 回程度で応答しなくなる)一晩持たない。
		//  そういう機種だけ true にして、普段はライブビューで測り、ライブビューでは
		//  追随できない明るさのときだけサムネイルへ落ちる。マスタ既定は camera_body_list.json の
		//  "meter_lv"、機体ごとの上書きは所持カメラの編集画面から。
		bool meterLv = false;
		// ダイジェスト認証(カメラ側の設定で有効にできる)。空なら認証なしの機体。
		//  authPass は**平文で持つ**。保存するときだけ secret::encrypt() を通す
		//  (ファイル上は "v1:..." の暗号文。詳細は secret.h)。
		std::string authUser;			// ユーザーID
		std::string authPass;			// パスワード(メモリ上は平文)
	};

	// 5.3 レンズ
	struct lens
	{
		std::string maker;			// メーカー名
		std::string name;			// レンズ名称
		double focalLength = 0.0;	// 焦点距離[mm]
		double fn = 0.0;			// 開放F値(F最小)
		double fnMax = 0.0;			// 最小絞り(F最大)。0=未設定
		bool   hasContact = true;	// 電子接点有無
		bool   fisheye = false;		// 魚眼レンズ(投影方式=等距離)。マスタ lenses_list.json の "fisheye" 由来
	};

	// 5.5 所持カメラ(camera + 組み合わせるレンズ + 自動挿入)
	struct ownedCamera
	{
		camera            cam;					// カメラ情報(JSONキーは "camera")
		std::vector<lens> lensList;				// 組み合わせるレンズ(先頭が初期値)
		bool              autoInsert = false;	// 撮影計画に自動的に挿入する
	};

	// 5.10 露出平滑化
	struct exposureSmoothing
	{
		double   hysteresis    = 1.0;	// ヒステリシス[段]。範囲 1/3～3
		uint16_t movingAverage = 5;		// 移動平均フレーム数。範囲 1～10
	};

	// --- 時刻ユーティリティ(プラットフォーム非依存) ---
	// 1970-01-01 からの日数(Howard Hinnant の civil アルゴリズム)。
	inline long long daysFromCivil(int y, unsigned m, unsigned d)
	{
		y -= (m <= 2);
		const int era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned mp = (m > 2) ? (m - 3u) : (m + 9u);	// 3月起点の月(0..11)
		const unsigned doy = (153u * mp + 2u) / 5u + d - 1u;
		const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
		return static_cast<long long>(era) * 146097 + static_cast<long long>(doe) - 719468;
	}

	// ローカル日時 + UTCオフセット[分] → Unix時刻(UTC秒)。
	inline long long toUnixUtc(const dateTime& t, int utcOffsetMin)
	{
		long long days = daysFromCivil(t.year, t.month, t.day);
		long long localSec = days * 86400LL + t.hour * 3600LL + t.min * 60LL + t.sec;
		return localSec - static_cast<long long>(utcOffsetMin) * 60LL;
	}

	// Unix時刻(UTC秒) + UTCオフセット[分] → ローカル日時(toUnixUtc の逆。Hinnant civil_from_days)。
	inline dateTime fromUnixUtc(long long unixSec, int utcOffsetMin)
	{
		long long localSec = unixSec + static_cast<long long>(utcOffsetMin) * 60LL;
		long long days = localSec / 86400LL;
		long long rem  = localSec - days * 86400LL;
		if (rem < 0) { rem += 86400LL; days -= 1; }
		long long z = days + 719468;
		long long era = (z >= 0 ? z : z - 146096) / 146097;
		unsigned  doe = static_cast<unsigned>(z - era * 146097);
		unsigned  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		long long y   = static_cast<long long>(yoe) + era * 400;
		unsigned  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		unsigned  mp  = (5 * doy + 2) / 153;
		unsigned  d   = doy - (153 * mp + 2) / 5 + 1;
		unsigned  m   = mp < 10 ? mp + 3 : mp - 9;
		y += (m <= 2);
		dateTime t{};
		t.year  = static_cast<uint16_t>(y);
		t.month = static_cast<uint16_t>(m);
		t.day   = static_cast<uint16_t>(d);
		t.hour  = static_cast<uint16_t>(rem / 3600);
		t.min   = static_cast<uint16_t>((rem % 3600) / 60);
		t.sec   = static_cast<uint16_t>(rem % 60);
		return t;
	}
}

#endif // _HGC_COMMON_H_
