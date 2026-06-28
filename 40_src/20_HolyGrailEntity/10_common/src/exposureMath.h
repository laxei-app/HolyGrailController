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
#include <string>
#include <vector>

namespace expo
{
	// 昼間のリニア輝度 18%。環境光 Bv 算出の基準(APEX)に使う(仕様 4.3.3)。
	inline constexpr double DAY_LINEAR_18 = 0.18;

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

	// --- APEX 値 仕様 4.2 ---
	inline double svFromIso(double iso) { return std::log2(iso / 100.0); }	// 感度
	inline double avFromFn (double fn)  { return std::log2(fn * fn); }		// 絞り
	inline double tvFromSs (double ss)  { return std::log2(1.0 / ss); }		// 時間

	// 文字列→実数。iso:整数, fn:小数, ss:"1/4000"/"8"/"0.5"等。無効("Bulb"等)は負を返す。
	enum class expoKind : uint8_t;
	double parseValue(const std::string& v, expoKind k);

	// === ev0(人が適正と感じる露出)のリニア輝度 — 環境光依存 仕様 4.3.3 / 4.3.4 ===
	// 旧仕様は ev0 を固定で 18%(0.18) としていたが、環境光(Bv)が暗いほど人が適正と感じる
	// リニア輝度は下がる(夕暮れが明るすぎる問題への対処)。Bv→ev0リニア輝度をシグモイドで求める。

	// 環境光 Bv(APEX) 仕様 4.3.3。測光リニア輝度と、その測光時の露出設定(APEX)から求める。
	//  Bv = Av + Tv - Sv + log2(linear / 0.18)
	inline double ambientBv(double linear, double av, double tv, double sv)
	{
		return av + tv - sv + std::log2(linear / DAY_LINEAR_18);
	}

	// ev0 リニア輝度のシグモイド係数 仕様 4.3.4(実装時に調整しうるので変更可能にする)。
	struct ev0Sigmoid
	{
		double linearHi = 0.18;   // 上限(昼間 18%)
		double linearLo = 0.025;  // 下限(2.5%)
		double bm       = -3.7;   // 環境光の中心位置
		double k        = 1.0;    // カーブの急峻さ
	};
	// プロセス共通の調整可能インスタンス(将来 設定/アセットから上書きできるよう参照を返す)。
	inline ev0Sigmoid& ev0Cfg() { static ev0Sigmoid c; return c; }

	// Bv → ev0 のリニア輝度 仕様 4.3.4。
	//  linear0 = lo + (hi - lo) / (1 + exp(-k(Bv - Bm)))
	inline double ev0LinearFromBv(double bv, const ev0Sigmoid& s)
	{
		return s.linearLo + (s.linearHi - s.linearLo) / (1.0 + std::exp(-s.k * (bv - s.bm)));
	}

	// 測光リニア輝度とその露出(iso/ss/fn)から ev0 のリニア輝度を求める(4.3.3 + 4.3.4)。
	// 露出値が無効なら従来どおり 18% を ev0 リニア輝度として返す(安全側)。
	double ev0LinearForMeasure(double linear, const hgc::exposure& meteredAt, const ev0Sigmoid& s);

	// ev に対応する目標リニア輝度。ev0 リニア輝度(環境光依存)を基準に算出する。
	inline double linearFromEvBase(double ev, double linear0)
	{
		return linear0 * std::pow(2.0, ev);
	}

	// APEX 値を 1/3 段グリッドに量子化する(最近傍)。
	double snapThird(double apex);

	// --- 設定可能値テーブル(データ構造仕様書43 §3.1.1.1 / 仕様書10 §4.2) ---
	enum class expoKind : uint8_t { iso, ss, fn };

	// テーブルの1要素。value=カメラ設定値文字列、real=実数、apex=1/3段スナップしたAPEX。
	struct expoEntry
	{
		std::string value;
		double      real = 0.0;
		double      apex = 0.0;
	};

	// 文字列→実数。iso:整数, fn:小数, ss:"1/4000"/"8"/"0.5"等。無効("Bulb"等)は負を返す。
	double parseValue(const std::string& v, expoKind k);

	// NPF ルールのシャッター速度[秒](点像を保つ目安。仕様 7.3.3 参考表示)。
	//  sensorW_mm: センサー横[mm], pixelW: センサー横[pixel], focal_mm: 焦点距離[mm], fn: 開放F値。
	//  t = (35*N + 30*p) / f、p[µm] = sensorW_mm / pixelW * 1000。算出できなければ 0。
	double npfShutterSec(double sensorW_mm, double pixelW, double focal_mm, double fn);

