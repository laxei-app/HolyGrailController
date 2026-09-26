#ifndef _RAW_STACK_H_
#define _RAW_STACK_H_
// RAW(Bayer)を何コマも足して1枚にする(2026-09-06)。
//
// 【なぜ足すか】スマホのセンサーは1コマの露光に上限がある(Pixel 6 広角で 8.3秒)。星空には
//  20〜48秒が欲しいので、上限以下のコマを続けて撮って**線形の画素値で足す**。三脚固定なら
//  星の流れ方も光子の総量も本物の長秒露光と同じになる(読み出しノイズだけ √N 倍増える)。
//
// 【JPEG ではなく RAW を足す理由】JPEG は階調カーブがかかった非線形の 8bit で、暗い空が
//  数階調に潰れている。足しても階段が残るだけで情報は増えない。RAW は線形で 10bit 以上
//  あるので素直に足せる。
//
// 【現像は 2×2 束ねだけ】最終成果物は 1920×1440 の動画なので、Bayer の 2×2 をひとつの
//  RGB 画素にまとめれば解像度は足りる(4080×3072 → 2040×1536)。デモザイクが要らず、
//  ノイズもさらに半分になる。ホワイトバランスと色変換の行列は撮影結果(CaptureResult)が
//  返すものをそのまま使う。周辺減光の地図も同じく撮影結果から受け取って掛け戻す。
//
// 【1コマでも同じ道を通す】上限以下の露光でも RAW → この現像 を通す。コマ数で経路が
//  変わると、境目で色や階調が跳んで動画に段差が出るため。
//
// ここは純粋な C++(JNI に依存しない)。Kotlin からの入口は jniBridge.cpp にある。
#include <cstdint>
#include <cstddef>
#include <vector>

namespace rawStack
{
	// Bayer の並び(Camera2 の SENSOR_INFO_COLOR_FILTER_ARRANGEMENT と同じ値)。
	enum cfa : int { RGGB = 0, GRBG = 1, GBRG = 2, BGGR = 3 };

	// 現像に要る値。すべて撮影結果(CaptureResult)と諸元(CameraCharacteristics)から埋める。
	struct developParams
	{
		int    frames     = 1;			// 足したコマ数(黒レベルをその分引く)
		int    whiteLevel = 1023;		// 飽和値(SENSOR_INFO_WHITE_LEVEL)
		float  black[4]   = {64, 64, 64, 64};	// 黒レベル(R, Gr, Gb, B の順。Bayer の並びに依らずこの順)
		float  gains[4]   = {1, 1, 1, 1};		// ホワイトバランス(R, Gr, Gb, B)
		float  ccm[9]     = {1,0,0, 0,1,0, 0,0,1};	// センサーRGB → 線形 sRGB の 3×3(行優先)
		// 周辺減光の地図(R, Gr, Gb, B の順に4面。無ければ cols=0)。画面全体を cols×rows に割った格子。
		const float* shading = nullptr;
		int    shadingCols = 0, shadingRows = 0;
	};

	// 周辺減光の格子を出力座標(0〜1)で引く(両線形)。ch=面番号(R, Gr, Gb, B)。
	//  現像と DNG 書き出しの両方で使う。
	float shadingAt(const developParams& p, int ch, float fx, float fy);

	// ── 画質の目安(2026-09-26 ユーザー依頼) ─────────────────
	// 【なぜ2コマ要るか】1コマの中のばらつきには、被写体の模様とノイズが混ざっていて分けられない。
	//  連続する2コマの**差**を取れば模様は消え、コマごとに変わるノイズだけが残る。
	//  さらに「1コマの中のばらつき」と比べれば、毎コマ同じ位置に出るノイズ(暗電流のムラ・
	//  ホットピクセル)も分けられる。加算撮影ではこちらが薄まらないので、別に見る意味がある。
	//   時間ノイズ   = σ(A − B) ÷ √2        … コマごとに変わる(光ショット+読み出し)
	//   固定パターン = √(空間σ² − 時間σ²)   … 毎コマ同じ位置に出る
	struct noiseStat
	{
		bool   ok       = false;
		double level    = 0.0;	// その場所の明るさ(0〜255)
		double temporal = 0.0;	// 時間ノイズ
		double fixed    = 0.0;	// 固定パターンノイズ
		double snr      = 0.0;	// level ÷ temporal(大きいほど良い)
	};
	// 現像したコマを渡す(RGBA。jniBridge が現像の直後に呼ぶ)。数コマに1度だけ測る。
	void noisePush(const uint8_t* rgba, int w, int h);
	// 測れていれば取り出して空にする(戻り=取り出せたか)。
	bool noiseTake(noiseStat& out);
	// 1回の撮影の始めに呼ぶ(持ち越さない)。
	void noiseReset(void);

	// 足し込み先。1コマ目の前に begin、コマごとに add、最後に develop。
	class accumulator
	{
	public:
		// 幅・高さは RAW の画素数(偶数へ切り下げて使う)。
		//  keepFull=真 なら DNG 用にフルサイズの加算(Bayer のまま)も持つ(4080×3072 で 50MB)。
		void begin(int width, int height, int cfaPattern, bool keepFull = false);
		// 1コマ足す。data は 16bit little-endian の Bayer。rowStride はバイト単位。
		//  戻り=足せたか(寸法違いなど)。
		bool add(const uint8_t* data, size_t bytes, int rowStrideBytes);
		// 2×2 束ねで現像して RGBA(8bit×4、行優先、幅=width/2、高さ=height/2)へ書く。
		//  out は (width/2)*(height/2)*4 バイト以上あること。戻り=書けたか。
		bool develop(const developParams& p, uint8_t* out, size_t outBytes) const;

		int  width(void)  const { return w_; }
		int  height(void) const { return h_; }
		int  frames(void) const { return frames_; }
		int  outWidth(void)  const { return w_ / 2; }
		int  outHeight(void) const { return h_ / 2; }
		int  cfaPattern(void) const { return cfa_; }
		// フルサイズの加算(DNG 用)。持っていなければ nullptr。
		const uint32_t* fullSum(void) const { return full_.empty() ? nullptr : full_.data(); }

	private:
		int w_ = 0, h_ = 0, cfa_ = RGGB, frames_ = 0;
		// 束ねた面ごとの和(R, G(2画素の和), B)。uint32 で十分(16bit × 数十コマ)。
		std::vector<uint32_t> r_, g_, b_;
		std::vector<uint32_t> full_;	// Bayer のままの和(DNG 用。要るときだけ確保する)
	};
}

#endif // _RAW_STACK_H_
