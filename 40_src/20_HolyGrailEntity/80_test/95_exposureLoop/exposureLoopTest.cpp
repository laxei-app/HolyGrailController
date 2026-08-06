// 露出制御ループの回帰テスト(カメラ不要・cl.exe で単体実行)。
//
// 【背景】2026-07-20 夕方の通しで、R10/R100 とも撮影露出が 1/3段/コマ で単調に暴走し、
//  実写が飽和85〜98%(白飛び)→ 明所限界で黒潰れになった。原因は「測光露出で測った明るさ」を
//  「撮影露出で撮るべき明るさ(ev0基準)」とそのまま比較していたこと。測光ループは中央値を
//  kMeterTargetX(0.35)に貼り付けるため、測光値は撮影露出を動かしても変化せず、比較結果が
//  永久に同じ向きのまま=フィードバックが閉じない。
//
// 【このテストが固定する仕様】
//  ・測光値は露出成分を割り戻して「場面の明るさ」にし、撮影露出へ投影してから比較する。
//  ・その結果、実測ログの条件で目標が約 1/64 秒に収束し、暴走しない。
//  ・旧ロジックは同条件で必ず「明るく」を返し続ける(=バグの再現)。
//
// 実測値の出典: hg_2026-07-20.log fr25 (18:31:25)
//   測光 linear=0.1145 / 測光露出 ISO100・1/100・f1.4 / 撮影露出 ISO100・4s・f1.4 / 目標ev=+0.0
//   実写 IMG_1092.CR3 は 1/100 で撮れており、中央値 0.359(sRGB) = 適正だった。

#include "exposureMath.h"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* name, const char* detail = "")
{
	std::printf("%s : %s %s\n", ok ? "PASS" : "**FAIL**", name, detail);
	if (!ok) { ++g_fail; }
}
static void checkNear(double got, double want, double tol, const char* name)
{
	char d[160];
	std::snprintf(d, sizeof(d), "(got=%.4f want=%.4f tol=%.4f)", got, want, tol);
	check(std::fabs(got - want) <= tol, name, d);
}

// --- captureRunner の実装と同じ式(土俵合わせ) ---
static double sceneRefFromMetered(double linear, const hgc::exposure& meterExp, const expo::expoTables& t)
{
	if (linear <= 0.0) { return -1.0; }
	return linear / std::pow(2.0, expo::brightnessStops(meterExp, t));
}
static double linearAtExposure(double sceneRef, const hgc::exposure& e, const expo::expoTables& t)
{
	if (sceneRef <= 0.0) { return -1.0; }
	return sceneRef * std::pow(2.0, expo::brightnessStops(e, t));
}
// 移動平均の遅れを傾きで補う式(= captureRunner::sceneNowFromBuf)。項目7と項目8で共有する。
static const double kSceneLeadMaxStops = 1.5;	// = captureRunner::kSceneLeadMaxStops
static double sceneNowFromBufRef(const std::vector<double>& buf)
{
	std::vector<double> l;
	for (double v : buf) { if (v > 0.0) { l.push_back(std::log2(v)); } }
	if (l.empty()) { return -1.0; }
	double mean = 0.0;
	for (double v : l) { mean += v; }
	mean /= static_cast<double>(l.size());
	if (l.size() < 3) { return std::pow(2.0, mean); }
	std::vector<double> d;
	for (size_t i = 1; i < l.size(); ++i) { d.push_back(l[i] - l[i - 1]); }
	std::sort(d.begin(), d.end());
	const size_t m = d.size() / 2;
	const double slope = (d.size() % 2 != 0) ? d[m] : (d[m - 1] + d[m]) / 2.0;
	double lead = slope * (static_cast<double>(l.size()) - 1.0) / 2.0;
	if (lead >  kSceneLeadMaxStops) { lead =  kSceneLeadMaxStops; }
	if (lead < -kSceneLeadMaxStops) { lead = -kSceneLeadMaxStops; }
	return std::pow(2.0, mean + lead);
}
static const double kStep = 1.0 / 3.0;
// 撮影中は必ず1ステップ(=1/3段)に留める(2026-07-24: 境目の多段ジャンプ禁止)。captureRunner と同値。
static const double kMaxCatchUp = 1.0 / 3.0;
static int stepsToClose(double needStops)
{
	const double a = std::fabs(needStops);
	int n = static_cast<int>(a / kStep + 0.5);
	const int maxN = static_cast<int>(kMaxCatchUp / kStep + 0.5);
	if (n < 1)    { n = 1; }
	if (n > maxN) { n = maxN; }
	return n;
}

