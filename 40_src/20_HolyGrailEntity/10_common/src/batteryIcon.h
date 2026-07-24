#ifndef _BATTERY_ICON_H_
#define _BATTERY_ICON_H_
// バッテリ残量アイコンの描画(CoreS3/StickS3 共通)。LovyanGFX のキャンバスへ描く。
//
// 【形】左側がプラス電極の電池マーク。本体は横長の矩形で、左端に小さな電極の出っ張り。
//   ┌╴────────┐
//   │██ ██ ██ │  ← 中を3段に区切り、残量ぶんだけ塗る(左から)
//   └╴────────┘
//   ・level5=3/3, level4=2/3, level3=1/3, level2=0/3(呼び出し側で点滅させる)
//   ・電極は左端(=プラス極を左に置く指定)。
//
// StickS3 の最下段はキーガイダンス+時刻で埋まっているため、小さめ(既定 幅18px)にして詰める。

#include <cstdint>
#include "batteryLevel.h"

namespace batt
{
	// アイコンの標準サイズ。StickS3 の最下段に収まるよう小さめにしてある。
	constexpr int kIconW = 18;	// 電極を含む全体の幅[px]
	constexpr int kIconH = 9;	// 高さ[px]

	// (x,y)を左上として電池アイコンを描く。
	//  gfx  : LGFX 互換キャンバス(drawRect/fillRect/color565 を持つもの)
	//  bars : 塗る段数(0〜3)。batt::bars(level) で得る
	//  col  : 枠と目盛りの色
	// 幅は kIconW 固定(桁位置が動かないよう、残量に依らず同じ場所・同じ幅で描く)。
	template <class GFX>
	void drawIcon(GFX& gfx, int x, int y, int bars, uint16_t col)
	{
		const int tipW = 2;					// 左の電極(プラス極)の出っ張り
		const int bodyX = x + tipW;
		const int bodyW = kIconW - tipW;
		// 電極(左端・縦中央)
		gfx.fillRect(x, y + kIconH / 2 - 1, tipW, 3, col);
		// 本体の枠
		gfx.drawRect(bodyX, y, bodyW, kIconH, col);
		// 中の3段。枠の内側に1px余白を取る。
		// **プラス側(左)から減る**ように、残量ぶんを「右詰め」で塗る。
		//  例) 2/3 なら 左の1段が空き、右2段が塗られる。
		//  (左詰めにすると右から空いていき「マイナス側から減る」見た目になってしまう)
		const int inX = bodyX + 2;
		const int inY = y + 2;
		const int inH = kIconH - 4;
		const int segW = (bodyW - 4 - 2) / 3;	// 3段+段間2pxぶんを引いて等分
		const int n = (bars < 0) ? 0 : (bars > 3 ? 3 : bars);
		for (int i = 3 - n; i < 3; ++i)			// 右側の n 段を塗る
		{
			gfx.fillRect(inX + i * (segW + 1), inY, segW, inH, col);
		}
	}

	// レベルに応じた色。level2(点滅)は赤、level3は黄、それ以上は白。
	//  gfx.color565 を使うためテンプレートにしてある。
	template <class GFX>
	uint16_t iconColor(GFX& gfx, level l)
	{
		switch (l)
		{
			case level::empty: return gfx.color565(0xFF, 0x55, 0x55);	// 残りわずか=赤
			case level::low:   return gfx.color565(0xFF, 0xCC, 0x33);	// 1/3=黄
			default:           return gfx.color565(0xFF, 0xFF, 0xFF);	// 白
		}
	}
}

#endif // _BATTERY_ICON_H_
