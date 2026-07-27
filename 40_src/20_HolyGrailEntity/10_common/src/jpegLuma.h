#ifndef _JPEG_LUMA_H_
#define _JPEG_LUMA_H_
// JPEGバイト列(撮影画像のサムネイル等)を復号して輝度ヒストグラムを作る。
// 撮影画像フィードバック測光(apiCanonCCAPI::meterSceneShot)の下請け。
// デコーダは lib/jpeg/hgc_tjpgd (TJpgDec改変版)。画像全体は保持せず、
// 出力コールバック内でヒストグラムへ直接積むのでRAM消費は僅か(プール約3.5KB+ヒスト512B)。
#include <cstdint>
#include <cstddef>

namespace jpglm
{
	// data/len のJPEGを復号し、輝度(Rec.601)ヒストグラム hist[256] を作る。
	//  cropRatio: 上下それぞれ画像高さのこの比率の行を捨てる(サムネイルのレターボックス黒帯対策。
	//             実測: 160x120サムネは3:2画像に上下黒帯が付き中央値が約0.2段下がる)。
	//  wOut/hOut: 画像サイズ(診断用)。
	//  return: 復号に成功したか。
	bool lumaHistogram(const uint8_t* data, size_t len,
	                   uint16_t hist[256], int& wOut, int& hOut, double cropRatio);
}

#endif // _JPEG_LUMA_H_
