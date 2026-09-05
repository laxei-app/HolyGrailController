#include "rawStack.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rawStack
{
	namespace
	{
		// Bayer の 2×2 の 4 位置(00,01,10,11)が R/Gr/Gb/B のどれか。値は「チャネル番号」
		//  (0=R 1=Gr 2=Gb 3=B)。黒レベルは位置順で渡されるので、この表でチャネルへ写す。
		const int kPosToChan[4][4] = {
			{0, 1, 2, 3},	// RGGB
			{1, 0, 3, 2},	// GRBG
			{2, 3, 0, 1},	// GBRG
			{3, 2, 1, 0},	// BGGR
		};

		// 線形 → sRGB の 8bit。表引き(4096 段)で足りる。
		struct srgbLut
		{
			uint8_t v[4097];
			srgbLut(void)
			{
				for (int i = 0; i <= 4096; ++i)
				{
					const double x = i / 4096.0;
					const double y = (x <= 0.0031308) ? (12.92 * x) : (1.055 * std::pow(x, 1.0 / 2.4) - 0.055);
					v[i] = static_cast<uint8_t>(std::lround(std::clamp(y, 0.0, 1.0) * 255.0));
				}
			}
			uint8_t at(float x) const
			{
				if (!(x > 0.0f)) { return 0; }
				if (x >= 1.0f)   { return 255; }
				return v[static_cast<int>(x * 4096.0f)];
			}
		};
		const srgbLut& lut(void) { static const srgbLut t; return t; }

		// 周辺減光の格子を出力座標で引く(両線形)。ch=チャネル番号。
		inline float shadingAt(const developParams& p, int ch, float fx, float fy)
		{
			if (p.shading == nullptr || p.shadingCols < 2 || p.shadingRows < 2) { return 1.0f; }
			const float gx = fx * (p.shadingCols - 1);
			const float gy = fy * (p.shadingRows - 1);
			int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
			if (x0 >= p.shadingCols - 1) { x0 = p.shadingCols - 2; }
			if (y0 >= p.shadingRows - 1) { y0 = p.shadingRows - 2; }
			const float tx = gx - x0, ty = gy - y0;
			const float* m = p.shading + static_cast<size_t>(ch) * p.shadingCols * p.shadingRows;
			const float a = m[y0 * p.shadingCols + x0],       b = m[y0 * p.shadingCols + x0 + 1];
			const float c = m[(y0 + 1) * p.shadingCols + x0], d = m[(y0 + 1) * p.shadingCols + x0 + 1];
			return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
		}
	}

	void accumulator::begin(int width, int height, int cfaPattern)
	{
		w_ = width & ~1; h_ = height & ~1;
		cfa_ = (cfaPattern >= 0 && cfaPattern <= 3) ? cfaPattern : RGGB;
		frames_ = 0;
		const size_t n = static_cast<size_t>(w_ / 2) * static_cast<size_t>(h_ / 2);
		r_.assign(n, 0); g_.assign(n, 0); b_.assign(n, 0);
	}

	bool accumulator::add(const uint8_t* data, size_t bytes, int rowStrideBytes)
	{
		if (w_ <= 0 || h_ <= 0 || data == nullptr) { return false; }
		if (rowStrideBytes < w_ * 2) { return false; }
		if (bytes < static_cast<size_t>(rowStrideBytes) * static_cast<size_t>(h_ - 1) + static_cast<size_t>(w_) * 2) { return false; }
		const int* pc = kPosToChan[cfa_];
		// 4 位置のうちどれが R / B か、残り2つが G。位置→面 の振り分けを行の外で決める。
		const int ow = w_ / 2;
		for (int y = 0; y < h_; y += 2)
		{
			const uint16_t* row0 = reinterpret_cast<const uint16_t*>(data + static_cast<size_t>(y) * rowStrideBytes);
			const uint16_t* row1 = reinterpret_cast<const uint16_t*>(data + static_cast<size_t>(y + 1) * rowStrideBytes);
			uint32_t* pr = &r_[static_cast<size_t>(y / 2) * ow];
			uint32_t* pg = &g_[static_cast<size_t>(y / 2) * ow];
			uint32_t* pb = &b_[static_cast<size_t>(y / 2) * ow];
			for (int x = 0; x < w_; x += 2)
			{
				const uint32_t v[4] = { row0[x], row0[x + 1], row1[x], row1[x + 1] };
				uint32_t r = 0, g = 0, b = 0;
				for (int k = 0; k < 4; ++k)
				{
					const int ch = pc[k];
					if (ch == 0) { r += v[k]; } else if (ch == 3) { b += v[k]; } else { g += v[k]; }
				}
				pr[x / 2] += r; pg[x / 2] += g; pb[x / 2] += b;
			}
		}
		++frames_;
		return true;
	}

	bool accumulator::develop(const developParams& p, uint8_t* out, size_t outBytes) const
	{
		const int ow = w_ / 2, oh = h_ / 2;
		if (ow <= 0 || oh <= 0 || out == nullptr) { return false; }
		if (outBytes < static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4) { return false; }
		const int f = (frames_ > 0) ? frames_ : 1;
		const int* pc = kPosToChan[cfa_];
		// 黒レベル(位置順で来る)をチャネル順へ。G は 2 画素の和なので黒も 2 つ分引く。
		float blackCh[4] = {0, 0, 0, 0};
		for (int k = 0; k < 4; ++k) { blackCh[pc[k]] = p.black[k]; }
		const float blackR = blackCh[0] * f, blackG = (blackCh[1] + blackCh[2]) * f, blackB = blackCh[3] * f;
		// 飽和までの幅。**コマ数で割らない**――足した分だけ明るくなるのが「長秒露光」そのもの。
		//  (N コマ分の露光として振る舞い、N×飽和 で白く飛ぶ)
		const float range = std::max(1.0f, static_cast<float>(p.whiteLevel) - (blackCh[0] + blackCh[3]) * 0.5f);
		const float invR = 1.0f / range, invG = 0.5f / range, invB = 1.0f / range;	// G は 2 画素の平均
		const float gR = p.gains[0], gG = 0.5f * (p.gains[1] + p.gains[2]), gB = p.gains[3];
		const srgbLut& L = lut();

		for (int y = 0; y < oh; ++y)
		{
			const float fy = (oh > 1) ? static_cast<float>(y) / (oh - 1) : 0.0f;
			const uint32_t* pr = &r_[static_cast<size_t>(y) * ow];
			const uint32_t* pg = &g_[static_cast<size_t>(y) * ow];
			const uint32_t* pb = &b_[static_cast<size_t>(y) * ow];
			uint8_t* o = out + static_cast<size_t>(y) * ow * 4;
			for (int x = 0; x < ow; ++x)
			{
				const float fx = (ow > 1) ? static_cast<float>(x) / (ow - 1) : 0.0f;
				float r = (static_cast<float>(pr[x]) - blackR) * invR;
				float g = (static_cast<float>(pg[x]) - blackG) * invG;
				float b = (static_cast<float>(pb[x]) - blackB) * invB;
				// 周辺減光を掛け戻し、ホワイトバランスを載せる
				r *= gR * shadingAt(p, 0, fx, fy);
				g *= gG * 0.5f * (shadingAt(p, 1, fx, fy) + shadingAt(p, 2, fx, fy));
				b *= gB * shadingAt(p, 3, fx, fy);
				// 飽和は WB の後で揃える(色付きの白飛びを避ける)
				r = std::min(r, 1.0f); g = std::min(g, 1.0f); b = std::min(b, 1.0f);
				const float R = p.ccm[0] * r + p.ccm[1] * g + p.ccm[2] * b;
				const float G = p.ccm[3] * r + p.ccm[4] * g + p.ccm[5] * b;
				const float B = p.ccm[6] * r + p.ccm[7] * g + p.ccm[8] * b;
				o[x * 4 + 0] = L.at(R);
				o[x * 4 + 1] = L.at(G);
				o[x * 4 + 2] = L.at(B);
				o[x * 4 + 3] = 255;
			}
		}
		return true;
	}
}
