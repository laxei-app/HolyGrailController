#ifndef _JPEG_LUMA_H_
#define _JPEG_LUMA_H_
// JPEGバイト列(撮影画像のサムネイル等)を復号して輝度ヒストグラムを作る。
// 撮影画像フィードバック測光(apiCanonCCAPI::meterSceneShot)の下請け。
// デコーダは lib/jpeg/hgc_tjpgd (TJpgDec改変版)。画像全体は保持せず、
// 出力コールバック内でヒストグラムへ直接積むのでRAM消費は僅か(プール約3.5KB+ヒスト1KB)。
#include <cstdint>
#include <cstddef>

namespace jpglm
{
	// data/len のJPEGを復号し、輝度(Rec.601)ヒストグラム hist[256] を作る。
	//  cropRatio: 上下それぞれ画像高さのこの比率の行を捨てる(サムネイルのレターボックス黒帯対策。
	//             実測: 160x120サムネは3:2画像に上下黒帯が付き中央値が約0.2段下がる)。
	//  wOut/hOut: 画像サイズ(診断用)。
	//  return: 復号に成功したか。
	// 【受け皿は 32 ビット(2026-09-20)】以前は uint16 で、1 ビンの上限が 65,535 だった。
	//  内蔵カメラの画像は 2040x1536 = 313 万画素あるので、同じ明るさの画素が全体の 2.1% を
	//  超えるとビンが溢れて巻き戻る。平らな白い空はこれを軽く超え、明側が数えられずに
	//  中央値が暗く出ていた(実測 朝の窓辺: 真値 173 に対し 138、露出が 0.7 段明るく仕上がる)。
	bool lumaHistogram(const uint8_t* data, size_t len,
	                   uint32_t hist[256], int& wOut, int& hOut, double cropRatio);
}

#endif // _JPEG_LUMA_H_
