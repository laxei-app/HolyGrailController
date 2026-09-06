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
		// 【スマホ向け(2026-09-06 仕様)】初期値のエディタが使う目盛りと範囲を切り替える印。
		//  真: 1/12 段、ss 48〜1/50000・F1.5〜3.5・ISO20〜12800 / 偽: 1/3 段、ss 30〜1/16000・
		//  F0.5〜24・ISO100〜24000。値そのものの意味は変えない(計画へ取り込むときはカメラの目盛りへ寄せる)。
		bool         forPhone      = false;

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
	// 3.7 撮影計画が所有する撮影制御方法一式(2026-08-11 改定)
	//
	// 【なぜ計画が持つか】以前は撮影制御方法をグローバルに1組だけ持ち、スケジュールを引き直すたびに
	//  そこから窓の中身を作り直していた。計画ごとに違う設定を持てないうえ、エッジでは受信した計画に
	//  「直前に受け取った別の計画」の設定が貼り付いて保存される事故が起きた(2026-08-08 実害)。
	//  計画が実体を所有し、ここだけを権威とする。窓(ccmWindow)はこの実体を指すだけで複製しない。
	//
	// 【初期値から取り込むのは2か所だけ】計画の新規作成時と、ユーザーが初期値リストから選んだとき。
	//  画角・撮影場所・開始時刻・機材を変えても取り込み直さない(窓の時刻を引き直すだけ)。
	//
	// 【使う/使わない】4種とも同じ形で used を持つ。使わないことにしても実体は保持し続けるので、
	//  もう一度使うことにしたときユーザーが編集した内容がそのまま復活する(初期値へは戻さない)。
	//  夜間/日中を使わないと時間帯に穴が空くため、UI では朝日/夕日だけを切り替えさせる。
	struct ccmOwned
	{
		std::shared_ptr<ccmNight>   night;
		std::shared_ptr<ccmSunrise> sunrise;
		std::shared_ptr<ccmSunset>  sunset;
		std::shared_ptr<ccmDay>     day;
		bool useNight   = true;
		// 朝日/夕日(太陽を直接撮る)は既定で**使わない**(2026-08-17 ユーザー指示)。
		//  太陽が画角に入る構図でないと意味がなく、既定で入れておくと薄明の連続性を
		//  分断してしまう(日中が朝日で割られる既知の課題も同根)。使う人が明示的に入れる。
		bool useSunrise = false;
		bool useSunset  = false;
		bool useDay     = true;

		// 4種そろっているか(欠けていたら初期値の取り込みが必要)。
		bool complete(void) const { return night && sunrise && sunset && day; }

		std::shared_ptr<ccmBase> get(ccmType t) const
		{
			switch (t)
			{
			case ccmType::night:   return night;
			case ccmType::sunrise: return sunrise;
			case ccmType::sunset:  return sunset;
			case ccmType::day:     return day;
			default:               return nullptr;
			}
		}

		bool used(ccmType t) const
		{
			switch (t)
			{
			case ccmType::night:   return useNight;
			case ccmType::sunrise: return useSunrise;
			case ccmType::sunset:  return useSunset;
			case ccmType::day:     return useDay;
			default:               return true;	// 移行(夜間前/後)は常に使う
			}
		}

		void setUsed(ccmType t, bool v)
		{
			switch (t)
			{
			case ccmType::night:   useNight   = v; break;
			case ccmType::sunrise: useSunrise = v; break;
			case ccmType::sunset:  useSunset  = v; break;
			case ccmType::day:     useDay     = v; break;
			default: break;
			}
		}

		// 型に対応する実体を差し替える(初期値リストからの選択で使う)。used は変えない。
		void set(ccmType t, const std::shared_ptr<ccmBase>& c)
		{
			if (!c) { return; }
			switch (t)
			{
			case ccmType::night:   night   = std::static_pointer_cast<ccmNight>(c);   break;
			case ccmType::sunrise: sunrise = std::static_pointer_cast<ccmSunrise>(c); break;
			case ccmType::sunset:  sunset  = std::static_pointer_cast<ccmSunset>(c);  break;
			case ccmType::day:     day     = std::static_pointer_cast<ccmDay>(c);     break;
			default: break;
			}
		}
	};
}

#endif // _CCM_H_
