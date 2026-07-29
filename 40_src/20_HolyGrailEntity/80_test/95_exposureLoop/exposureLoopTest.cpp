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


	std::printf("\n%s (fail=%d)\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
	return g_fail == 0 ? 0 : 1;
}
