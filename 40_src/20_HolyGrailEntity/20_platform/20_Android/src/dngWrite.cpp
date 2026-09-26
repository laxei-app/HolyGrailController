// 加算した RAW を DNG として書き出す(dngWrite.h の説明を参照)。
#include "dngWrite.h"
#include "rawStack.h"
#include <unistd.h>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace rawStack
{
	namespace
	{
		// ── TIFF の小道具(リトルエンディアン固定) ──────────────────
		enum tiffType : uint16_t { T_BYTE = 1, T_ASCII = 2, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5,
		                           T_UNDEFINED = 7, T_SRATIONAL = 10 };

		struct entry
		{
			uint16_t tag = 0, type = 0;
			uint32_t count = 0;
			uint32_t value = 0;			// 4 バイトに収まる値、または外に置いたデータの位置
			std::vector<uint8_t> blob;	// 4 バイトを超える中身(位置は書き出し時に決める)
		};

		void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF); }
		void put32(std::vector<uint8_t>& v, uint32_t x)
		{
			v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
			v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
		}

		entry eShort(uint16_t tag, uint16_t a)
		{
			entry e; e.tag = tag; e.type = T_SHORT; e.count = 1; e.value = a; return e;
		}
		entry eShorts(uint16_t tag, const uint16_t* a, uint32_t n)
		{
			entry e; e.tag = tag; e.type = T_SHORT; e.count = n;
			if (n <= 2) { e.value = a[0] | (n > 1 ? (static_cast<uint32_t>(a[1]) << 16) : 0u); }
			else { for (uint32_t i = 0; i < n; ++i) { put16(e.blob, a[i]); } }
			return e;
		}
		entry eLong(uint16_t tag, uint32_t a)
		{
			entry e; e.tag = tag; e.type = T_LONG; e.count = 1; e.value = a; return e;
		}
		entry eAscii(uint16_t tag, const char* s)
		{
			entry e; e.tag = tag; e.type = T_ASCII;
			const size_t n = std::strlen(s) + 1;
			e.count = static_cast<uint32_t>(n);
			if (n <= 4) { std::memcpy(&e.value, s, n); }
			else { e.blob.assign(s, s + n); }
			return e;
		}
		entry eBytes(uint16_t tag, const uint8_t* a, uint32_t n)
		{
			entry e; e.tag = tag; e.type = T_BYTE; e.count = n;
			if (n <= 4) { std::memcpy(&e.value, a, n); }
			else { e.blob.assign(a, a + n); }
			return e;
		}
		// 有理数。分母は 10000 固定(色の行列やゲインはこの精度で足りる)。
		entry eRational(uint16_t tag, const double* a, uint32_t n, bool signedType, uint32_t den = 10000)
		{
			entry e; e.tag = tag; e.type = signedType ? T_SRATIONAL : T_RATIONAL; e.count = n;
			for (uint32_t i = 0; i < n; ++i)
			{
				const double v = a[i] * den;
				if (signedType) { put32(e.blob, static_cast<uint32_t>(static_cast<int32_t>(std::lround(v)))); }
				else            { put32(e.blob, static_cast<uint32_t>(std::lround(std::max(0.0, v)))); }
				put32(e.blob, den);
			}
			return e;
		}

		// 3×3 の逆行列(行優先)。戻り=求まったか。
		bool invert3(const float* m, double* out)
		{
			const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
			const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
			if (std::fabs(det) < 1e-12) { return false; }
			const double id = 1.0 / det;
			out[0] = (e * i - f * h) * id; out[1] = (c * h - b * i) * id; out[2] = (b * f - c * e) * id;
			out[3] = (f * g - d * i) * id; out[4] = (a * i - c * g) * id; out[5] = (c * d - a * f) * id;
			out[6] = (d * h - e * g) * id; out[7] = (b * g - a * h) * id; out[8] = (a * e - b * d) * id;
			return true;
		}

		bool writeAllFd(int fd, const void* p, size_t n)
		{
			const uint8_t* q = static_cast<const uint8_t*>(p);
			while (n > 0)
			{
				const ssize_t w = ::write(fd, q, n);
				if (w <= 0) { return false; }
				q += w; n -= static_cast<size_t>(w);
			}
			return true;
		}

		// CFA の位置(0..3 = 左上・右上・左下・右下)がどの色か。rawStack.cpp と同じ並び。
		//  0=R 1=G 2=G 3=B を DNG の CFAPattern(0=赤 1=緑 2=青)へ直す。
		void cfaPatternBytes(int cfa, uint8_t out[4])
		{
			static const uint8_t kPat[4][4] = {
				{ 0, 1, 1, 2 },	// RGGB
				{ 1, 0, 2, 1 },	// GRBG
				{ 1, 2, 0, 1 },	// GBRG
				{ 2, 1, 1, 0 },	// BGGR
			};
			const int k = (cfa >= 0 && cfa <= 3) ? cfa : 0;
			for (int i = 0; i < 4; ++i) { out[i] = kPat[k][i]; }
		}

		// 位置 → 黒レベル/周辺減光の面番号(R, Gr, Gb, B の順)。rawStack.cpp の kPosToChan と同じ。
		void posToChan(int cfa, int out[4])
		{
			static const int kPosToChan[4][4] = {
				{ 0, 1, 2, 3 },	// RGGB: R Gr / Gb B
				{ 1, 0, 3, 2 },	// GRBG
				{ 2, 3, 0, 1 },	// GBRG
				{ 3, 2, 1, 0 },	// BGGR
			};
			const int k = (cfa >= 0 && cfa <= 3) ? cfa : 0;
			for (int i = 0; i < 4; ++i) { out[i] = kPosToChan[k][i]; }
		}
	}

	bool writeDng(int fd, const uint32_t* cfaSum, int w, int h, int cfaPattern,
	              const developParams& p, const dngInfo& info)
	{
		if (fd < 0 || cfaSum == nullptr || w <= 0 || h <= 0) { return false; }
		const int frames = (p.frames > 0) ? p.frames : 1;

		// ── 画素の伸ばし方を決める ──────────────────────────────
		//  加算値は (黒×コマ数)〜(白×コマ数)。黒を引いて 0〜65535 へ伸ばす。
		//  周辺減光を掛けるぶん、隅ほど値が増えるので、その最大値も込みで縮めて飽和させない。
		int chan[4]; posToChan(cfaPattern, chan);
		float blackPos[4];	// 位置ごとの黒レベル(左上・右上・左下・右下)
		for (int k = 0; k < 4; ++k) { blackPos[k] = p.black[chan[k]]; }
		const float blackAvg = (blackPos[0] + blackPos[1] + blackPos[2] + blackPos[3]) * 0.25f;
		const double range = std::max(1.0, static_cast<double>(p.whiteLevel) - blackAvg) * frames;
		double maxShade = 1.0;
		if (p.shading != nullptr && p.shadingCols >= 2 && p.shadingRows >= 2)
		{
			const size_t n = static_cast<size_t>(4) * p.shadingCols * p.shadingRows;
			for (size_t i = 0; i < n; ++i) { maxShade = std::max(maxShade, static_cast<double>(p.shading[i])); }
		}
		const double scale = 65535.0 / (range * maxShade);

		// ── 画素を作る(行ごとに書き出す) ───────────────────────
		std::vector<uint16_t> line(static_cast<size_t>(w));

		// ── タグ ────────────────────────────────────────────────
		std::vector<entry> ifd;
		ifd.push_back(eLong(254, 0));							// NewSubfileType = 本画像
		ifd.push_back(eLong(256, static_cast<uint32_t>(w)));	// ImageWidth
		ifd.push_back(eLong(257, static_cast<uint32_t>(h)));	// ImageLength
		ifd.push_back(eShort(258, 16));							// BitsPerSample
		ifd.push_back(eShort(259, 1));							// Compression = 無圧縮
		ifd.push_back(eShort(262, 32803));						// PhotometricInterpretation = CFA
		ifd.push_back(eAscii(271, info.maker));					// Make
		ifd.push_back(eAscii(272, info.model));					// Model
		ifd.push_back(eLong(273, 0));							// StripOffsets(後で入れる)
		ifd.push_back(eShort(274, 1));							// Orientation
		ifd.push_back(eShort(277, 1));							// SamplesPerPixel
		ifd.push_back(eLong(278, static_cast<uint32_t>(h)));	// RowsPerStrip = 全部で1枚
		ifd.push_back(eLong(279, static_cast<uint32_t>(w) * h * 2));	// StripByteCounts
		ifd.push_back(eShort(284, 1));							// PlanarConfiguration
		ifd.push_back(eAscii(305, info.software));				// Software
		if (info.dateTime != nullptr && info.dateTime[0] != '\0') { ifd.push_back(eAscii(306, info.dateTime)); }
		{
			const uint16_t dim[2] = { 2, 2 };
			ifd.push_back(eShorts(33421, dim, 2));				// CFARepeatPatternDim
			uint8_t pat[4]; cfaPatternBytes(cfaPattern, pat);
			ifd.push_back(eBytes(33422, pat, 4));				// CFAPattern
		}
		{
			const uint8_t ver[4] = { 1, 4, 0, 0 };
			ifd.push_back(eBytes(50706, ver, 4));				// DNGVersion
			const uint8_t bver[4] = { 1, 1, 0, 0 };
			ifd.push_back(eBytes(50707, bver, 4));				// DNGBackwardVersion
		}
		ifd.push_back(eAscii(50708, info.model));				// UniqueCameraModel
		{
			const double bl[1] = { 0.0 };
			ifd.push_back(eRational(50714, bl, 1, false, 1));	// BlackLevel(引いてあるので 0)
		}
		ifd.push_back(eLong(50717, 65535));						// WhiteLevel
		{
			// AsShotNeutral … 無彩色がセンサーでどう写るか = ホワイトバランスの逆数。
			const double gR = std::max(1e-6f, p.gains[0]);
			const double gG = std::max(1e-6f, (p.gains[1] + p.gains[2]) * 0.5f);
			const double gB = std::max(1e-6f, p.gains[3]);
			const double neutral[3] = { gG / gR, 1.0, gG / gB };
			ifd.push_back(eRational(50728, neutral, 3, false));
		}
		{
			// ColorMatrix1 … XYZ(D65) → センサー。端末の「センサー → 線形 sRGB」の逆行列に
			//  XYZ(D65) → 線形 sRGB を掛ける。答えない端末では単位行列が入っているので、
			//  そのときは sRGB と同じ扱いになる(色は後から振ればよい)。
			static const double kXyzToSrgb[9] = {
				 3.2404542, -1.5371385, -0.4985314,
				-0.9692660,  1.8760108,  0.0415560,
				 0.0556434, -0.2040259,  1.0572252 };
			double inv[9];
			if (!invert3(p.ccm, inv))
			{
				for (int i = 0; i < 9; ++i) { inv[i] = (i % 4 == 0) ? 1.0 : 0.0; }
			}
			double cm[9];
			for (int r = 0; r < 3; ++r)
			{
				for (int c = 0; c < 3; ++c)
				{
					double s = 0.0;
					for (int k = 0; k < 3; ++k) { s += inv[r * 3 + k] * kXyzToSrgb[k * 3 + c]; }
					cm[r * 3 + c] = s;
				}
			}
			ifd.push_back(eRational(50721, cm, 9, true));
			ifd.push_back(eShort(50778, 21));					// CalibrationIlluminant1 = D65
		}
		{
			// 何コマ足したか・実際の露光は説明として残す(読み手が事情を分かるように)。
			char note[160];
			std::snprintf(note, sizeof(note),
			              "TwyLapse stacked %d frames, total exposure %.3fs, ISO %d, shading applied",
			              frames, info.exposureSec, info.iso);
			ifd.push_back(eAscii(270, note));					// ImageDescription
		}

		std::sort(ifd.begin(), ifd.end(), [](const entry& a, const entry& b) { return a.tag < b.tag; });

		// ── 位置決め: ヘッダ(8) + IFD + 外置きデータ + 画素 ──────
		const uint32_t ifdOffset = 8;
		const uint32_t ifdBytes  = 2 + static_cast<uint32_t>(ifd.size()) * 12 + 4;
		uint32_t blobPos = ifdOffset + ifdBytes;
		for (auto& e : ifd)
		{
			if (!e.blob.empty())
			{
				if (blobPos & 1) { ++blobPos; }		// 偶数境界へ
				e.value = blobPos;
				blobPos += static_cast<uint32_t>(e.blob.size());
			}
		}
		const uint32_t pixelPos = (blobPos & 1) ? blobPos + 1 : blobPos;
		for (auto& e : ifd) { if (e.tag == 273) { e.value = pixelPos; } }	// StripOffsets

		// ── 書き出し ────────────────────────────────────────────
		std::vector<uint8_t> head;
		put16(head, 0x4949); put16(head, 42); put32(head, ifdOffset);	// "II", 42, IFD の位置
		put16(head, static_cast<uint16_t>(ifd.size()));
		for (const auto& e : ifd)
		{
			put16(head, e.tag); put16(head, e.type); put32(head, e.count); put32(head, e.value);
		}
		put32(head, 0);		// 次の IFD 無し
		for (const auto& e : ifd)
		{
			if (e.blob.empty()) { continue; }
			if (head.size() + ifdOffset < e.value) { head.push_back(0); }	// 詰め物
			head.insert(head.end(), e.blob.begin(), e.blob.end());
		}
		while (head.size() + ifdOffset < pixelPos) { head.push_back(0); }
		if (!writeAllFd(fd, head.data(), head.size())) { return false; }

		for (int y = 0; y < h; ++y)
		{
			const uint32_t* src = cfaSum + static_cast<size_t>(y) * w;
			const float fy = (h > 1) ? static_cast<float>(y) / (h - 1) : 0.0f;
			for (int x = 0; x < w; ++x)
			{
				const int pos = (y & 1) * 2 + (x & 1);
				const float fx = (w > 1) ? static_cast<float>(x) / (w - 1) : 0.0f;
				const float sh = shadingAt(p, chan[pos], fx, fy);
				double v = (static_cast<double>(src[x]) - static_cast<double>(blackPos[pos]) * frames) * sh * scale;
				if (v < 0.0) { v = 0.0; }
				if (v > 65535.0) { v = 65535.0; }
				line[static_cast<size_t>(x)] = static_cast<uint16_t>(v + 0.5);
			}
			if (!writeAllFd(fd, line.data(), line.size() * 2)) { return false; }
		}
		return true;
	}
}