	// 値文字列群からテーブルを作る(§4.2)。apexを算出し1/3段にスナップ、apex昇順ソート。無効値は除外。
	std::vector<expoEntry> buildTable(const std::vector<std::string>& values, expoKind k);

	// 標準テーブル用の値文字列(カメラ未接続時の編集用)。
	std::vector<std::string> standardValues(expoKind k);			// iso/ss(fnは下記)
	std::vector<std::string> standardFn(double fnMin, double fnMax);	// レンズf範囲の1/3段F値

	// iso/ss/fn 三つ分のテーブル。
	struct expoTables
	{
		std::vector<expoEntry> iso, ss, fn;
	};
	// 標準テーブル一式(編集用)。fnはレンズの開放〜最小絞り範囲。
	expoTables standardTables(double fnMin = 1.0, double fnMax = 32.0);

	// 露出(文字列)→明るさ(段)。テーブルでapexを引く(無ければ実数から算出)。大きいほど明るい。
	double brightnessStops(const hgc::exposure& e, const expoTables& t);

	// ヒストグラム(輝度bin列)の中央値を 0.0～1.0(sRGB符号化)で返す。仕様 4.3.1。
	//  lumBins : 輝度ヒストグラム。nBins 個。
	//  戻り値  : 中央値の位置 /(nBins-1)。要素が無ければ 0。
	double histMedian(const uint16_t* lumBins, int nBins);

	// 露出設定を優先度・限界に従って 1/3 段ずつ増減させる制御。
	// 仕様 4.4(最初の補正)・4.5(露出補正)・7.4(優先度と限界)。
	class exposureCtl
	{
	public:
		// 設定可能値テーブルと撮影制御方法の限界・優先度で初期化する。
		//  tables                : iso/ss/fn の設定可能値テーブル(apex昇順)
		//  limitBright/limitDark : 明側/暗側の限界(iso/ss/fn の文字列。空="限界なし")
		//  priority              : 変更する優先度(上位から先に変更)
		void init(const expoTables& tables,
		          const hgc::exposure& limitBright,
		          const hgc::exposure& limitDark,
		          const hgc::exposureType priority[hgc::exposureTypeNum]);

		// 現在値を設定する(各テーブルの最も近い値にスナップする)。
		void setCurrent(const hgc::exposure& e);
		// 明側/暗側の限界を基準にする(仕様 4.4)。
		void setToBrightLimit();
		void setToDarkLimit();

		hgc::exposure current() const { return cur_; }

		// 1/3 段 明るく/暗くする。優先度順(reversePriority=true で逆順)に限界まで変更。変更不可なら false。
		// 逆順は §4.5 の往復対称(基準へ近づくときは優先度の逆で戻す)に使う。
		bool brighten(bool reversePriority = false);
		bool darken(bool reversePriority = false);

		// home(基準)へ戻る向きの 1/3 段。home からずれている軸を優先度の逆順で先に戻し、
		// home にある軸は飛び越えない(離れたときと逆順に巻き戻す=往復で同じ組合せ。§4.5)。
		// bright=戻る向き(home が現在より明るければ true)。
		bool stepHome(bool bright, const hgc::exposure& home);

		// evStops 段ぶん露出を変更する(正=明るく)。1/3段刻みで反映。
		// 戻り値: 反映後の露出設定。
		hgc::exposure applyStops(double evStops);

	private:
		struct ladder { std::vector<expoEntry> e; int idx = 0; }; // apex昇順と現在位置
		ladder iso_, ss_, fn_;	// iso/ss は idx↑で明るい、fn は idx↑で暗い
		// 限界の実数(0=限界なし)。real が 0 になる露出値は無いので 0 を番兵に使う。
		double limBIso_ = 0, limDIso_ = 0, limBSs_ = 0, limDSs_ = 0, limBFn_ = 0, limDFn_ = 0;
		hgc::exposureType priority_[hgc::exposureTypeNum] =
			{ hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		hgc::exposure cur_{};

		void rebuildCurrent();
		bool stepIso(bool bright);
		bool stepSs(bool bright);
		bool stepFn(bool bright);
		bool stepOne(bool bright, bool reverse = false);
	};
}

#endif // _EXPOSURE_MATH_H_
