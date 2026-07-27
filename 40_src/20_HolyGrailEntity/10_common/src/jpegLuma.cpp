#include "jpegLuma.h"
#include <jpeg/hgc_tjpgd.h>
#include <cstring>
#include <vector>

namespace
{
	// 復号セッション(入力ストリームと出力先ヒストグラム)。
	struct ctx
	{
		const uint8_t* data = nullptr;
		size_t         len  = 0;
		size_t         pos  = 0;
		uint16_t*      hist = nullptr;
		uint32_t       yTop = 0;	// この行から
		uint32_t       yEnd = 0;	// この行の手前まで集計(レターボックス除去)
	};

	// 入力: メモリ上のJPEGから読み出す(TJpgDecのinfunc。第1引数は jd->device = ctx*)。
	uint32_t inFunc(void* dev, uint8_t* buf, uint32_t n)
	{
		ctx* c = static_cast<ctx*>(dev);
		const uint32_t remain = static_cast<uint32_t>(c->len - c->pos);
		if (n > remain) { n = remain; }
		if (buf != nullptr) { std::memcpy(buf, c->data + c->pos, n); }
		c->pos += n;	// buf==nullptr はスキップ指示
		return n;
	}

	// 出力: MCUブロック(RGB888)を輝度へ落としてヒストグラムに積む(画像は保持しない)。
	//  第1引数は jd->device = ctx*。
	uint32_t outFunc(void* dev, void* bitmap, JRECT* rect)
	{
		ctx* c = static_cast<ctx*>(dev);
		const uint8_t* p = static_cast<const uint8_t*>(bitmap);
		for (uint32_t y = rect->top; y <= rect->bottom; ++y)
		{
			const bool count = (y >= c->yTop && y < c->yEnd);
			for (uint32_t x = rect->left; x <= rect->right; ++x, p += 3)
			{
				if (!count) { continue; }
				// Rec.601 輝度(整数演算)。
				const uint32_t lum = (299u * p[0] + 587u * p[1] + 114u * p[2] + 500u) / 1000u;
				++c->hist[lum > 255u ? 255u : lum];
			}
		}
		return 1;	// 続行
	}
}

namespace jpglm
{
	bool lumaHistogram(const uint8_t* data, size_t len,
	                   uint16_t hist[256], int& wOut, int& hOut, double cropRatio)
	{
		wOut = hOut = 0;
		std::memset(hist, 0, sizeof(uint16_t) * 256);
		if (data == nullptr || len == 0) { return false; }

		std::vector<uint8_t> pool(3900);	// TJpgDecの作業領域(必要量は実測~3.1KB+余裕)
		ctx c;
		c.data = data; c.len = len; c.hist = hist;

		lgfxJdec jd;
		std::memset(&jd, 0, sizeof(jd));
		if (hgc_jd_prepare(&jd, inFunc, pool.data(), static_cast<uint_fast16_t>(pool.size()), &c) != JDR_OK)
		{
			return false;
		}
		wOut = jd.width; hOut = jd.height;
		if (cropRatio < 0.0) { cropRatio = 0.0; }
		if (cropRatio > 0.4) { cropRatio = 0.4; }
		c.yTop = static_cast<uint32_t>(jd.height * cropRatio + 0.5);
		c.yEnd = jd.height - c.yTop;
		if (c.yEnd <= c.yTop) { c.yTop = 0; c.yEnd = jd.height; }

		return hgc_jd_decomp(&jd, outFunc, 0) == JDR_OK;
	}
}
