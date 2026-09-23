#ifndef _DNG_WRITE_H_
#define _DNG_WRITE_H_
// 加算した RAW を DNG(TIFF 構造)として書き出す(2026-09-23 ユーザー指示)。
//
// 【なぜ自前で書くか】Android の DngCreator は「センサーが吐いた 1 コマの生データ」を包む道具で、
//  何コマも足して桁が増えた値を渡せない(飽和値も黒レベルも端末の諸元のまま書かれる)。
//  こちらは足した結果を 16 ビットいっぱいへ伸ばし、飽和値・黒レベル・色の行列を自分で書く。
//
// 【中身】Bayer のまま(CFA)、フルサイズ、無圧縮 16 ビット。現像に要る情報はタグで渡す:
//   ・BlackLevel = 0 / WhiteLevel = 65535(加算ぶんを込みで伸ばしてあるため)
//   ・AsShotNeutral … 撮影時のホワイトバランス(掛けずにタグで渡す。後から自由に振れる)
//   ・ColorMatrix1 … XYZ(D65) → センサー。端末が答えた色行列から作る
//   ・周辺減光だけは掛けてから書く(掛け戻しの地図を DNG の作法で渡す手が重いため。
//     Apple の ProRAW も同じ流儀。隅の増幅ぶんは全体を縮めて飽和させない)
//
// 【大きさ】4080×3072 で約 25MB/コマ。呼ぶ側が「残すかどうか」を利用者に選ばせること。
#include <cstdint>
#include <cstddef>

namespace rawStack
{
	struct developParams;	// rawStack.h

	// 書き出しに要る、現像用とは別の情報。
	struct dngInfo
	{
		const char* maker   = "builtin";
		const char* model   = "phone";
		const char* software = "TwyLapse";
		const char* dateTime = "";		// "YYYY:MM:DD HH:MM:SS"(空なら書かない)
		double exposureSec  = 0.0;		// 実際の合計露光(加算ぶんを含む)
		int    iso          = 0;
		double focalMm      = 0.0;
		double fnumber      = 0.0;
	};

	// fd へ書く(ギャラリーへ出すため、開くのは Kotlin 側)。戻り=書けたか。
	//  cfaSum: フルサイズの加算値(画素ごと)。w×h。frames: 足したコマ数。
	bool writeDng(int fd, const uint32_t* cfaSum, int w, int h, int cfaPattern,
	              const developParams& p, const dngInfo& info);
}

#endif // _DNG_WRITE_H_