int main()
{
	// EOS R10 相当のテーブル(f1.4 レンズ)
	expo::expoTables t = expo::standardTables(1.4, 16.0);

	hgc::exposure meterExp; meterExp.iso = "100"; meterExp.ss = "1/100"; meterExp.fn = "1.4";
	hgc::exposure shotExp;  shotExp.iso  = "100"; shotExp.ss  = "4";     shotExp.fn  = "1.4";
	const double metered = 0.1145;	// fr25 の実測

	// --- 1) APEX/Bv/ev0 が実測ログどおりに求まること ---
	// brightnessStops は APEX を 1/3 段へスナップするので、素の log2 とは僅かに異なる。
	const double bMeter = expo::brightnessStops(meterExp, t);
	const double bShot  = expo::brightnessStops(shotExp, t);
	checkNear(bMeter, -7.6667, 0.02, "brightnessStops(ISO100,1/100,f1.4)");
	checkNear(bShot,   1.0000, 0.02, "brightnessStops(ISO100,4s,f1.4)");
	checkNear(bShot - bMeter, 8.6667, 0.02, "測光露出と撮影露出の段差=約8.7段");

	const double bv = expo::ambientBv(metered, expo::avFromFn(1.4), expo::tvFromSs(1.0 / 100.0), expo::svFromIso(100));
	checkNear(bv, 6.96, 0.05, "環境光 Bv");

	expo::ev0Sigmoid cfg;
	cfg.bm = expo::ev0BmFromAltitude(3.5, cfg);	// 日の入25分前 ≒ 太陽高度+3.5°
	const double lin0 = expo::ev0LinearFromBv(bv, cfg);
	checkNear(lin0, 0.18, 0.005, "ev0リニア(日中=18%)");

	// --- 2) 旧ロジックの再現: 測光値をそのまま目標と比べると常に「明るく」 ---
	//     どんなに撮影露出を明るくしても判定が変わらない = 暴走する。
	{
		bool alwaysBrighten = true;
		hgc::exposure e = meterExp;	// 1/100 から出発
		expo::exposureCtl ctl;
		// 計画JSONの定義に合わせる: limitBright=最も露出の多い側(暗所用) / limitDark=最も少ない側(明所用)。
		hgc::exposure limB; limB.iso = "1600"; limB.ss = "8";      limB.fn = "1.4";
		hgc::exposure limD; limD.iso = "100";  limD.ss = "1/4000"; limD.fn = "16";
		hgc::exposureType prio[3] = { hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		ctl.init(t, limB, limD, prio);
		ctl.setCurrent(e);
		for (int i = 0; i < 40; ++i)
		{
			// 旧: avg(測光値) と linD(=lin0) を直接比較
			if (!(metered < lin0)) { alwaysBrighten = false; break; }
			ctl.brighten();
		}
		const double endB = expo::brightnessStops(ctl.current(), t);
		char d[160];
		std::snprintf(d, sizeof(d), "(40コマ後 %s %s -> 明るさ%.2f段)", ctl.current().iso.c_str(), ctl.current().ss.c_str(), endB);
		check(alwaysBrighten && endB > bMeter + 8.0, "旧ロジックは判定が変わらず暴走する(バグ再現)", d);
	}

	// --- 3) 新ロジック: 撮影露出へ投影して比較 → 収束し、暴走しない ---
	{
		expo::exposureCtl ctl;
		// 計画JSONの定義に合わせる: limitBright=最も露出の多い側(暗所用) / limitDark=最も少ない側(明所用)。
		hgc::exposure limB; limB.iso = "1600"; limB.ss = "8";      limB.fn = "1.4";
		hgc::exposure limD; limD.iso = "100";  limD.ss = "1/4000"; limD.fn = "16";
		hgc::exposureType prio[3] = { hgc::exposureType::iso, hgc::exposureType::ss, hgc::exposureType::fn };
		ctl.init(t, limB, limD, prio);
		ctl.setCurrent(shotExp);	// 暴走途中の 4秒 から開始

		const double sceneRef = sceneRefFromMetered(metered, meterExp, t);
		const double evT = 0.0;	// 日中 目標ev=+0.0
		const double center = expo::linearFromEvBase(evT, lin0);

		int frames = 0;
		for (; frames < 30; ++frames)
		{
			const double predicted = linearAtExposure(sceneRef, ctl.current(), t);
			const double need = std::log2(center / predicted);
			if (std::fabs(need) <= kStep) { break; }	// 1/3段以内=収束
			const int steps = stepsToClose(need);
			for (int s = 0; s < steps; ++s)
			{
				if (need < 0.0) { if (!ctl.darken())   { break; } }
				else            { if (!ctl.brighten()) { break; } }
			}
		}
		const hgc::exposure fin = ctl.current();
		const double predFin = linearAtExposure(sceneRef, fin, t);
		char d[200];
		std::snprintf(d, sizeof(d), "(%sコマで収束 iso=%s ss=%s f=%s 予測リニア=%.4f)",
		              std::to_string(frames).c_str(), fin.iso.c_str(), fin.ss.c_str(), fin.fn.c_str(), predFin);
		check(frames < 30, "新ロジックは収束する", d);
		// 目標 0.18 に 1/3段以内で一致すること
		checkNear(std::log2(predFin / center), 0.0, kStep + 1e-6, "収束後の残差が1/3段以内");
		// 実写で適正だった 1/100 付近(=測光露出+0.65段)に落ち着くこと
		checkNear(expo::brightnessStops(fin, t) - bMeter, 0.65, 0.5, "収束先は測光露出+約0.65段(≒1/64秒)");
		// 暴走していない(4秒より確実に短い)
		check(expo::brightnessStops(fin, t) < bShot - 6.0, "4秒から6段以上暗い側へ戻っている");
	}

	// --- 4) 撮影中は必ず1ステップ(=1/3段)に留める(境目のジャンプ禁止。速い収束は撮影前のinitialConverge) ---
	check(stepsToClose(0.05) == 1, "小さな誤差は1ステップ(1/3段)");
	check(stepsToClose(1.0)  == 1, "1段の誤差でも1ステップ(1/3段)に留める");
	check(stepsToClose(9.0)  == 1, "大きな誤差でも1ステップ(1/3段)に留める(多段ジャンプ禁止)");
	// --- 5) 夕日/朝日の振動(2026-07-29,30 実機で再現)と、帯の必要幅 ---
	//
	// 【実測(R100)】露出を1歩(1/3段)変えたときの測光の応答 γ = Δlog2(Y)/ΔE は
	//   中央 1.39(日中) / 1.46(朝日) / 1.27(preNight)。ただし一定ではなく、
	//   朝日の四分位 1.33〜1.82、単発では 0.93〜2.27 まで散る。
	//   サムネイルがカメラ現像のJPEGで、ピクチャースタイルが auto のためコマ毎に
	//   トーンカーブが変わる。ALO は CCAPI に出てこないので切れない。
	//   (サムネの使い回し=古いフレームは0.0〜0.4%。原因ではないことを確認済み)
	//
	// 【なぜ振動するか】刻み q・応答 γ のループは、デッドバンドが γq 以上でないと静止点を
	//   持たない。1歩動かした後の需要が再び帯を超えるため必ず引き返す。
	//   γ が散るので、帯は「その場のγ」ではなく「起こりうるγの上側」で決める必要がある。
	//   γ が一定ならデッドバンド(項目4の wouldOvershoot)だけで止まるが、実機は止まらなかった。
	//   → 止まらない主因は γ の平均値ではなく **ばらつき** である。
	{
		const double kDrift  = 0.03;	// 場面の変化(段/コマ)。夜明けの実測相当
		const int    kFrames = 200;
		const int    kMovAvg = 3;		// 夕日/朝日の設定
		const double kGuard  = 1.0;		// 反転抑制中でも通す急変[段]

		auto wouldOvershoot = [](double need, double band)
		{
			const double a = std::fabs(need);
			if (a >= kStep) { return false; }
			return (kStep - a) > (band / 2.0);
		};

		// 実測の散らばりを決定的に再現する(乱数を使わないので結果は毎回同じ)。
		// 0.93〜2.27 を中央1.46寄りに巡回させる。
		static const double kGammaSeq[] = {
			1.46, 1.82, 1.33, 2.27, 1.10, 1.55, 0.93, 1.68, 1.40, 1.95,
			1.21, 1.60, 1.05, 2.05, 1.35, 1.50, 1.15, 1.75, 1.28, 1.62 };
		const int kNG = static_cast<int>(sizeof(kGammaSeq) / sizeof(kGammaSeq[0]));

		auto run = [&](double band, bool antiChatter, int& reversals, double& worstErr)
		{
			double scene = 0.0, expo = 0.0;
			int lastDir = 0, lock = 0, lastMoved = 0;
			std::vector<double> buf;
			reversals = 0; worstErr = 0.0;
			for (int f = 0; f < kFrames; ++f)
			{
				scene += kDrift;
				const double gamma    = kGammaSeq[f % kNG];
				const double err      = scene + expo;			// 真の露出誤差(0=適正)
				const double sceneRef = gamma * err - expo;		// 現像で γ 倍に見える
				buf.push_back(sceneRef);
				while (static_cast<int>(buf.size()) > kMovAvg) { buf.erase(buf.begin()); }
				double avg = 0.0;
				for (double v : buf) { avg += v; }
				avg /= static_cast<double>(buf.size());
				if (lock > 0) { --lock; }
				if (f > 3 * kMovAvg && std::fabs(err) > worstErr) { worstErr = std::fabs(err); }

				const double need = -(avg + expo);				// +:明るく -:暗く
				if (std::fabs(need) <= band / 2.0) { continue; }
				const int dir = (need < 0.0) ? -1 : 1;
				if (wouldOvershoot(need, band)) { continue; }
				if (antiChatter && !(lastDir == 0 || dir == lastDir || lock <= 0
				                     || std::fabs(need) >= kGuard)) { continue; }
				expo += dir * kStep;
				if (lastMoved != 0 && dir != lastMoved) { ++reversals; }
				lastMoved = dir;
				lastDir = dir; lock = kMovAvg;
			}
		};

		int rNow = 0, rLock = 0, rWide = 0;
		double eNow = 0.0, eLock = 0.0, eWide = 0.0;
		run(0.300, false, rNow,  eNow);		// 実機の状態(帯0.3・デッドバンドのみ)
		run(kStep, true,  rLock, eLock);	// 下限1歩 + 反転抑制
		run(0.800, true,  rWide, eWide);	// 帯を γ上側×1歩 まで広げた場合
		char d[240];
		std::snprintf(d, sizeof(d),
		              "(反転 実機相当=%d回 / 1歩+抑制=%d回 / 0.8段+抑制=%d回  最大誤差 %.2f / %.2f / %.2f段)",
		              rNow, rLock, rWide, eNow, eLock, eWide);
		check(rNow >= 5,             "γがばらつくと帯0.3段では振動する(実機の再現)", d);
		// 帯=1歩 はしきい値 max(帯/2, 1歩-帯/2) が最小になる点なので、かえって悪化する。
		// ここを1歩にしてはいけないことを固定する(2026-07-30 に一度この値を入れて否決した)。
		check(rLock >= rNow,         "帯=1歩(0.333段)は現状より悪化する→採用不可", d);
		check(rWide == 0,            "帯をγの上側(0.8段)まで広げると止まる", d);
		check(eWide <= eNow + 0.35,  "帯を広げても追従の遅れは小さい", d);
	}

	// --- 6) ヒステリシス帯の下限(captureRunner::effHysteresis と同じ値) ---
	{
		const double kFloor = 0.8;		// = captureRunner::kMinHysteresisStops
		auto effHysteresis = [&](double raw) { return (raw > kFloor) ? raw : kFloor; };
		checkNear(effHysteresis(0.3), kFloor, 1e-9, "夕日/朝日の0.3段は下限0.8段へ引き上げられる");
		checkNear(effHysteresis(kStep), kFloor, 1e-9, "1歩(0.333段)も下限0.8段へ引き上げられる");
		checkNear(effHysteresis(1.0), 1.0,    1e-9, "日中の1.0段は下限より広いのでそのまま");
		// 下限は γ_max×1歩 以上であること(静止点の存在条件)
		check(kFloor >= 2.27 * kStep - 1e-9, "下限は実測γの上側(2.27)×1歩=0.76段以上");
	}

	// --- 7) 移動平均の遅れを傾きで補う(2026-08-02 案C')。captureRunner::sceneNowFromBuf と同じ式 ---
	//
	// 【背景】2026-08-01 の postNight で、写真が目標より最大 1.45段 明るくなった(IMG_4627)。
	//  内訳は ヒステリシス帯 +0.50段 と 移動平均の遅れ +0.92段 で、主犯は後者。
	//  n点平均は (n-1)/2 コマ遅れた値になり、遅れ[段] = 変化速度[段/コマ] × (n-1)/2。
	//  空が 0.09段/コマ の間は 0.18段 だが、夜明けが 0.46段/コマ に加速すると 0.92段 に膨らむ。
	//  「一部の時間帯だけ明るくずれる」のはこれが理由で、一定量のヒステリシスでは説明できない。
	//
	// 【このテストが固定する仕様】
	//  ・一定速度の変化では遅れが 0 になること
	//  ・1コマだけの外れ値(車のライト等)に対し、単純平均と同程度にしか反応しないこと
	//    (最小二乗の傾きだと3倍に過剰反応し、消えた後に逆振れする。だから差分の中央値を使う)
	//  ・外挿量は上限で頭打ちになること
	{
		const double kLeadMax = kSceneLeadMaxStops;

		// 実装と同じ: 段(log2)で平均し、差分の中央値を傾きとして (n-1)/2 コマ分だけ外挿する
		auto sceneNow = [](const std::vector<double>& buf) { return sceneNowFromBufRef(buf); };
		auto plainAvg = [](const std::vector<double>& buf)
		{
			double a = 0.0;
			for (double v : buf) { a += v; }
			return a / static_cast<double>(buf.size());
		};
		auto stops = [](double a, double b) { return std::log2(a / b); };

		// ① 一定速度で明るくなる(夜明け 0.30段/コマ。上限0.5段に当たらない範囲で見る)
		{
			const double rate = 0.46;	// 実測の夜明けの最速(2026-08-01)
			std::vector<double> buf;
			for (int i = 0; i < 5; ++i) { buf.push_back(std::pow(2.0, rate * i)); }
			const double truth = std::pow(2.0, rate * 4);	// 最新コマの真値
			checkNear(stops(sceneNow(buf), truth), 0.0, 0.02, "一定速度の変化で遅れが消える");
			// 単純平均は (n-1)/2 コマ分だけ遅れる
			checkNear(stops(truth, plainAvg(buf)), rate * 2.0, 0.20, "単純平均は2コマ分遅れる(比較)");
		}

		// ② 1コマだけ2段明るい(車のライト)。単純平均と同程度までしか反応しないこと
		{
			std::vector<double> buf = { 1.0, 1.0, 4.0, 1.0, 1.0 };	// 中央のコマだけ +2段
			const double got   = stops(sceneNow(buf),  1.0);
			const double plain = stops(plainAvg(buf), 1.0);
			char d[160];
			std::snprintf(d, sizeof(d), "(推定=%+.2f段 単純平均=%+.2f段)", got, plain);
			check(got <= plain + 0.05, "一過性の光に過剰反応しない(単純平均以下)", d);
			check(got > 0.0, "一過性の光を完全に無視はしない", d);
		}

		// ③ 光が消えた後に逆振れしない(外れ値がバッファから抜ける途中)
		{
			std::vector<double> buf = { 4.0, 1.0, 1.0, 1.0, 1.0 };	// 古い側に外れ値
			const double got = stops(sceneNow(buf), 1.0);
			char d[120];
			std::snprintf(d, sizeof(d), "(推定=%+.2f段)", got);
			check(got > -0.10, "外れ値が抜けるときに暗い側へ逆振れしない", d);
		}

		// ④ 外挿量の頭打ち(急変時に行き過ぎない)
		{
			const double rate = 2.0;	// 2段/コマ の極端な変化
			std::vector<double> buf;
			for (int i = 0; i < 5; ++i) { buf.push_back(std::pow(2.0, rate * i)); }
			double mean = 0.0;
			for (double v : buf) { mean += std::log2(v); }
			mean /= 5.0;
			checkNear(stops(sceneNow(buf), std::pow(2.0, mean)), kLeadMax, 1e-6,
			          "外挿量は上限(1.5段)で頭打ちになる");
		}

		// ⑤ 有効な値が無ければ -1(測光失敗が続いた場合に壊れない)
		{
			std::vector<double> buf = { -1.0, 0.0, -1.0 };
			check(sceneNow(buf) < 0.0, "有効な測光値が無ければ無効を返す");
		}
	}


	// --- 8) ライブビューの張り付き検出(2026-08-03 実機の暴走から) ---
	//
	// 【何が起きたか】postNight 04:11 の R10。暗所でカメラがライブビューに自動ゲインをかけ、
	//  露出を絞っても測光値が下がらなくなった(張り付き)。制御は「まだ明るい」と読み続け、
	//  ISO1250→125 まで11コマ連続で絞り続けた。写真は13コマで 4.2段 暗くなり
	//  (IMG_0566 +1.57段 → IMG_0579 -2.61段)、測光値は実際の写真より最大 2.88段 暗かった。
	//
	// 【なぜ検出できなかったか】張り付き判定は adaptMeterSs にしか無く、そこは
	//  「測光ssへ切替えたコマ」でしか呼ばれない。7/31 に入れた「撮影ssのまま測る」経路は
	//  その手前で return していたので、判定ごと無効になっていた。
	//  さらに判定条件が「露出が0.5段以上動いたとき」だったため、撮影中の 1歩=1/3段(0.333)
	//  では **切替経路でも一度も働かない**。値の範囲(lvUsableAsIs)は正常なままなので気づけない。
	//
	// 【このテストが固定する仕様】応答比 = Δ測光値[段] / Δ露出[段]。正常なら 1.0。
	//  ・1歩(1/3段)の変化でも判定が働くこと(最小変化量 ≦ 1/3段)
	//  ・実測の張り付き列(応答比 -0.2 前後)を張り付きと判定すること
	//  ・実測の正常列(応答比 +1.5)を張り付きと判定しないこと
	//  ・場面自体の変化が乗った通常コマを誤検出しないこと
	//  ・1コマのノイズでは確定せず、連続 kMeterPinConfirm 回で確定すること
	{
		const double kRatio    = 0.50;	// = apiCanonCCAPI kMeterRespondRatio
		const double kMinStops = 0.30;	// = apiCanonCCAPI kMeterRespondMinStops
		const int    kConfirm  = 2;		// = apiCanonCCAPI kMeterPinConfirm

		check(kMinStops <= 1.0 / 3.0 + 1e-9, "1歩(1/3段)でも応答比の判定が働く最小変化量である");

		// 実装と同じ判定器。dSs=露出の変化[段]、dLin=測光値の変化[段]
		struct detector
		{
			double ratio; double minStops; int confirm; int streak = 0;
			bool feed(double dSs, double dLin)
			{
				if (std::fabs(dSs) < minStops) { return false; }	// 判定できるだけ動いていない
				if ((dLin / dSs) < ratio) { ++streak; } else { streak = 0; }
				return streak >= confirm;
			}
		};

		// ① 実測の暴走列(04:11:00〜04:13:15)。露出を1歩(-0.333段)ずつ絞ったときの測光値の変化[段]。
		//    ISO1250→320 の間、測光値は下がるどころか上がっていた。
		{
			const double dLin[] = { -0.22 * -1.0 / 3.0, -0.25 * -1.0 / 3.0, -0.24 * -1.0 / 3.0,
			                        -0.17 * -1.0 / 3.0, -0.22 * -1.0 / 3.0, -0.22 * -1.0 / 3.0 };
			detector d{ kRatio, kMinStops, kConfirm };
			int at = -1;
			for (int i = 0; i < 6; ++i)
			{
				if (d.feed(-1.0 / 3.0, dLin[i]) && at < 0) { at = i; }
			}
			char note[96];
			std::snprintf(note, sizeof(note), "(%dコマ目で検出)", at + 1);
			check(at >= 0, "実測の張り付き列(応答比-0.2前後)を張り付きと判定する", note);
			check(at == kConfirm - 1, "確定は連続2コマ目。それ以上は引っ張らない", note);
		}

		// ② 実測の回復列(ISO250→160)。応答比 +1.5 は正常。張り付きと判定してはいけない。
		{
			detector d{ kRatio, kMinStops, kConfirm };
			const double r[] = { 1.51, 1.55 };
			bool pinned = false;
			for (int i = 0; i < 2; ++i) { if (d.feed(-1.0 / 3.0, r[i] * -1.0 / 3.0)) { pinned = true; } }
			check(!pinned, "応答比+1.5の正常な測光を張り付きと誤判定しない");
		}

		// ③ 通常コマ: 1歩絞る間に場面が 0.08段 明るくなった。応答比 0.76 で正常側。
		{
			detector d{ kRatio, kMinStops, kConfirm };
			bool pinned = false;
			for (int i = 0; i < 5; ++i) { if (d.feed(-1.0 / 3.0, -1.0 / 3.0 + 0.08)) { pinned = true; } }
			check(!pinned, "場面変化が乗った通常コマを誤検出しない(応答比0.76)");
		}

		// ④ 1コマだけの外れ値では確定しない(測光ノイズで切替経路へ落とさない)
		{
			detector d{ kRatio, kMinStops, kConfirm };
			check(!d.feed(-1.0 / 3.0, +0.10), "1コマ目の異常では確定しない");
			check(!d.feed(-1.0 / 3.0, -1.0 / 3.0 + 0.08), "正常コマが挟まれば連続回数はリセットされる");
		}

		// ⑤ 露出を動かしていないコマは判定材料にならない(ゼロ割・誤検出を出さない)
		{
			detector d{ kRatio, kMinStops, kConfirm };
			bool pinned = false;
			for (int i = 0; i < 5; ++i) { if (d.feed(0.0, -0.5)) { pinned = true; } }
			check(!pinned, "露出が動いていないコマでは張り付きと判定しない");
		}

		// ⑥ 張り付き中は外挿しないこと。captureRunner は検出コマでバッファを捨てるので、
		//    偽のトレンド(絞っているのに明るくなり続ける)を増幅しない。
		{
			// 暴走中の測光値: 絞っているのに毎コマ +0.07段 ずつ上がっていた
			std::vector<double> bad;
			for (int i = 0; i < 5; ++i) { bad.push_back(std::pow(2.0, 0.07 * i)); }
			double mean = 0.0;
			for (double v : bad) { mean += std::log2(v); }
			mean /= 5.0;
			// 捨てずに外挿すると平均より 0.14段 明るい側へ行き過ぎる(=さらに絞る方向)
			const double leaked = std::log2(sceneNowFromBufRef(bad)) - mean;
			check(leaked > 0.10, "張り付き列をそのまま渡すと外挿が上振れする(捨てる根拠)");
			// 検出コマで捨てた後は1点だけ。傾きは使われない。
			std::vector<double> one = { bad.back() };
			checkNear(std::log2(sceneNowFromBufRef(one)), std::log2(bad.back()), 1e-9,
			          "捨てた直後は1点のみ=外挿されない");
		}
	}


	// --- 9) 測光ss切替の入口(2026-08-04)。「測れなかった」と「撮影ssでは測光に向かない」を分ける ---
	//
	// 【背景】切替なし経路は、LV取得の失敗も「値が範囲外」も同じ扱いで測光ss切替へ落としていた。
	//  切替は「撮影ssではLVが忠実に積分できない場面」への対処であって、通信やLVの一過性の失敗に
	//  効く手ではない。混ぜたために 2026-08-03 17:28 の夕R10 で次が起きた:
	//   ・通信のスタールで切替経路に落ちる
	//   ・測光ssが未学習なので初期値=撮影ss-5段。1/500 → 1/16000 という日中では真っ黒な値
	//   ・その測光値 0.0017 は下限 0.001548 のわずか 0.13段上。adaptMeterSs は
	//     「信号は足りている→現状維持」を選び、1/16000 に居座る
	//   ・抜けたのは lvAsIsWait_ が尽きた19コマ後。その間 写真は最大1.3段 明るくなった
	//
	// 【このテストが固定する仕様】
	//  ・LVが取れなかった(戻り値NG、またはヒストが空で linear<=0)→ 切替へ落とさない。据え置き
	//  ・取れて値が範囲外 → 従来どおり切替へ
	//  ・本当に真っ暗で切替に入った場合は、adaptMeterSs の伸長で数コマで撮影ssへ戻れること
	{
		const double kLoX = 0.020, kHiX = 0.850;	// = kMeterUsableLoX / kMeterUsableHiX
		const double loLin = expo::srgbToLinear(kLoX);
		const double hiLin = expo::srgbToLinear(kHiX);

		// 実装と同じ判定: 取得できたか / 使える範囲か
		auto acquired   = [](bool rcOk, double linear) { return rcOk && (linear > 0.0); };
		auto usableAsIs = [&](double linear)
		{
			if (!(linear > 0.0)) { return false; }
			return (linear >= loLin) && (linear <= hiLin);
		};
		// 入口の行き先。0=そのまま採用 / 1=測光ss切替へ / 2=据え置き(切替へ落とさない)
		auto route = [&](bool rcOk, double linear)
		{
			if (!acquired(rcOk, linear)) { return 2; }
			return usableAsIs(linear) ? 0 : 1;
		};

		check(route(false, -1.0) == 2, "LV取得が失敗(戻り値NG) → 据え置き。切替へ落とさない");
		check(route(true,   0.0) == 2, "ヒストが空(linear=0でも戻り値はOK) → 据え置き。切替へ落とさない");
		check(route(true, loLin * 0.25) == 1, "取れて暗すぎ(真っ暗) → 従来どおり切替へ");
		check(route(true, hiLin * 4.00) == 1, "取れて明るすぎ(飽和) → 従来どおり切替へ");
		check(route(true, 0.05)         == 0, "取れて範囲内 → 撮影ssのまま採用");

		// 17:28 の実測値が「切替へ落とす側」ではなく「据え置き側」に分類されること。
		// (as-is が返したのは失敗か空データ。0.0017 という値は切替後の測光で得たもの)
		check(route(true, 0.0) == 2, "17:28の入口(空データ)は据え置きへ分類される");

		// 本当に真っ暗で切替に入った場合の復帰: adaptMeterSs は下限に届くまで1コマ1段伸ばす。
		// 撮影ssに追いつけば decideMeterSs が空を返し、切替なしへ戻る。
		{
			const double kMaxLenStep = 1.0;			// = kMeterMaxLenStep
			const double kInitDrop   = 5.0;			// = kMeterInitDropStops
			double atMeterSs = -kInitDrop;			// 撮影ss基準の測光ss[段]。初期値は5段短い
			double x = loLin * std::pow(2.0, atMeterSs + kInitDrop) / 64.0;	// 真っ暗(下限を大きく割る)
			int n = 0;
			while (atMeterSs < 0.0 && n < 20)
			{
				if (x >= loLin) { break; }			// 信号が足りたら伸ばすのをやめる
				double d = std::log2(loLin / x);
				if (d > kMaxLenStep) { d = kMaxLenStep; }
				atMeterSs += d; x *= std::pow(2.0, d);
				++n;
			}
			char note[96];
			std::snprintf(note, sizeof(note), "(%dコマで測光ss %+.1f段)", n, atMeterSs);
			check(n <= 6, "真っ暗で切替に入っても数コマで撮影ssへ戻れる(固まらない)", note);
		}

		// 一方、17:28 の値は下限のすぐ上にあり「現状維持」に落ちる=自力では抜けられない。
		// これが「失敗を切替に混ぜてはいけない」根拠。
		{
			const double stuck = 0.0017;			// 実測(1/16000での測光値)
			char note[120];
			std::snprintf(note, sizeof(note), "(下限 %.6f の %+.2f段上)", loLin, std::log2(stuck / loLin));
			check(stuck > loLin && stuck < hiLin,
			      "17:28の測光値は『信号は足りている』に分類され、伸長が働かない", note);
		}
	}

	// --- 10) 測光ssの下り階段(2026-08-06 夕R10 の実機暴走から) ---
	//
	// 【何が起きたか】19:02、日没後の preNight で測光ss切替に入った直後から、測光ssが
	//  0.5s → 1/4 → 1/8 → 1/15 → 1/30 → … → 1/16000 と 1段/コマで単調に短くなり続けた。
	//  撮影露出はそれに追随して ISO320/8s → ISO100/(1/20s) まで 8.3段 落ち、
	//  19:12〜20:24 の 305コマ(全960コマ中)が Y=0.0002 の真っ黒になった。
	//  20:24 の星景(固定露出)への切替でようやく復帰した。
	//
	// 【原因】adaptMeterSs の張り付き判定が向きを見ていなかった(std::fabs(dSs))。
	//  この判定の意味は「伸ばしたのに上がらない=LVの積分上限に当たった」で、対処は
	//  「天井を1段下げる=短くする」。ところが fabs のため「短くしたのに指示ほど下がらない」
	//  コマにも成立する。暗所では測光値がヒストグラムの最下位ビンに張り付いて応答が鈍るので
	//  必ず成立し、対処の短縮がまた成立を生む自己増幅になる。
	//  実測の連鎖(fr=500〜513): 0.4→0.5s で伸ばして無反応 → 正当な張り付き → 1段短縮。
	//  以降は「短縮 → 応答比 0.3〜0.4 → 誤って張り付き → さらに短縮」を表の下端まで繰り返した。
	//
	// 【このテストが固定する仕様】
	//  ・伸ばして反応が無いコマは従来どおり張り付きと判定する(正当な検出は失わない)
	//  ・縮めたコマでは張り付きと判定しない(向きを見る)
	//  ・実測の下り階段の入力を流しても、測光ssが単調減少し続けないこと
	{
		const double kRatio    = 0.50;	// = kMeterRespondRatio
		const double kMinStops = 0.30;	// = kMeterRespondMinStops

		// 修正後の adaptMeterSs と同じ判定(伸ばした側だけを見る)。
		auto pinnedNew = [&](double dSs, double dLin)
		{
			if (!(dSs >= kMinStops)) { return false; }
			return (dLin / dSs) < kRatio;
		};
		// 修正前(向きを見ない)。バグの再現用。
		auto pinnedOld = [&](double dSs, double dLin)
		{
			if (std::fabs(dSs) < kMinStops) { return false; }
			return (dLin / dSs) < kRatio;
		};

		// ① 正当な検出: 伸ばした(+0.33段)のに測光値が動かない → 張り付き。
		check(pinnedNew(+1.0 / 3.0, 0.0), "伸ばして無反応なら張り付きと判定する(積分上限)");
		check(pinnedNew(+1.0, 0.0),       "1段伸ばして無反応でも張り付きと判定する");
		// 伸ばして素直に上がったコマは張り付きではない。
		check(!pinnedNew(+1.0, 1.0),      "伸ばして指示どおり上がったコマは張り付きではない");

		// ② 誤検出の除去: 縮めた側は判定しない。実測 fr=501〜505 の応答比。
		//    (1段短縮に対し測光値は 0.32/0.0/0.415 段しか下がらない=黒の底に当たっている)
		{
			const double dLin[] = { -0.32, -0.415, 0.0, 0.0, -0.30 };
			int oldPin = 0, newPin = 0;
			for (int i = 0; i < 5; ++i)
			{
				if (pinnedOld(-1.0, dLin[i])) { ++oldPin; }
				if (pinnedNew(-1.0, dLin[i])) { ++newPin; }
			}
			char note[96];
			std::snprintf(note, sizeof(note), "(修正前=%d回 修正後=%d回)", oldPin, newPin);
			check(oldPin == 5, "修正前は縮めた5コマ全部を張り付きと誤判定する(バグの再現)", note);
			check(newPin == 0, "修正後は縮めたコマを張り付きと判定しない", note);
		}

		// ③ 下り階段が起きないこと。実測 fr=500 起点で、暗所(測光値は底に張り付いて動かない)を
		//    模して回す。1コマの流れ = 張り付き判定 → 天井更新 → 暗すぎなら伸長 → 天井でクランプ。
		{
			const double kPinBackoff = 1.0;		// = kMeterPinBackoffStops
			const double kCeilRelax  = 0.10;	// = kMeterCeilRelaxStops
			const double kMaxLenStep = 1.0;		// = kMeterMaxLenStep
			const double kSsFloor    = -14.0;	// 測光ss表の下端(撮影ss基準の相対段。1/16000 相当)

			auto run = [&](bool useOld)
			{
				double cur = 0.0;			// 測光ssの位置[段](相対)
				double ceil_ = 1e9;
				double prev = cur;
				double minSeen = 0.0;
				for (int i = 0; i < 40; ++i)
				{
					const double dSs = cur - prev;
					// 暗所の応答: 底に張り付いているので、指示の 30〜40% しか動かない。
					const double dLin = dSs * 0.35;
					const bool pinned = useOld ? pinnedOld(dSs, dLin) : pinnedNew(dSs, dLin);
					prev = cur;
					double want;
					if (pinned) { ceil_ = cur - kPinBackoff; want = ceil_; }
					else
					{
						want = cur + kMaxLenStep;	// 測光値が下限を割っている=伸ばす側
						ceil_ += kCeilRelax;
						if (want > ceil_) { want = ceil_; }
					}
					cur = want;
					if (cur < kSsFloor) { cur = kSsFloor; }	// 表の下端で止まる
					if (cur < minSeen) { minSeen = cur; }
				}
				return minSeen;
			};
			const double oldMin = run(true);
			const double newMin = run(false);
			char note[128];
			std::snprintf(note, sizeof(note), "(修正前 %.1f段 / 修正後 %.1f段)", oldMin, newMin);
			check(oldMin <= kSsFloor + 1e-9, "修正前は測光ssが表の下端まで走り切る(バグの再現)", note);
			check(newMin > -2.5, "修正後は測光ssが下り階段にならず1段幅に収まる", note);
		}
	}

	// --- 11) 測光ss切替経路の採否(2026-08-07) ---
	//
	// 【背景】切替なし経路(①)には lvUsableAsIs による採否判定があるが、切替経路(②)には
	//  無く、帯の外の値でも ok=true / sceneRef をそのまま返していた。①と②で非対称だった。
	//  実測(2026-08-06 夕R10 19:12〜20:24): 測光ssが 1/16000 まで走った状態で Y=0.0002
	//  (下限 0.001548 を約3段下回る)を返し続け、「1/16000 でこれだけ写る=非常に明るい場面」
	//  と解釈されて撮影露出が 8s → 1/20秒 まで絞られた。305コマが真っ黒になった直接の経路。
	//
	// 【このテストが固定する仕様】
	//  ・切替経路でも、帯の外の値は露出制御に使わない(usable=false → 露出据え置き)
	//  ・ただし値そのものはログに残す(ok は落とさない=Y=/LVHIST が消えない)
	//  ・帯の内側なら従来どおり採用する
	//  ・据え置きにしていれば、黒つぶれの値では撮影露出が1歩も動かないこと
	{
		const double loLin = expo::srgbToLinear(0.020);	// = kMeterUsableLoX
		const double hiLin = expo::srgbToLinear(0.850);	// = kMeterUsableHiX
		auto usable = [&](double linear) { return (linear > 0.0) && (linear >= loLin) && (linear <= hiLin); };

		// 実測値の分類
		check(!usable(0.0002), "1/16000で測った Y=0.0002 は帯の外 → 露出制御に使わない");
		check(!usable(0.0004), "暴走中の Y=0.0004 も帯の外");
		check( usable(0.0160), "暴走直前の Y=0.0160 は帯の内側 → 従来どおり採用");
		check( usable(0.1469), "日中の Y=0.1469 は帯の内側");
		check(!usable(0.9500), "飽和側 Y=0.95 も帯の外 → 使わない");

		// ログは残す(ok と usable を分けた理由)。ok は「測れたか」だけを表す。
		{
			const bool ok = true, use = usable(0.0002);
			check(ok && !use, "測れているので ok=true(ログにY=が出る)が、usable=false で据え置き");
		}

		// 露出が動かないこと。据え置き = 目標露出を1歩も変えない。
		{
			// 暴走時と同じ入力: 測光ss 1/16000(撮影ss 8s より 14.97段短い)で Y=0.0002。
			// 採否を入れないと sceneRef が跳ね上がり「明るすぎ」と読んで絞り続ける。
			const double meterStops = -14.97;			// 測光露出[段](撮影露出基準)
			const double sceneRef   = 0.0002 / std::pow(2.0, meterStops);
			const double predicted  = sceneRef * std::pow(2.0, 0.0);	// 撮影露出で写る明るさ
			const double target     = 0.18;				// 中庸グレー
			const double need       = std::log2(target / predicted);	// 負=暗くしろ
			char note[128];
			std::snprintf(note, sizeof(note), "(予測=%.1f 目標=%.2f → %+.1f段の指示)", predicted, target, need);
			check(need < -5.0, "採否が無いと『5段以上暗くしろ』という指示になる(暴走の再現)", note);
			check(!usable(0.0002), "採否を入れればこの値は制御に届かない=露出は据え置き", note);
		}
	}

	// --- 12) 測光ss天井の下限と復帰速度(2026-08-07) ---
	//
	// 【背景】meterCeilStops_ には下限が無く、張り付きの誤検出が連鎖すると ss表の下端まで
	//  走れた。復帰は緩和 0.10段/コマ だけで、15秒周期では1段に25秒、下端からは約35分かかる。
	//  さらに 0.10 は張り付き判定の最小変化量 0.30段 に届かないため、天井を緩めているあいだ
	//  「伸ばしたのに上がらない」を一度も検出できず、天井が青天井に伸びる副作用もあった。
	//
	// 【このテストが固定する仕様】
	//  ・天井は 撮影ss-kMeterInitDropStops(=測光ssの初期値)より下げない
	//  ・緩和は1歩(1/3段)。張り付き判定の最小変化量以上であること
	//  ・下端相当から数コマで初期値へ戻れること
	{
		const double kInitDrop   = 5.0;			// = kMeterInitDropStops
		const double kCeilRelax  = 1.0 / 3.0;	// = kMeterCeilRelaxStops(修正後)
		const double kMinStops   = 0.30;		// = kMeterRespondMinStops

		check(kCeilRelax >= kMinStops,
		      "天井の緩和は張り付き判定の最小変化量以上(緩和中も判定が働く)");
		check(0.10 < kMinStops,
		      "修正前の 0.10 は最小変化量に届かず、緩和中は判定が働かなかった(バグの再現)");

		// 天井の下限。撮影ss基準の相対段で -kInitDrop。
		const double ceilFloor = -kInitDrop;
		{
			double ceil_ = -3.0;					// 張り付きで下げようとする
			ceil_ = ceil_ - 1.0;					// = -4.0 (kMeterPinBackoffStops)
			if (ceil_ < ceilFloor) { ceil_ = ceilFloor; }
			check(ceil_ >= ceilFloor - 1e-9, "1回の後退では下限を割らない");

			ceil_ = -14.0;							// 下端まで走った状態を模す
			if (ceil_ < ceilFloor) { ceil_ = ceilFloor; }
			check(std::fabs(ceil_ - ceilFloor) < 1e-9, "下限より下は切り上げられる(-14段→-5段)");
		}

		// 復帰速度。下限から初期値(=下限そのもの)ではなく、後退した位置から戻る所要コマ数。
		{
			double ceil_ = ceilFloor;			// 最も下がった状態
			const double want = 0.0;			// 撮影ss相当まで戻したい
			int n = 0;
			while (ceil_ < want && n < 100) { ceil_ += kCeilRelax; ++n; }
			char note[96];
			// 5段 ÷ 1歩 = 15コマ(端数で16)。15秒周期なら約4分。
			std::snprintf(note, sizeof(note), "(%dコマ=約%d分@15秒周期)", n, n * 15 / 60);
			check(n <= 16, "下限から撮影ss相当まで16コマ以内で戻れる", note);

			// 旧値との比較(同じ距離を何コマかかっていたか)。
			double old_ = ceilFloor; int on = 0;
			while (old_ < want && on < 1000) { old_ += 0.10; ++on; }
			std::snprintf(note, sizeof(note), "(修正前=%dコマ 修正後=%dコマ)", on, n);
			check(on > n * 2, "修正前は同じ復帰に倍以上かかっていた", note);
		}
	}

	// --- 13) 測光ss切替の反映待ち(2026-08-07) ---
	//
	// 【背景】waitLvReflect の抜け条件は「指示した段数の kMeterReflectRatio(=0.5) 以上、
	//  測光値が動いたか」だけだった。深く切替えるほど要求される変化量が大きくなる一方、
	//  LVの中央値はヒストグラム最下位ビン(Y≒0.0002)より下がれない。要求が限界を超えると
	//  この条件は構造的に成立しなくなり、必ず上限 2600ms まで待つ。
	//  実測(2026-08-06 夕R10)の境目:
	//    要求下落 2.00/3.00/3.45段 → 623/941/928ms で抜けた
	//    要求下落 3.95段以上       → 2641〜2876ms(全コマ上限)
	//
	// 【このテストが固定する仕様】
	//  ・比率判定は従来どおり(浅い切替では今までどおり抜ける)
	//  ・底に達して動かなくなったら「これ以上変わらない」として抜ける(第2の条件)
	//  ・切替直後の未反映で早すぎる離脱をしない(最低待ち時間)
	//  ・予算の超過が1ポーリング分ぶん出ない
	{
		const double kRatio       = 0.50;	// = kMeterReflectRatio
		const int    kPollMs      = 200;	// = kMeterReflectPollMs
		const int    kMinWaitMs   = 600;	// = kMeterReflectMinWaitMs
		const double kStableStops = 0.10;	// = kMeterReflectStableStops
		const int    kBudgetMs    = 2600;	// = kMeterSettleMaxMs

		// 抜け判定(実装と同じ)。lin/prevLin/elapsed から「抜けるか」を返す。
		auto exitNow = [&](double before, double delta, double lin, double prevLin, int elapsed)
		{
			if ((std::log2(lin / before) / delta) >= kRatio) { return true; }
			if (prevLin > 0.0 && elapsed >= kMinWaitMs
			 && std::fabs(std::log2(lin / prevLin)) <= kStableStops) { return true; }
			return false;
		};

		// ① 浅い切替(要求下落 2.00段)。実測は 623ms で抜けている。比率判定で抜けること。
		{
			const double before = 0.0150, delta = -4.00;	// 要求下落 -2.00段
			const double lin    = before * std::pow(2.0, -2.10);	// 素直に下がった
			check(exitNow(before, delta, lin, -1.0, 400), "浅い切替は従来どおり比率判定で抜ける");
		}

		// ② 深い切替。LVの中央値は最下位ビンより下がれないので、到達できる下落幅には上限がある。
		//    到達可能 = log2(底 / 切替前)。比率判定が要求するのは |delta|/2 なので、
		//    |delta|/2 が到達可能を超えた時点で**構造的に成立しなくなる**。
		{
			const double before = 0.0100;					// 切替前(撮影ss=8sで測った値)
			const double floorY = 0.0004;					// 最下位ビン(これ以上下がらない)
			const double reach  = std::log2(floorY / before);	// 到達できる下落[段] = -4.64
			const double bound  = std::fabs(reach) * 2.0;		// この段数を超える切替は抜けられない
			char note[128];
			std::snprintf(note, sizeof(note), "(到達可能 %.2f段 → 切替 %.1f段 超で成立しなくなる)", reach, bound);
			check(bound > 9.0 && bound < 10.0, "到達可能な下落幅から抜けられる切替の上限が決まる", note);

			// 実測の 1/16000(撮影ss 2s に対し -14.97段)。要求 -7.48段 > 到達可能 -4.64段。
			const double delta = -14.97;
			check(!exitNow(before, delta, floorY, -1.0, 400),
			      "底に達すると比率判定だけでは抜けられない(バグの再現)", note);
			check(exitNow(before, delta, floorY, floorY * 1.02, 800),
			      "連続2回が0.1段以内なら底に達したとみなして抜ける");
			// 浅い側は従来どおり抜けられること(第2条件が浅い切替を壊していない)。
			check(exitNow(before, -7.0, floorY, -1.0, 400),
			      "到達可能の範囲内(-7.0段)なら従来どおり比率判定で抜ける", note);
		}

		// ③ 早すぎる離脱をしない。切替直後、まだ動いていない2回で抜けてはいけない。
		{
			const double before = 0.0100, delta = -7.91;
			check(!exitNow(before, delta, before, before, 400),
			      "最低待ち時間(600ms)の前は安定判定で抜けない");
			// 正常時の実測は 623ms 以上なので、この下限で損はしない。
			check(kMinWaitMs <= 623, "最低待ち時間は正常時の実測(623ms)を超えない");
		}

		// ④ 予算の超過。従来は先頭で経過を見てから寝て取りに行っていたので1周期ぶん超えた。
		{
			// 修正前: elapsed < budget なら入る → 最大 budget-1 + poll + 取得 まで伸びる
			const int oldWorst = (kBudgetMs - 1) + kPollMs + 300;	// 取得を300msとして
			// 修正後: elapsed + poll >= budget なら入らない
			const int newWorst = (kBudgetMs - kPollMs - 1) + kPollMs + 300;
			char note[128];
			std::snprintf(note, sizeof(note), "(修正前 最悪%dms / 修正後 最悪%dms)", oldWorst, newWorst);
			check(newWorst < oldWorst, "予算チェックを前倒しして超過を1ポーリング分減らす", note);
			check(oldWorst >= 2951, "修正前の最悪値は実測の 2951ms を説明できる", note);
		}
	}

	std::printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
	return g_fail == 0 ? 0 : 1;
}
