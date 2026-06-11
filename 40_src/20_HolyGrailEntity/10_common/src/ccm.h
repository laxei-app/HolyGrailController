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
		moon,			// 月の影響への対処
		linear			// リニア移行(仕様3.7。ユーザー設定項目なし)
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
		// 撮影開始時の初期値を「明所限界(limitDark)」にするか。false なら「暗所限界(limitBright)」。
		// §4.4 最初の補正の起点。優先でなかった露出設定がどちら寄りになるかに影響する。
		bool         initialBright = true;

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
		bool   autoEdge    = true;	// 開始終了の自動判別

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

	// 3.6.1 月の影響への対処の種別
	enum class moonMode : uint8_t
	{
		none = 0,			// 補正しない
		autoBrightness,		// 画角全体の明るさで自動補正
		moonExposure		// 月自体に露出を合わせる
	};

	// 3.6.2 月の影響への対処
	struct ccmMoon : ccmBase
	{
		moonMode mode = moonMode::none;		// 対処方法の種別
		double   startLuminance = 0.0;		// 自動補正開始輝度。0～+3ev (autoBrightness)
		double   ev = 0.0;					// 目標露出補正。0～-10ev (autoBrightness)
		exposure initialExposure;			// 開始時露出設定 (moonExposure)
		bool     atmosphericExtinction = false;	// 大気消光補正 (moonExposure)
		double   extinctionCoef = 0.2;		// 大気消光係数。0.1～0.6
		bool     geocentricCorrection = false;	// 地心距離補正 (moonExposure)
		double   skyBrightnessCoef = 100.0;	// 月周辺の空の明るさ補正係数。0～100[%]

		ccmMoon() : ccmBase(ccmType::moon) {}
		std::unique_ptr<ccmBase> clone() const override { return std::make_unique<ccmMoon>(*this); }
	};

	// 3.6.3 月の影響への対処の初期値 (H-G曲線)
	struct moonExposureCurve
	{
		double fullMoonMag  = -12.7;	// 満月の最も明るい瞬間の等級
		double nightLineAge = 0.0;		// 夜間撮影線の月齢
		double nightLineMag = 0.0;		// 夜間撮影線の等級
	};
}

#endif // _CCM_H_
