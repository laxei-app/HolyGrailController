#ifndef _EXPOSURE_MATH_H_
#define _EXPOSURE_MATH_H_
// 仕様書(10) 4.2-4.5 の露出制御アルゴリズム。
//  - APEX 値(Sv/Av/Tv)の算出と 1/3 段量子化
//  - ヒストグラム中央値 → リニア輝度(sRGB逆補正)
//  - リニア輝度 ⇔ ev
//  - 露出設定(iso/ss/fn)の優先度・限界に従った 1/3 段ステップ制御
// カメラ I/O には依存しない純粋な計算モジュール(単体テスト可能)。

#include "hgcCommon.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace expo
{
	// ev0(中庸グレー)のリニア輝度。仕様 4.3: ヒストグラム中央値 0.18 が ev0。
	inline constexpr double EV0_LINEAR = 0.18;

	// --- sRGB 逆補正(デガンマ) 仕様 4.3.2 ---
	inline double srgbToLinear(double x)
	{
		if (x <= 0.04045) { return x / 12.92; }
		return std::pow((x + 0.055) / 1.055, 2.4);
	}

	// --- リニア輝度 ⇔ ev 仕様 4.3.2 ---
	// 今の輝度 linearN から目標輝度 linearT までの ev 差(段)。
	inline double evFromLinear(double linearN, double linearT)
	{
		return std::log2(linearT / linearN);
	}
	// ev に対応するリニア輝度(ev0=0.18 基準)。
	inline double linearFromEv(double ev)
	{
		return EV0_LINEAR * std::pow(2.0, ev);
	}

	// --- APEX 値 仕様 4.2 ---
	inline double svFromIso(double iso) { return std::log2(iso / 100.0); }	// 感度
	inline double avFromFn (double fn)  { return std::log2(fn * fn); }		// 絞り
	inline double tvFromSs (double ss)  { return std::log2(1.0 / ss); }		// 時間

	// 画像の明るさ(段)。大きいほど明るい。Sv が上がると明るく、Av/Tv が上がると暗い。
	inline double brightnessStops(const hgc::exposure& e)
	{
		return svFromIso(static_cast<double>(e.iso)) - avFromFn(e.fn) - tvFromSs(e.ss);
	}

	// APEX 値を 1/3 段グリッドに量子化する(最近傍)。
	double snapThird(double apex);

	// ヒストグラム(輝度bin列)の中央値を 0.0～1.0(sRGB符号化)で返す。仕様 4.3.1。
	//  lumBins : 輝度ヒストグラム。nBins 個。
	//  戻り値  : 中央値の位置 /(nBins-1)。要素が無ければ 0。
	double histMedian(const uint16_t* lumBins, int nBins);

	// 露出設定を優先度・限界に従って 1/3 段ずつ増減させる制御。
	// 仕様 4.4(最初の補正)・4.5(露出補正)・7.4(優先度と限界)。
	class exposureCtl
	{
	public:
		// カメラの設定可能値と撮影制御方法の限界・優先度で初期化する。
		//  isoList/ssList/fnList : カメラから取得した設定可能値(順不同で可)
		//  limitBright/limitDark : 明側/暗側の限界(iso/ss/fn)
		//  priority              : 変更する優先度(上位から先に変更)
		void init(const std::vector<uint16_t>& isoList,
		          const std::vector<double>&   ssList,
		          const std::vector<double>&   fnList,
		          const hgc::exposure& limitBright,
		          const hgc::exposure& limitDark,
		          const hgc::exposureType priority[hgc::exposureTypeNum]);

		// 現在値を設定する(各リストの最も近い値にスナップする)。
		void setCurrent(const hgc::exposure& e);
		// 明側/暗側の限界を初期値にする(仕様 4.4)。
		void setToBrightLimit();
		void setToDarkLimit();

		hgc::exposure current() const { return cur_; }

		// 1/3 段 明るく/暗くする。優先度順に限界まで変更。変更できなければ false。
		bool brighten();
		bool darken();

		// evStops 段ぶん露出を変更する(正=明るく)。1/3段刻みで反映。
		// 戻り値: 反映後の露出設定。
		hgc::exposure applyStops(double evStops);

	private:
		struct ladder { std::vector<double> vals; int idx = 0; }; // 昇順値と現在位置
		ladder iso_, ss_, fn_;	// iso/ss は idx↑で明るい、fn は idx↑で暗い
		hgc::exposure limitBright_{};
		hgc::exposure limitDark_{};
		hgc::exposureType priority_[hgc::exposureTypeNum] =
			{ hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		hgc::exposure cur_{};

		void rebuildCurrent();
		bool stepIso(bool bright);
		bool stepSs(bool bright);
		bool stepFn(bool bright);
		bool stepOne(bool bright);
	};
}

#endif // _EXPOSURE_MATH_H_
