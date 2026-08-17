// 撮影計画が撮影制御方法を所有する構造の回帰テスト(2026-08-11)。カメラ不要。
//
// 【何を守るためのテストか】
//  以前は撮影制御方法をグローバルに1組だけ持ち、buildSchedule がスケジュールを引き直すたびに
//  そこから窓の中身を作り直していた。そのため
//   ・計画ごとに違う設定を持てない
//   ・エッジで受信した計画に「直前に受け取った別の計画」の設定が貼り付いて保存される
//  という不具合が起きた(2026-08-08 実害: 朝R10 の「日中」にスマホに存在しない値が入った)。
//  計画が実体を所有し、窓はそれを指すだけ、という構造に変えた。その性質を固定する。
#include "cs.h"
#include "ccm.h"
#include "csJson.h"
#include "astroSched.h"
#include <cstdio>
#include <cstdlib>	// std::llabs
#include <string>

namespace
{
	int g_pass = 0;
	int g_fail = 0;

	void check(bool ok, const char* what)
	{
		if (ok) { ++g_pass; }
		else    { ++g_fail; std::printf("  [NG] %s\n", what); }
	}

	void checkStr(const std::string& got, const std::string& want, const char* what)
	{
		if (got == want) { ++g_pass; }
		else { ++g_fail; std::printf("  [NG] %s  got=\"%s\" want=\"%s\"\n", what, got.c_str(), want.c_str()); }
	}

	hgc::dateTime dt(int y, int mo, int d, int h, int mi)
	{
		hgc::dateTime t{};
		t.year = static_cast<uint16_t>(y); t.month = static_cast<uint16_t>(mo); t.day = static_cast<uint16_t>(d);
		t.hour = static_cast<uint16_t>(h); t.min = static_cast<uint16_t>(mi); t.sec = 0;
		return t;
	}

	// テスト用の計画。東京・夕方から翌朝まで(夜間/移行/日中/朝日 が一通り出る長さ)。
	hgc::cs makePlan(const char* name)
	{
		hgc::cs p;
		p.name = name;
		p.place.name = "Tokyo"; p.place.latitude = 35.681; p.place.longitude = 139.767; p.place.altitude = 40.0;
		p.start = dt(2026, 8, 11, 16, 0);
		p.end   = dt(2026, 8, 12,  7, 0);
		p.interval = 15.0;
		p.azimuth = 270.0; p.elevation = 20.0; p.landscape = true;
		p.camera.model = "EOS R10"; p.camera.name = "R10";
		p.lens.name = "SIGMA 16mm F1.4"; p.lens.fn = 1.4; p.lens.fnMax = 16.0;

		auto n = std::make_shared<hgc::ccmNight>();
		n->name = "星景"; n->limitBright = n->limitDark = n->initial = hgc::exposure{ "1600", "8", "1.4" };
		auto sr = std::make_shared<hgc::ccmSunrise>();
		sr->name = "朝日"; sr->limitBright = hgc::exposure{ "1600", "8", "1.4" };
		sr->limitDark = hgc::exposure{ "100", "1/4000", "16" }; sr->initial = sr->limitBright;
		auto ss = std::make_shared<hgc::ccmSunset>();
		ss->name = "夕日"; ss->limitBright = hgc::exposure{ "1600", "8", "1.4" };
		ss->limitDark = hgc::exposure{ "100", "1/4000", "16" }; ss->initial = ss->limitBright;
		auto dy = std::make_shared<hgc::ccmDay>();
		dy->name = "日中"; dy->limitBright = hgc::exposure{ "1600", "8", "1.4" };
		dy->limitDark = hgc::exposure{ "100", "1/4000", "16" }; dy->initial = dy->limitBright;

		p.ccm.night = n; p.ccm.sunrise = sr; p.ccm.sunset = ss; p.ccm.day = dy;
		return p;
	}

	int countType(const hgc::cs& p, hgc::ccmType t)
	{
		int n = 0;
		for (const auto& w : p.ccmList) { if (w.type == t) { ++n; } }
		return n;
	}

	const hgc::ccmBase* firstOfType(const hgc::cs& p, hgc::ccmType t)
	{
		for (const auto& w : p.ccmList) { if (w.type == t) { return w.ccm.get(); } }
		return nullptr;
	}
}

