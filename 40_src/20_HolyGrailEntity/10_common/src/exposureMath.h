#ifndef _EXPOSURE_MATH_H_
#define _EXPOSURE_MATH_H_
// 仕様書(10) 4.2-4.5 の露出制御アルゴリズム。
//  - APEX 値(Sv/Av/Tv)の算出と 1/3 段量子化
//  - ヒストグラム中央値 → リニア輝度(sRGB逆補正)
//  - リニア輝度 ⇔ ev
//  - 露出設定(iso/ss/fn)の優先度・限界に従った 1/3 段ステップ制御
// カメラ I/O には依存しない純粋な計算モジュール(単体テスト可能)。

#include "hgcCommon.h"
#include "cameraData.h"	// cmdt::shotRange(デバイスが答える設定可能値と刻み)
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

class apiBase;	// 露出制御はデバイスを指すだけ(実体は .cpp で使う)

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
		// 下限(6%)。**薄明のいちばん暗い側の明るさを決めるのはこの値**(bm ではない)。
		//  2026-08-30 の通し(Edge01/R50V/曇天)で 0.025 から変更。夜明けの postNight は
		//  高度 -12°〜-4° の45分間ずっとここに貼り付き(実測 linear 0.019〜0.033)、そのあと
		//  15分で 0.18 まで駆け上がる。区間内で 2.84段 も振れて「前半だけ暗い」と見える。
		//  実測の環境光 Bv で試算すると 0.06 で振れ幅は 1.58段 に縮む。
		//  上限は 0.07 付近。postNight の露出上限は夜間の固定露出なので、それより明るい目標を
		//  置いても届かない(実測 04:16 の上限は linear 0.074)。0.08 以上にすると序盤が夜間露出に
		//  張り付いたままになり、シグモイド導入前(ev0=0.18固定)の挙動へ戻ってしまう。
		//  【朝夕で共通の値】上げると夕方の薄明後半も同じだけ明るくなる。元々「夕暮れが
		//  明るすぎる」を直すために入れた下限なので、変更後は夕方の通しでも確認すること。
		double linearLo = 0.06;
		double bm       = -3.7;   // 環境光の中心位置(実行時に太陽高度から算出し上書き。これは既定/フォールバック)
		double k        = 1.0;    // カーブの急峻さ
		// ② 中心 bm を太陽高度[°]から求める係数(暫定値。通しテスト+夕暮れで調整)。
		//   bm = clamp(bmHorizon - bmSlope*高度, bmDayClamp, bmTwilightClamp)。太陽が低いほど bm 大=薄明を暗く保つ。
		double bmHorizon       = 0.0;   // 高度0°(日の出/日の入)での中心
		double bmSlope         = 0.5;   // 高度1°あたりの bm 変化(高度↑で bm↓)
		double bmDayClamp      = -2.0;  // 太陽が高いときの下限
		double bmTwilightClamp = 3.0;   // 薄明/夜のときの上限
	};

	// 太陽高度[°] → ev0 シグモイド中心 bm(② 薄明ほど暗く保つ)。
	inline double ev0BmFromAltitude(double sunAltDeg, const ev0Sigmoid& s)
	{
		double bm = s.bmHorizon - s.bmSlope * sunAltDeg;
		if (bm < s.bmDayClamp)      { bm = s.bmDayClamp; }
		if (bm > s.bmTwilightClamp) { bm = s.bmTwilightClamp; }
		return bm;
	}
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

	// 【露出の軸の素性(2026-09-19)】デバイスが答える。上位は刻みという語彙を持たない。
	//  段の向きはどの軸も「大きいほど明るい」で揃える。
	//   iso = log2(ISO/100) / ss = log2(秒) / fn = -log2(F^2)
	struct axisInfo
	{
		double lo    = 0.0;	// 動ける下端[段](暗い側)
		double hi    = 0.0;	// 動ける上端[段](明るい側)。lo==hi なら動かない軸
		double notch = 0.0;	// **いまの位置での**丸めの粗さ[段]。0 = 無段(丸めない)
	};
	// 露出の一点を軸ごとの段で表す。has～ が偽の軸は「指定なし/読めない」。
	//  撮影制御方法の限界のように軸が欠けた露出を測るために使う。
	struct expoPoint
	{
		double iso = 0.0, ss = 0.0, fn = 0.0;
		bool   hasIso = false, hasSs = false, hasFn = false;
		double sum(void) const
		{ return (hasIso ? iso : 0.0) + (hasSs ? ss : 0.0) + (hasFn ? fn : 0.0); }
	};

	// 【段⇄実数(2026-09-19)】どの軸も「大きいほど明るい」で揃えた寄与[段]と、
	//  その軸の実数(ISO 値 / 秒 / F 値)を行き来する。
	//   iso = log2(ISO/100) / ss = log2(秒) / fn = -log2(F^2)
	//  **デバイス層が apiBase::expoAxes / expoResolve / expoStops を実装するための道具**。
	//  制御側(captureRunner / exposureCtl)は段しか扱わないので、ここは呼ばない。
	double stopsOfReal(double real, expoKind k);
	double realOfStops(double stops, expoKind k);

	// APEX 値を stepStops 段のグリッドに量子化する(最近傍)。
	//  【刻みは決め打ちしない(2026-09-07)】1/3 段はキヤノン機の語彙。内蔵カメラは 1/12 段。
	//  刻みはデバイスが答え(cmdt::shotRange::stepStops)、テーブルがそれを覚える(expoTables::stepStops)。
	double snapStops(double apex, double stepStops);
	// 1/3 段(旧来の呼び名。標準テーブルと、刻みを言わない呼び出しの既定)。
	double snapThird(double apex);

	// --- 設定可能値テーブル(データ構造仕様書43 §3.1.1.1 / 仕様書10 §4.2) ---
	enum class expoKind : uint8_t { iso, ss, fn };

	// テーブルの1要素。value=表示用の文字列(カメラの語彙)、real=論理値(実数)、apex=刻みに揃えたAPEX。
	//  計算は real と apex で行い、value は表示・ログ・計画の鍵にだけ使う。
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

	// 値文字列群からテーブルを作る(§4.2)。apex を算出し stepStops 段に揃え、real 昇順に並べる。無効値は除外。
	//  reals: 論理値(文字列と同じ並び)。null か長さ違いなら文字列を読み戻して実数にする。
	std::vector<expoEntry> buildTable(const std::vector<std::string>& values, expoKind k,
	                                  double stepStops = 1.0 / 3.0,
	                                  const std::vector<double>* reals = nullptr);

	// 標準テーブル用の値文字列(カメラ未接続時の編集用)。
	std::vector<std::string> standardValues(expoKind k);			// iso/ss(fnは下記)
	std::vector<std::string> standardFn(double fnMin, double fnMax);	// レンズf範囲の1/3段F値

	// 【上下限と刻みから編集用の目盛りを作る(2026-09-19)】
	//  カメラが答える並びをそのまま出すのはやめた。内蔵カメラは無段になり、
	//  並びが両端だけになったため、そのままでは「2 つしか選べない」状態になる(実機で発覚)。
	//  範囲はカメラ/レンズの実力、刻みは編集する人の好み、と役割を分ける。
	//  stepStops: 0.5 / 1/3 / 1/12 など。0 なら 1/3 段。
	std::vector<std::string> rangeValues(expoKind k, double stepStops, double loReal, double hiReal);

	// 初期値(プリセット)のエディタ用の目盛り(2026-09-06 仕様)。カメラに依らない。
	//  forPhone=真: 1/12 段。ss 48〜1/50000、F1.5〜3.5、ISO20〜12800(数値から作る)
	//  forPhone=偽: 1/3 段。ss 30〜1/16000、F0.5〜24、ISO100〜24000(慣用の表記を範囲で絞る)
	//  並びは実数の昇順(ss は速→遅)。
	std::vector<std::string> presetValues(expoKind k, bool forPhone);

	// iso/ss/fn 三つ分のテーブルと、その刻み[段]。
	struct expoTables
	{
		std::vector<expoEntry> iso, ss, fn;
		double stepStops = 1.0 / 3.0;	// 既定(軸ごとの指定が無いときに使う)
		// テーブルに無い値を評価するときも同じ刻みに揃える。軸ごとに違うことがある(2026-09-19)。
		double isoStep = 1.0 / 3.0;
		double ssStep  = 1.0 / 3.0;
		double fnStep  = 1.0 / 3.0;
	};
	// 標準テーブル一式(編集用)。fnはレンズの開放〜最小絞り範囲。1/3 段。
	expoTables standardTables(double fnMin = 1.0, double fnMax = 32.0);
	// デバイスが答えた設定可能値(文字列・論理値・刻み)からテーブル一式を作る。
	//  撮影で使うテーブルは必ずここを通す(刻みと論理値を落とさないため)。
	expoTables tablesFromRange(const cmdt::shotRange& r);

	// 【設定可能値の並びから露出ステップ[段]を測る(2026-09-19)】
	//  隣り合う値の APEX 差の**中央値**を返す。0=測れない(有効な値が 3 つ未満)。
	//  中央値にするのは、並びの端や Bulb、拡張感度のような例外に引きずられないため
	//  (実機 EOS R10 の ISO は 1 段刻みだが 25600→32000 だけ 0.32 段しか離れていない)。
	double medianStepStops(const std::vector<std::string>& values, expoKind k);

	// 【設定可能値の並びから露出ステップ[段]を見分ける(2026-09-19)】
	//  キヤノンはカメラ本体の設定で ss を 1/3 段と 1/2 段、ISO を 1/3 段と 1 段に切り替えられる。
	//  決め打ちにすると、1/2 段のカメラで 1 目盛りあたり 0.17 段の誤差が出る(実機 EOS R10 で確認)。
	//  中央値がよくある刻み(1/3・1/2・1)に近ければそれと見なす。
	//  見分けられない(値が少ない/見覚えのない刻み)ときは 0 か中央値をそのまま返す。
	//  **デバイスが刻みを答えられるならそちらが正**。これはその裏取りと、答えない機種の代替。
	double detectStepStops(const std::vector<std::string>& values, expoKind k);

	// 【デバイスの申告と並びが矛盾しないか(2026-09-19)】
	//  キヤノンは CCAPI で「本体に設定されている刻み」を答える(customfunction/...)。
	//  ただし答えるのは**本体メニューの設定**で、「実際に送れる値の並び」ではない。
	//  APEX の格子は送れる値と一致していないといけないので、鵜呑みにせずここで確かめる。
	//  並びの中央値と stepStops がずれていれば false(呼び出し側は測った値へ落とす)。
	//  値が少なくて測れないときは、否定する根拠が無いので true(申告を信じる)。
	bool stepMatchesValues(const std::vector<std::string>& values, expoKind k, double stepStops);

	// 露出(文字列)→明るさ(段)。テーブルでapexを引く(無ければ実数から算出)。大きいほど明るい。
	double brightnessStops(const hgc::exposure& e, const expoTables& t);

	// 【デッドゾーン制御(2026-09-08 ユーザー決定)】ヒステリシス帯からはみ出た分だけを返す[段]。
	//  predicted=いまの露出で写る明るさ(リニア)、linD/linU=帯の下端/上端(リニア)。
	//  帯の中なら 0。下に出ていれば +(明るくする量)、上に出ていれば −(暗くする量)。
	//  「中央まで戻す」のではなく「縁まで戻す」。場面がゆっくり変わる間は縁に沿って小刻みに追従し、
	//  帯の広さが段差の大きさにならない(以前は帯を越えた瞬間に中央までの差を一度に埋めていた)。
	double excessStops(double predicted, double linD, double linU);

	// ヒストグラム(輝度bin列)の中央値を 0.0～1.0(sRGB符号化)で返す。仕様 4.3.1。
	//  lumBins : 輝度ヒストグラム。nBins 個。
	//  戻り値  : 中央値の位置 /(nBins-1)。要素が無ければ 0。
	double histMedian(const uint16_t* lumBins, int nBins);

	// 露出設定を優先度・限界に従って動かす制御。仕様 4.4 / 4.5 / 7.4。
	//
	// 【無段階(ステップレス)制御 2026-09-19】
	//  内部は無段(実数)。守るのは上下限だけで、その間は計算したとおりの値を持つ。
	//
	// 【テーブルを持たない 2026-09-19 ユーザー決定】
	//  設定できる値の並びと刻みは**カメラの都合**なので、この層は一切持たない。
	//  デバイス(apiBase)に「段」で聞き、「段」で指示する。
	//   expoAxes    … 各軸の動ける範囲と、いまの位置での丸めの粗さ
	//   expoResolve … 望む段 → 実際に送る値(丸めるのはここだけ)
	//   expoStops   … 値 → 段(計画の限界・基準を同じものさしで測る)
	//  これにより、
	//   ・未知の刻みのカメラでも、真に無段のカメラでも、特別扱い無しで通る
	//   ・刻みを答えないカメラを勝手に 1/3 段と決めつける逃げ道が無くなる
	//   ・テーブルの複製が消える(以前は tables_ + 制御 4 個 + 同期撮影の台数ぶん)
	class exposureCtl
	{
	public:
		// デバイスと撮影制御方法の限界・優先度で初期化する。
		//  dev は撮影セッションの間ずっと生きていること(この器は参照を持つだけ)。
		//  戻り: デバイスが段で答えられなければ false(撮影を始めてはいけない)。
		bool init(apiBase* dev,
		          const hgc::exposure& limitBright,
		          const hgc::exposure& limitDark,
		          const hgc::exposureType priority[hgc::exposureTypeNum]);

		// 現在値を設定する(無段。デバイスの範囲の外へは出さない)。
		//  **撮影制御方法の限界の外にある値はそのまま受け取る**。窓の境目で前の窓の露出を
		//  引き継ぐときに使うので、ここで限界へ引き戻すと境目で段差になる(2026-08-29)。
		//  限界の外にいる間は「内へ戻る向き」だけ動ける。
		void setCurrent(const hgc::exposure& e);
		void setToBrightLimit();
		void setToDarkLimit();
		// 最長ss(=最も露出の多い側のss)の上限を maxSsSec[秒] に締める(仕様7.4.2)。
		void capLongestSs(double maxSsSec);

		// いまの露出。**デバイスが設定できるいちばん近い値**(カメラへ送るのはこれ)。
		hgc::exposure current() const { return cur_; }
		// 丸める前の無段の位置[段]。大きいほど明るい。
		double brightness() const;
		// 軸ごとの現在位置[段](丸める前)。配分の比較に使う。
		expoPoint point() const;
		// 丸めた後の明るさ[段]。current() を実際に乗せたときの明るさ。
		double appliedBrightness() const { return gotSum_; }

		// 丸めの粗さ[段]。いちばん細かい/いちばん粗い(動ける軸だけ)。
		//  無段の軸は 0 なので数えない。どの軸も無段なら 0 を返す。
		double minStepStops() const;
		double maxStepStops() const;

		// 【無段階で動かす】evStops 段ぶん(正=明るく)。優先度順に軸へ配り、上下限で止める。
		//  home != nullptr: home からずれている軸を優先度の逆順で先に戻す(§4.5 往復対称)。
		//   戻す軸は home を通り越さない。戻し終えてもまだ残っていれば通常の優先度順で配る。
		//  戻り: 実際に動いた量[段]。
		double moveStops(double evStops, const hgc::exposure* home = nullptr);

		// 1 目盛りだけ明るく/暗くする(優先度順に、動かせる軸を1つ)。
		//  無段の軸には目盛りが無いので amountStops を代わりに使う(0 なら動かない)。
		//  窓の境目の配分寄せ(migrateToward)と試験で使う。
		bool brighten(double amountStops = 0.0);
		bool darken(double amountStops = 0.0);
		// 軸を指名して動かす。目盛りがあればその1目盛り、無ければ amountStops。
		bool stepAxis(hgc::exposureType axis, bool bright, double amountStops = 0.0);

		// evStops 段ぶん露出を変更する(正=明るく)。無段。戻り値: 反映後の露出設定。
		hgc::exposure applyStops(double evStops);

	private:
		// 軸1本。値の並びは持たない。デバイスが答えた範囲と粗さ、それといまの位置だけ。
		struct axis
		{
			expoKind kind = expoKind::iso;
			double   b    = 0.0;			// いまの寄与(無段)
			double   tLo  = 0.0, tHi = 0.0;	// デバイスが出せる範囲
			double   bLo  = 0.0, bHi = 0.0;	// それを撮影制御方法の限界で締めたもの
			double   notch = 0.0;			// 丸めの粗さ[段]。0=無段
			bool     lim   = false;			// 限界が指定されているか(締めたか)
			double   limB = 0.0, limD = 0.0;	// 限界の段(明側/暗側)
			bool     hasLimB = false, hasLimD = false;
		};
		apiBase* dev_ = nullptr;
		axis     iso_, ss_, fn_;
		double   ssCap_ = 0.0;	// capLongestSs で締めた最長 ss[秒](0=無し)
		hgc::exposureType priority_[hgc::exposureTypeNum] =
			{ hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		hgc::exposure cur_{};
		double        gotSum_ = 0.0;	// current() を乗せたときの明るさ[段]

		void   refreshAxes();				// デバイスへ範囲と粗さを聞き直す
		void   recalcRanges();				// 範囲 ∩ 限界
		void   rebuildCurrent();			// b → デバイスが出せる値(cur_)
		double moveAxis(axis& a, double delta);
		axis&  axisRef(hgc::exposureType t);
		bool   stepOne(bool bright, double amountStops);
	};

	// 【窓の境目の配分寄せ 仕様 2026-08-29】いまの明るさを変えずに、iso/ss/fn の配分だけを
	//  「この撮影制御方法なら選ぶ組み合わせ」へ1目盛り近づける。明るい向きの軸と暗い向きの
	//  軸を1つずつ同時に動かすので明るさは変わらず、自動露出の1歩とは別枠で使える。
	//  もう合っている / 動かせる組が無い ときは false(何も変えない)。
	//  want は寄せ先を計算するための作業用の器。**呼ぶ側が窓ごとに1回だけ** ctl と同じ
	//  デバイス・限界・優先度・ss上限で init しておくこと。中身は毎回上書きする。
	//  amountStops = 目盛りを持たない軸(無段)を動かす量[段]。
	bool migrateToward(exposureCtl& ctl,
	                   exposureCtl& want,
	                   const hgc::exposure& initial,
	                   double amountStops);
}

#endif // _EXPOSURE_MATH_H_
