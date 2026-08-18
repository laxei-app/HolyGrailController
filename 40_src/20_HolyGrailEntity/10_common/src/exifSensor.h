#ifndef _EXIF_SENSOR_H_
#define _EXIF_SENSOR_H_

#include <cstdint>
#include <cstddef>

// 撮影画像の EXIF からセンサーの実寸と画素数を読む(2026-08-19)。
//
// 【なぜ要るか】センサー横/縦[mm]と横画素数は、機材マスターに載っている機種でしか埋まらない。
//  マスターに無い機種は空のままで、NPFも撮影シミュレーションも出せない。
//  CCAPI はセンサーの寸法も画素数も返さない(deviceinformation にもどこにも無い。実測)。
//  ところが**撮影した画像の EXIF には入っている**。
//    ・PixelXDimension / PixelYDimension … 画素数
//    ・FocalPlaneXResolution / YResolution … 1インチ(または1cm)あたりの画素数
//    センサー横[mm] = PixelX / FocalPlaneXResolution × 25.4
//  実測(EOS R50 V): 6000 / 6825.938567 × 25.4 = 22.33mm、4000 / 同 = 14.88mm。
//  カタログ値(APS-C 22.3×14.9)と一致する。
//
// 【先頭だけで足りる】上のタグはファイル先頭付近にあるので、本体を全部落とす必要は無い。
//  実測で CR3(25MB)の先頭 64KB に入っていた。JPEG は APP1 が先頭にあるのでなお確実。
//
// 【解析の方針】CR3 は ISO-BMFF、JPEG は APP1 と容器が違うが、どちらも中身は
//  TIFF ヘッダ("II*\0" か "MM\0*")で始まる IFD である。容器を解く代わりに
//  バッファから TIFF ヘッダを探して IFD を読む。容器の種類に依存しないので、
//  将来 HEIF など別の形式が来ても同じ処理で通る。
namespace exifSensor
{
	// 読み取れた諸元。値が入っているものだけ true 側のフィールドが埋まる。
	struct spec
	{
		double   sensorWmm = 0.0;	// センサー横[mm]
		double   sensorHmm = 0.0;	// センサー縦[mm]
		uint32_t pixelW    = 0;		// センサー横[pixel]
		uint32_t pixelH    = 0;		// センサー縦[pixel]
		bool ok(void) const { return (sensorWmm > 0.0 && pixelW > 0); }
	};

	// buf から諸元を読む。true = 使える値が揃った。
	//  縮小画像(kind=display 等)を渡すと FocalPlaneResolution も縮小に合わせて書き換えられて
	//  いるため**センサー寸法は正しく出る**が、画素数は縮小後の値になる。画素数まで要るときは
	//  元画像(kind=main)の先頭を渡すこと。
	bool parse(const uint8_t* buf, size_t len, spec& out);
}

#endif // _EXIF_SENSOR_H_
