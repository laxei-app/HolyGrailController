#include "exifSensor.h"

#include <cstring>

namespace
{
	// EXIF タグ番号(TIFF/Exif 仕様)。
	constexpr uint16_t kPixelX        = 0xA002;	// PixelXDimension
	constexpr uint16_t kPixelY        = 0xA003;	// PixelYDimension
	constexpr uint16_t kFocalPlaneXRs = 0xA20E;	// FocalPlaneXResolution
	constexpr uint16_t kFocalPlaneYRs = 0xA20F;	// FocalPlaneYResolution
	constexpr uint16_t kFocalPlaneUnt = 0xA210;	// FocalPlaneResolutionUnit (2=inch, 3=cm)
	constexpr uint16_t kExifIfd       = 0x8769;	// Exif IFD へのポインタ

	// 1つの IFD に入っているエントリ数の上限。これを超える値は「TIFF ヘッダに見えた別のデータ」
	//  と判断して読み飛ばす(誤検出したまま延々と舐めないための歯止め)。
	constexpr uint32_t kMaxEntries = 512;
	// 追いかける IFD の数の上限(IFD0 → Exif IFD で足りる。循環参照よけも兼ねる)。
	constexpr int      kMaxIfd     = 8;

	inline uint16_t rd16(const uint8_t* p, bool le)
	{
		return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
		          : static_cast<uint16_t>((p[0] << 8) | p[1]);
	}
	inline uint32_t rd32(const uint8_t* p, bool le)
	{
		return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
		             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
		          : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
		             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
	}

	// 1つの TIFF ブロック(base から始まる)を読んで、欲しいタグを拾う。
	struct picked
	{
		double   fpx = 0.0, fpy = 0.0;
		uint16_t unit = 0;
		uint32_t px = 0, py = 0;
		bool enough(void) const { return (fpx > 0.0 && px > 0); }
	};

	bool scanTiff(const uint8_t* buf, size_t len, size_t base, picked& got)
	{
		if (base + 8 > len) { return false; }
		bool le;
		if      (std::memcmp(buf + base, "II\x2A\x00", 4) == 0) { le = true;  }
		else if (std::memcmp(buf + base, "MM\x00\x2A", 4) == 0) { le = false; }
		else { return false; }

		uint32_t next[kMaxIfd];
		int      nNext = 0;
		next[nNext++] = rd32(buf + base + 4, le);

		for (int i = 0; i < nNext && i < kMaxIfd; ++i)
		{
			const size_t o = base + next[i];
			if (o + 2 > len) { continue; }
			const uint32_t n = rd16(buf + o, le);
			if (n == 0 || n > kMaxEntries) { continue; }
			if (o + 2 + static_cast<size_t>(n) * 12 > len) { continue; }

			for (uint32_t e = 0; e < n; ++e)
			{
				const uint8_t* p   = buf + o + 2 + static_cast<size_t>(e) * 12;
				const uint16_t tag = rd16(p, le);
				const uint16_t typ = rd16(p + 2, le);
				const uint8_t* vp  = p + 8;	// 4バイト以内の値はここに直接入る

				if (tag == kExifIfd)
				{	// Exif IFD へ降りる(FocalPlane 系と PixelXDimension はこの中)
					if (nNext < kMaxIfd) { next[nNext++] = rd32(vp, le); }
					continue;
				}
				switch (tag)
				{
				case kPixelX: case kPixelY:
				{
					const uint32_t v = (typ == 3) ? rd16(vp, le) : rd32(vp, le);
					if (tag == kPixelX) { got.px = v; } else { got.py = v; }
					break;
				}
				case kFocalPlaneXRs: case kFocalPlaneYRs:
				{
					if (typ != 5) { break; }				// RATIONAL 以外は想定外
					const size_t vo = base + rd32(vp, le);	// 8バイトなので値は別の場所
					if (vo + 8 > len) { break; }
					const uint32_t num = rd32(buf + vo, le);
					const uint32_t den = rd32(buf + vo + 4, le);
					if (den == 0) { break; }
					const double v = static_cast<double>(num) / static_cast<double>(den);
					if (tag == kFocalPlaneXRs) { got.fpx = v; } else { got.fpy = v; }
					break;
				}
				case kFocalPlaneUnt:
					got.unit = rd16(vp, le);
					break;
				default:
					break;
				}
			}
		}
		return got.enough();
	}
}

namespace exifSensor
{
	bool parse(const uint8_t* buf, size_t len, spec& out)
	{
		if (buf == nullptr || len < 16) { return false; }

		// TIFF ヘッダを総当たりで探す。CR3 は moov の中の CMT ボックス、JPEG は APP1 の
		//  "Exif\0\0" の直後にある。位置は容器ごとに違うので、容器を解かずに印を探す。
		//  先頭64KB程度しか渡ってこないので総当たりでも軽い。
		for (size_t i = 0; i + 8 <= len; ++i)
		{
			if (!((buf[i] == 'I' && buf[i + 1] == 'I' && buf[i + 2] == 0x2A && buf[i + 3] == 0x00) ||
			      (buf[i] == 'M' && buf[i + 1] == 'M' && buf[i + 2] == 0x00 && buf[i + 3] == 0x2A)))
			{ continue; }

			picked got;
			if (!scanTiff(buf, len, i, got)) { continue; }

			// 単位: 2=インチ / 3=センチ。未指定はインチ扱い(EXIF の既定)。
			const double perUnitMm = (got.unit == 3) ? 10.0 : 25.4;
			out.pixelW    = got.px;
			out.pixelH    = got.py;
			out.sensorWmm = static_cast<double>(got.px) / got.fpx * perUnitMm;
			const double fpy = (got.fpy > 0.0) ? got.fpy : got.fpx;
			out.sensorHmm = (got.py > 0) ? (static_cast<double>(got.py) / fpy * perUnitMm) : 0.0;

			// 桁が明らかにおかしい値は採らない(誤検出よけ)。
			//  現行のセンサーは対角で言えば数mm〜60mm程度に収まる。
			if (out.sensorWmm < 1.0 || out.sensorWmm > 100.0) { out = spec(); continue; }
			if (out.sensorHmm < 0.0 || out.sensorHmm > 100.0) { out = spec(); continue; }
			return out.ok();
		}
		return false;
	}
}
