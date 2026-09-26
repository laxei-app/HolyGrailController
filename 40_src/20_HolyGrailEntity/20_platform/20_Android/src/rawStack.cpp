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

	}

	// 周辺減光の格子を出力座標で引く(両線形)。ch=チャネル番号。
	float shadingAt(const developParams& p, int ch, float fx, float fy)
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

	void accumulator::begin(int width, int height, int cfaPattern, bool keepFull)
	{
		w_ = width & ~1; h_ = height & ~1;
		cfa_ = (cfaPattern >= 0 && cfaPattern <= 3) ? cfaPattern : RGGB;
		frames_ = 0;
		const size_t n = static_cast<size_t>(w_ / 2) * static_cast<size_t>(h_ / 2);
		r_.assign(n, 0); g_.assign(n, 0); b_.assign(n, 0);
		// DNG は Bayer のままフルサイズで出すので、束ねる前の和も持つ(要るときだけ)。
		if (keepFull) { full_.assign(static_cast<size_t>(w_) * static_cast<size_t>(h_), 0); }
		else          { full_.clear(); full_.shrink_to_fit(); }
	}

	bool accumulator::add(const uint8_t* data, size_t bytes, int rowStrideBytes)
	{
		if (w_ <= 0 || h_ <= 0 || data == nullptr) { return false; }
		if (rowStrideBytes < w_ * 2) { return false; }
		if (bytes < static_cast<size_t>(rowStrideBytes) * static_cast<size_t>(h_ - 1) + static_cast<size_t>(w_) * 2) { return false; }
		// 【書きかけのコマは足さない(2026-09-23)】HAL がバッファを落とすと、上の方だけ書かれた
		//  画像がそのまま届くことがある(実測: 3072 行のうち 660 行だけ。logcat の
		//  "capture buffer lost" と同時刻)。足すと上の方だけ二重露光になり、現像も測光も狂う。
		//  最後の行が丸ごと 0 で、先頭の行が 0 でなければ「書きかけ」と見て断る
		//  (黒レベルは 64 前後あるので、本当に暗い夜空でも 0 一色にはならない)。
		{
			const uint16_t* first = reinterpret_cast<const uint16_t*>(data);
			const uint16_t* last  = reinterpret_cast<const uint16_t*>(
			                            data + static_cast<size_t>(h_ - 1) * rowStrideBytes);
			bool lastZero = true, firstZero = true;
			for (int x = 0; x < w_; ++x)
			{
				if (last[x]  != 0) { lastZero  = false; }
				if (first[x] != 0) { firstZero = false; }
				if (!lastZero) { break; }
			}
			if (lastZero && !firstZero) { return false; }
		}
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
			uint32_t* f0 = full_.empty() ? nullptr : &full_[static_cast<size_t>(y) * w_];
			uint32_t* f1 = full_.empty() ? nullptr : &full_[static_cast<size_t>(y + 1) * w_];
			for (int x = 0; x < w_; x += 2)
			{
				const uint32_t v[4] = { row0[x], row0[x + 1], row1[x], row1[x + 1] };
				if (f0 != nullptr)
				{
					f0[x] += v[0]; f0[x + 1] += v[1]; f1[x] += v[2]; f1[x + 1] += v[3];
				}
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

// ── 画質の目安(2026-09-26) ──────────────────────────────────
namespace
{
	// 【測る場所は「一番悪いところ」(ユーザー指示)】画面を升目に割って升ごとに測り、
	//  SN比の悪い順に並べて**下位10%の位置**を代表値にする。最小値そのものを採ると、
	//  揺れる木の葉のように「動いた升」が必ず1位になってしまう。
	constexpr int kBlock      = 16;		// 升の大きさ[画素]
	constexpr int kEveryFrame = 20;		// 何コマに1度測るか
	constexpr int kWorstPct   = 10;		// 下位何%を代表値にするか
	constexpr int kLevelMin   = 8;		// 真っ黒な升は除く(SN比が意味を持たない)
	constexpr int kLevelMax   = 250;	// 飽和した升も除く(ノイズが見えない)
	constexpr double kFloorSigma = 0.29;	// 量子化の下限(1/√12)。これ未満は測れていない

	// 【ばらつきは中央値から出す(標準偏差では測れない)】コマの間隔は数十秒あるので、
	//  升の中で雲や葉が動く。標準偏差はその数画素に引きずられて何倍にもなり、
	//  「カメラのノイズ」ではなく「風の強さ」を測ってしまう。中央絶対偏差なら、
	//  升の半分未満しか動いていない限り動いた画素を無視できる。1.4826 は正規分布の換算。
	double madSigma(std::vector<double>& v)
	{
		if (v.size() < 8) { return 0.0; }
		const size_t mid = v.size() / 2;
		std::nth_element(v.begin(), v.begin() + mid, v.end());
		const double m = v[mid];
		for (size_t i = 0; i < v.size(); ++i) { v[i] = std::fabs(v[i] - m); }
		std::nth_element(v.begin(), v.begin() + mid, v.end());
		return 1.4826 * v[mid];
	}

	std::vector<uint8_t>  g_prevGray;	// 直前のコマ(測る回だけ持つ)
	int                   g_gw = 0, g_gh = 0;
	int                   g_frameNo = 0;
	rawStack::noiseStat   g_stat;

	void toGray(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out)
	{
		out.resize(static_cast<size_t>(w) * h);
		for (size_t i = 0, n = out.size(); i < n; ++i)
		{
			const uint8_t* p = rgba + i * 4;
			const uint32_t y = (299u * p[0] + 587u * p[1] + 114u * p[2] + 500u) / 1000u;
			out[i] = static_cast<uint8_t>(y > 255u ? 255u : y);
		}
	}
}

namespace rawStack
{
	void noiseReset(void)
	{
		g_prevGray.clear(); g_prevGray.shrink_to_fit();
		g_gw = g_gh = 0; g_frameNo = 0; g_stat = noiseStat{};
	}

	bool noiseTake(noiseStat& out)
	{
		if (!g_stat.ok) { return false; }
		out = g_stat; g_stat = noiseStat{};
		return true;
	}

	void noisePush(const uint8_t* rgba, int w, int h)
	{
		if (rgba == nullptr || w < kBlock * 4 || h < kBlock * 4) { return; }
		++g_frameNo;
		const bool measureNow = (!g_prevGray.empty() && g_gw == w && g_gh == h);
		if (!measureNow)
		{
			// 次のコマで測る回だけ、直前のコマを控える(毎コマ持つと無駄に写す)。
			// 短い撮影でも1件は残るよう、3コマ目で1度測ってから以降は kEveryFrame ごと。
			if (g_frameNo == 2 || (g_frameNo % kEveryFrame) == (kEveryFrame - 1))
			{
				toGray(rgba, w, h, g_prevGray); g_gw = w; g_gh = h;
			}
			return;
		}
		std::vector<uint8_t> cur;
		toGray(rgba, w, h, cur);

		struct cell { double level, temporal, spatial; };
		std::vector<cell> cells;
		cells.reserve(static_cast<size_t>((w / kBlock) * (h / kBlock)));
		std::vector<double> lv, dv, sv;
		for (int by = 0; by + kBlock <= h; by += kBlock)
		{
			for (int bx = 0; bx + kBlock <= w; bx += kBlock)
			{
				lv.clear(); dv.clear();
				for (int j = 0; j < kBlock; ++j)
				{
					const size_t row = static_cast<size_t>(by + j) * w + bx;
					for (int i = 0; i < kBlock; ++i)
					{
						lv.push_back(cur[row + i]);
						dv.push_back(static_cast<double>(cur[row + i]) - g_prevGray[row + i]);
					}
				}
				// 明るさも中央値で見る(動いた画素に引かれないように)。
				std::nth_element(lv.begin(), lv.begin() + lv.size() / 2, lv.end());
				const double ma = lv[lv.size() / 2];
				if (ma < kLevelMin || ma > kLevelMax) { continue; }

				// 【面のざらつきは2階差分で見る】升の中の明暗の傾きや模様をそのまま測ると、
				//  空と木の境目のような「絵」がノイズに化ける。周りとの差の差(ラプラシアン)
				//  を採れば、なだらかな傾きは消えてざらつきだけが残る。6 は核の大きさぶん。
				sv.clear();
				for (int j = 1; j + 1 < kBlock; ++j)
				{
					const size_t row = static_cast<size_t>(by + j) * w + bx;
					for (int i = 1; i + 1 < kBlock; ++i)
					{
						const double lap =
							1.0 * cur[row - w + i - 1] - 2.0 * cur[row - w + i] + 1.0 * cur[row - w + i + 1]
						  - 2.0 * cur[row     + i - 1] + 4.0 * cur[row     + i] - 2.0 * cur[row     + i + 1]
						  + 1.0 * cur[row + w + i - 1] - 2.0 * cur[row + w + i] + 1.0 * cur[row + w + i + 1];
						sv.push_back(lap);
					}
				}
				cell c;
				c.level    = ma;
				c.temporal = madSigma(dv) / std::sqrt(2.0);	// 2コマぶんなので √2 で割る
				c.spatial  = madSigma(sv) / 6.0;
				if (c.temporal < kFloorSigma) { c.temporal = kFloorSigma; }
				cells.push_back(c);
			}
		}
		g_prevGray.clear(); g_prevGray.shrink_to_fit(); g_gw = g_gh = 0;
		if (cells.size() < 16) { return; }
		std::sort(cells.begin(), cells.end(), [](const cell& a, const cell& b) {
			return (a.level / a.temporal) < (b.level / b.temporal);	// SN比の悪い順
		});
		const cell& c = cells[cells.size() * kWorstPct / 100];
		g_stat.ok       = true;
		g_stat.level    = c.level;
		g_stat.temporal = c.temporal;
		g_stat.fixed    = std::sqrt(std::max(0.0, c.spatial * c.spatial - c.temporal * c.temporal));
		g_stat.snr      = c.level / c.temporal;
	}
}
