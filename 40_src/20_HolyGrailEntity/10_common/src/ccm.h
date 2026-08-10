#ifndef _CCM_H_
#define _CCM_H_
// データ構造仕様書(43) 第3章 撮影制御方法 (Capture Control Method = ccm)。
// すべての撮影制御方法は ccmBase からの派生とする。
// 露出設定(iso/ss/fn)の明暗限界と優先度は全方法が持つため基本クラスに置く(3.1.4)。

#include "hgcCommon.h"
#include <memory>

namespace hgc
{
	// 3.1.3 撮影制御方法の種別
	enum class ccmType : uint8_t
	{
		invalid = 0,	// 無効
		night,			// 夜間撮影
		sunrise,		// 朝日撮影(太陽直接撮影 朝)
		sunset,			// 夕日撮影(太陽直接撮影 夕)
		day,			// 日中撮影
		preNight,		// 夜間前移行(旧リニア移行, 仕様3.7。自動露出→夜間。ユーザー設定項目なし)
		postNight		// 夜間後移行(仕様3.9。夜間→次の自動露出へ逆優先度で移行)
	};

	// 3.1.4 撮影制御方法 基本クラス
	struct ccmBase
	{
		ccmType      type = ccmType::invalid;	// 種別
		std::string  name;						// ユーザー定義名称
		uint32_t     color = 0;					// RGB の色
		exposure     limitBright;				// 明るい側の限界(iso/ss/fn 上限)
		exposure     limitDark;					// 暗い側の限界(iso/ss/fn 下限)
		// 露出設定を変更する優先度(上位から先に変更)
		exposureType priority[exposureTypeNum] = { exposureType::iso, exposureType::ss, exposureType::fn };
		// 露出の基準(iso/ss/fn)。§4.4 最初の補正の起点・§4.5 往復対称の基準(home)。変数名 initial は維持。
		// 日中=明所限界/中間点/暗所限界から選択、朝日/夕日=明暗いずれかの限界、夜間=固定露出。
		// 夜間前/夜間後移行は実行時に決まる(保存しない)。
		exposure     initial;
		// 個別の露出平滑化(0=全体設定を使用)。朝日/夕日で個別指定する(§5.10 拡張)。
		double       hysteresis    = 0.0;	// ヒステリシス[段]。0=全体設定
		uint16_t     movingAverage = 0;		// 移動平均フレーム数。0=全体設定

		ccmBase() = default;
		explicit ccmBase(ccmType t) : type(t) {}
		virtual ~ccmBase() = default;
		// cs を自己完結コピーする際に使用する深いコピー
		virtual std::unique_ptr<ccmBase> clone() const { return std::make_unique<ccmBase>(*this); }
	};

	// 3.2 夜間撮影 (固定露出: limitBright == limitDark に固定露出値を設定)
	struct ccmNight : ccmBase
	{
		double sunAltitude = -18.0;	// 開始終了の太陽高度[°]
		double postNightEv = 0.0;	// 夜間後移行(仕様3.9/7.4.10)の露出補正[ev]。範囲 -5.0〜+5.0
		double preNightEv  = 0.0;	// 夜間前移行(仕様3.7)の露出補正[ev]。範囲 -5.0〜+5.0

		ccmNight() : ccmBase(ccmType::night) {}
		std::unique_ptr<ccmBase> clone() const override { return std::make_unique<ccmNight>(*this); }
	};

	// 3.3 朝日撮影 (自動露出)。太陽高度は撮り始め〜終わりの範囲で指定する。
	struct ccmSunrise : ccmBase
	{
		double sunAltitude    = -6.0;	// 撮り始めの太陽高度[°]
		double sunAltitudeEnd =  0.0;	// 終わりの太陽高度[°](朝日は昇るので終わりが明るい)
		double ev = -3.0;			// 露出補正。範囲 -5.0～+5.0、1/3刻み

		ccmSunrise() : ccmBase(ccmType::sunrise) {}
		std::unique_ptr<ccmBase> clone() const override { return std::make_unique<ccmSunrise>(*this); }
	};

	// 3.4 夕日撮影 (朝日撮影と時系列が逆。構成は同等)
	struct ccmSunset : ccmBase
	{
		double sunAltitude    =  0.0;	// 撮り始めの太陽高度[°](夕日は沈むので始まりが明るい)
		double sunAltitudeEnd = -6.0;	// 終わりの太陽高度[°]
		double ev = -3.0;			// 露出補正

		ccmSunset() : ccmBase(ccmType::sunset) {}
		std::unique_ptr<ccmBase> clone() const override { return std::make_unique<ccmSunset>(*this); }
	};

	// 3.5 日中撮影 (自動露出)
	struct ccmDay : ccmBase
	{
		double ev = 0.0;	// 露出補正。初期値±0

		ccmDay() : ccmBase(ccmType::day) {}
		std::unique_ptr<ccmBase> clone() const override { return std::make_unique<ccmDay>(*this); }
	};
}

#endif // _CCM_H_