int main(void)
{
	std::printf("=== 撮影計画が撮影制御方法を所有する構造のテスト ===\n");

	// ---------------------------------------------------------------- ①
	// スケジュールが組めること。窓の実体は「計画が所有する実体そのもの」を指す(複製しない)。
	std::printf("\n[1] 窓は計画所有の実体を指す(複製しない)\n");
	{
		hgc::cs p = makePlan("A");
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "buildSchedule が成功する");
		check(!p.ccmList.empty(), "窓が1つ以上できる");
		check(countType(p, hgc::ccmType::night) > 0, "夜間の窓ができる");
		check(countType(p, hgc::ccmType::day) > 0, "日中の窓ができる");
		// 同じ型の窓は同一インスタンスを指す。
		const hgc::ccmBase* d = firstOfType(p, hgc::ccmType::day);
		check(d == p.ccm.day.get(), "日中の窓は plan.ccm.day と同一インスタンス");
		// 計画の ccm を編集すると窓にも即反映される(コピーではないことの確認)。
		p.ccm.day->name = "日中(編集)";
		checkStr(firstOfType(p, hgc::ccmType::day)->name, "日中(編集)", "ccm を編集すると窓へ反映");
	}

	// ---------------------------------------------------------------- ②
	// 計画ごとに独立していること。2つの計画を交互に扱っても混ざらない。
	// (エッジで後から受けた計画に前の計画の設定が貼り付いた不具合の再発防止)
	std::printf("\n[2] 計画ごとに独立している\n");
	{
		hgc::cs a = makePlan("A");
		hgc::cs b = makePlan("B");
		a.ccm.day->name = "Aの日中"; a.ccm.day->limitBright = hgc::exposure{ "1600", "8", "1.4" };
		b.ccm.day->name = "Bの日中"; b.ccm.day->limitBright = hgc::exposure{ "3200", "8", "1.4" };
		check(astro::buildSchedule(a, 540) == ERR_HGC_OK, "A のスケジュール");
		check(astro::buildSchedule(b, 540) == ERR_HGC_OK, "B のスケジュール");
		checkStr(firstOfType(a, hgc::ccmType::day)->name, "Aの日中", "A は A の設定のまま");
		checkStr(firstOfType(b, hgc::ccmType::day)->name, "Bの日中", "B は B の設定のまま");
		checkStr(firstOfType(a, hgc::ccmType::day)->limitBright.iso, "1600", "A の暗所限界が B に汚染されない");
		checkStr(firstOfType(b, hgc::ccmType::day)->limitBright.iso, "3200", "B の暗所限界が A に汚染されない");
		// A を引き直しても B は動かない。
		check(astro::buildSchedule(a, 540) == ERR_HGC_OK, "A を引き直す");
		checkStr(firstOfType(b, hgc::ccmType::day)->name, "Bの日中", "A の引き直しで B が変わらない");
	}

	// ---------------------------------------------------------------- ③
	// 使う/使わない。使わない型の窓は作られない。実体は保持され、また使うと編集内容が戻る。
	std::printf("\n[3] 使う/使わない(実体は保持する)\n");
	{
		hgc::cs p = makePlan("C");
		p.ccm.useSunrise = true;	// 朝日/夕日は既定で「使わない」(2026-08-17)。このテストは明示的に使う
		p.ccm.sunrise->name = "朝日(ユーザー編集)";
		p.ccm.sunrise->limitDark = hgc::exposure{ "200", "1/2000", "11" };
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "使う状態でスケジュール");
		const int sunriseWindows = countType(p, hgc::ccmType::sunrise);
		check(sunriseWindows > 0, "朝日の窓ができる");

		p.ccm.useSunrise = false;
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "使わない状態でスケジュール");
		check(countType(p, hgc::ccmType::sunrise) == 0, "朝日の窓が無くなる");
		check(p.ccm.sunrise != nullptr, "使わなくても実体は残る");
		checkStr(p.ccm.sunrise->name, "朝日(ユーザー編集)", "使わない間も編集内容が残る");

		p.ccm.useSunrise = true;
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "また使う状態でスケジュール");
		check(countType(p, hgc::ccmType::sunrise) == sunriseWindows, "朝日の窓が元どおり戻る");
		checkStr(p.ccm.sunrise->name, "朝日(ユーザー編集)", "戻したとき初期値で上書きされない");
		checkStr(firstOfType(p, hgc::ccmType::sunrise)->limitDark.iso, "200", "編集した限界がそのまま使われる");
	}

	// ---------------------------------------------------------------- ④
	// 保存/復元。一式は1回だけ書かれ、窓は型で解決される。往復して内容が保たれる。
	std::printf("\n[4] 保存と復元(一式1回 + 窓は型のみ)\n");
	{
		hgc::cs p = makePlan("D");
		p.ccm.day->name = "日中D";
		p.ccm.night->limitBright = hgc::exposure{ "3200", "6", "2.0" };
		p.ccm.useSunset  = false;
		p.ccm.useSunrise = true;	// 既定は「使わない」。true/false 両方が往復することを見る
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "スケジュール");
		const std::string js = csjson::toJson(p);

		// 窓ごとに実体を書いていない(旧形式の名残が無い)ことを、サイズと内容で確かめる。
		check(js.find("\"planCcm\"") == std::string::npos, "別枠 planCcm を書かない");
		check(js.find("\"sunriseMode\"") == std::string::npos, "旧 bandMode を書かない");
		check(js.find("\"useSunset\":false") != std::string::npos, "使う/使わないを書く");

		hgc::cs q;
		check(csjson::fromJson(js, q), "復元できる");
		checkStr(q.ccm.day ? q.ccm.day->name : "", "日中D", "日中の設定が保たれる");
		checkStr(q.ccm.night ? q.ccm.night->limitBright.iso : "", "3200", "夜間の限界が保たれる");
		check(q.ccm.useSunset == false, "使わない指定が保たれる");
		check(q.ccm.useSunrise == true, "使う指定が保たれる");
		check(q.ccmList.size() == p.ccmList.size(), "窓の数が保たれる");
		// 復元後も窓は所有実体を指す(複製されていない)。
		const hgc::ccmBase* qd = firstOfType(q, hgc::ccmType::day);
		check(qd != nullptr && qd == q.ccm.day.get(), "復元後も窓は所有実体を指す");
		// 復元した計画の ccm を編集しても、元の計画には影響しない。
		if (q.ccm.day) { q.ccm.day->name = "変更後"; }
		checkStr(p.ccm.day->name, "日中D", "復元先を編集しても元の計画は変わらない");
	}

	// ---------------------------------------------------------------- ④-2
	// 既定値(2026-08-17 ユーザー指示):
	//  ・夜間↔移行の境界は -12°(航海薄明)。以前は -18°(天文薄明)だった
	//  ・朝日/夕日(太陽を直接撮る)は既定で使わない
	std::printf("\n[4-2] 既定値(夜間境界 -12° / 朝日・夕日は使わない)\n");
	{
		hgc::cs p = makePlan("F");
		check(p.ccm.useSunrise == false, "朝日は既定で使わない");
		check(p.ccm.useSunset  == false, "夕日は既定で使わない");
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "既定のスケジュール");
		check(countType(p, hgc::ccmType::sunrise) == 0, "既定では朝日の窓ができない");
		check(countType(p, hgc::ccmType::sunset)  == 0, "既定では夕日の窓ができない");
		check(countType(p, hgc::ccmType::night)   >  0, "夜間の窓はできる");

		// 夜間の窓の始まりが「太陽高度 -12°(下降)」の時刻と一致することを確かめる。
		//  -18° のままなら夜間の始まりはもっと遅い時刻になるので、ここでずれを検出できる。
		const hgc::ccmWindow* nw = nullptr;
		for (const auto& w : p.ccmList) { if (w.ccm && w.ccm->type == hgc::ccmType::night) { nw = &w; break; } }
		check(nw != nullptr, "夜間の窓を取り出せる");
		if (nw != nullptr)
		{
			astro::altTime a12 = astro::sunAltitudeTime(p.place, p.start, -12.0, false, 540);
			astro::altTime a18 = astro::sunAltitudeTime(p.place, p.start, -18.0, false, 540);
			check(a12.valid && a18.valid, "-12°/-18°(下降)の時刻が求まる");
			if (a12.valid && a18.valid)
			{
				// 分類は1分刻みのサンプリング、sunAltitudeTime は解析的な探索なので数分ずれる
				// (大気差の扱いも違う)。**どちらに近いか**で既定値の変更を判定する。
				const long long ns  = hgc::toUnixUtc(nw->start, 540);
				const long long d12 = std::llabs(ns - hgc::toUnixUtc(a12.when, 540));
				const long long d18 = std::llabs(ns - hgc::toUnixUtc(a18.when, 540));
				check(d12 < d18,   "夜間の始まりが -18° より -12° に近い(既定が -12°)");
				check(d12 < 600,   "-12° の時刻との差が10分以内");
			}
		}
	}

	// ---------------------------------------------------------------- ⑤
	// 終了≤開始は何も作らずに失敗を返す。既存の窓を壊さない。
	// (以前は18か所すべてが戻り値を無視しており、黙って空のまま進んでいた)
	std::printf("\n[5] 終了≤開始は失敗を返し、既存の窓を壊さない\n");
	{
		hgc::cs p = makePlan("E");
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "正常なスケジュール");
		const size_t before = p.ccmList.size();
		p.end = p.start;					// 終了=開始
		check(astro::buildSchedule(p, 540) != ERR_HGC_OK, "終了=開始は失敗を返す");
		check(p.ccmList.size() == before, "失敗しても既存の窓を消さない");
	}

	// ---------------------------------------------------------------- ⑥
	// 夜間の固定露出は計画所有の夜間設定から取る(移行のクランプ基準に使われる)。
	std::printf("\n[6] 夜間の固定露出を計画から取る\n");
	{
		hgc::cs p = makePlan("F");
		p.ccm.night->limitBright = hgc::exposure{ "2500", "10", "1.8" };
		p.ccm.night->preNightEv  = -1.0;
		p.ccm.night->postNightEv = 2.0;
		check(astro::buildSchedule(p, 540) == ERR_HGC_OK, "スケジュール");
		checkStr(p.nightFixedExposure.iso, "2500", "夜間の固定露出(iso)");
		checkStr(p.nightFixedExposure.ss,  "10",   "夜間の固定露出(ss)");
		check(p.nightPreNightEv  == -1.0, "夜間前移行の目標ev");
		check(p.nightPostNightEv ==  2.0, "夜間後移行の目標ev");
	}

	std::printf("\n=== 結果: PASS %d / FAIL %d ===\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
