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


	std::printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
	return g_fail == 0 ? 0 : 1;
}
