#ifndef _STD_TEMPLATES_H_
#define _STD_TEMPLATES_H_
// 【標準ひな形(2026-09-21 ユーザー指示)】「ひな形のセットを使って何も変更せずとも撮影できる」ための
//  撮影計画ひな形一式を、インストール時にカメラごとに自動で作る。
//
//  種類は 8 つ(内蔵カメラは光条なしの 4 つ):
//    星景/夜景 × 日の出含む/日の入含む × (光条あり/なし)
//    星景 = 夜間の ss を NPF 未満の最大に / 夜景 = その半分(街明かりが飛ばないように)
//    光条 = 朝日(夕日)と日中の F を 11〜16 に絞る(太陽の光条を出す。絞りが固定のカメラには作らない)
//    日の出含む = 21:00〜翌 09:00、朝は 夜間→移行 -12° / 移行→朝日 -3° / 朝日→日中 +3°、
//                夕は 日中→移行 -3° / 移行→夜間 -12°(夕日は使わない)
//    日の入含む = 15:00〜翌 03:00、その鏡像(夕日を使い、朝日は使わない)
//
//  名前(ひな形の題名・撮影制御方法の名前)は UI の言語で渡す(Entity と通信路に日本語を置かない決まり)。
//  種類の識別子(tplKind)は英語の固定文字列で、「同じカメラ・同じ種類」が既にあれば作らない
//  (利用者が名前を変えたり消したりしたものを起動のたびに作り直さない)。
//
//  ここは共通部分なので機種の判断を書かない。夜間の露出・明所の限界・周期の規則・絞りの固定は
//  呼ぶ側(内蔵カメラ=builtinRegister / ミラーレス=hge_seedStandardTemplates)が答える。
#include "hgcCommon.h"
#include "cs.h"
#include <string>
#include <map>

namespace stdtpl
{
	// 種類(英語の識別子。JSON の tplKind とひな形の名前の鍵に使う)
	enum class kind : uint8_t
	{
		starSunrise = 0,		// "star_sunrise"          Starscape with Sunrise
		nightSunrise,			// "night_sunrise"         Nightscape with Sunrise
		starSunriseSunstar,		// "star_sunrise_sunstar"  Starscape with Sunrise (Sunstar)
		nightSunriseSunstar,	// "night_sunrise_sunstar" Nightscape with Sunrise (Sunstar)
		starSunset,				// "star_sunset"           Starscape with Sunset
		nightSunset,			// "night_sunset"          Nightscape with Sunset
		starSunsetSunstar,		// "star_sunset_sunstar"   Starscape with Sunset (Sunstar)
		nightSunsetSunstar,		// "night_sunset_sunstar"  Nightscape with Sunset (Sunstar)
		count
	};
	const char* key(kind k);
	bool isSunstar(kind k);
	bool isSunrise(kind k);		// 日の出含む(偽=日の入含む)
	bool isStar(kind k);		// 星景(偽=夜景)

	// UI から渡る名前。tpl[key] = ひな形の名前(カメラ名の後ろに付ける)、
	//  night 等 = 撮影制御方法の名前。無いものは英語の識別子/型名になる。
	struct names
	{
		std::map<std::string, std::string> tpl;
		std::string night, sunrise, sunset, day;
	};
	// namesJson: {"tpl":{"star_sunrise":"…",…}, "ccm":{"night":"…","sunrise":"…","sunset":"…","day":"…"}, "night":"…",…}
	//  ccmKey: 撮影制御方法の名前を読む場所。"ccm" ならそのオブジェクト、空なら最上位(スマホ用初期値の名前と同じ)。
	names parseNames(const std::string& namesJson, const char* ccmKey);

	// そのカメラの実力(呼ぶ側が答える)
	struct gear
	{
		hgc::camera   camera;			// 所持カメラ(iso/ss の並びを含む)
		hgc::lens     lens;				// 組み合わせるレンズ
		hgc::exposure starNight;		// 星景の夜間露出(NPF 以下の最大 ss・開放・ISO1600。解決済みの文字列)
		hgc::exposure cityNight;		// 夜景の夜間露出(ss がその半分)
		hgc::exposure bright;			// 明所限界(ISO100・1/8000・F16。出せない端末は端で止めた値)
		double        starInterval = 0.0;	// 撮影周期[秒](星景)
		double        cityInterval = 0.0;	// 撮影周期[秒](夜景)
		bool          fnFixed  = false;	// 絞りが固定(光条の種類を作らない)
		bool          forPhone = false;	// 撮影制御方法の forPhone(初期値エディタの目盛り切替)
	};

	// 1 種類ぶんのひな形を組み立てる(保存はしない)。
	void build(const gear& g, kind k, const names& nm, hgc::cs& out);
	// 無いものだけ作って保存する。戻り=作った数。withSunstar=偽なら光条の種類を作らない。
	int seed(const gear& g, const names& nm, bool withSunstar);
}

#endif // _STD_TEMPLATES_H_
