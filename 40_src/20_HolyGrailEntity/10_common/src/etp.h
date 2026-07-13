#ifndef _ETP_H_
#define _ETP_H_
// エッジ端末との通信パケット(Edge Terminal Paket = etp)。データ構造仕様書43 §6。
// 本ヘッダはパケットの組立/解釈のみを担い、トランスポート(UDP/TCP)には依存しない。
// 書式: header(u16=0x8080) cmd(u16) method(u16) length(u32) data[length]
//       terminal(u32=0x01234567) sum(u32)  すべてリトルエンディアン。
// sum は header から terminal までを 4 バイトごとの u32 として総和した値の 2 の補数。
// data は 4 の整数倍。JSON 送出時は末尾を空白で 4 バイト境界に詰める(JSONは末尾空白を無視)。

#include <cstdint>
#include <string>
#include <vector>

namespace etp
{
	constexpr uint16_t HEADER   = 0x8080;
	constexpr uint32_t TERMINAL = 0x01234567;

	// §6.1.2 method
	enum method : uint16_t
	{
		M_GET = 1, M_PUT = 2, M_POST = 3, M_DELETE = 4, M_ACK = 100, M_NAK = 200
	};

	// §6.1.3 cmd
	enum cmd : uint16_t
	{
		C_SEARCH = 1000, C_ACTION = 1, C_STOP = 2, C_PROGRESS = 3,
		C_DIRECTION = 4, C_CAPTURE_PLAN = 5, C_CONTROL_METHOD = 6, C_TIME = 7,
		C_NAME_BMP = 8,	// 計画名のモノクロ2値ビットマップ(width u16LE, height u16LE, 1bpp pixels)
		C_RESEARCH = 9,	// 継続(カメラ未検出時の即再探索)。data=計画id(空=全取得フェーズ)。スマホ→エッジ
		C_CAMERA_INFO = 10,	// オンラインカメラ情報の通知。data=JSON配列[{serial,model,ip,online}]。既知IPテーブル更新。スマホ→エッジ
		C_LOG_LIST = 11,	// ログファイル名一覧の取得。応答 data=JSON配列["hg_YYYY-MM-DD.log",...]。スマホ→エッジ(M_GET)
		C_LOG_READ = 12,	// ログファイルの部分読み出し。data="name\t<offset>"。応答 data=生バイト(<要求長で末尾)。スマホ→エッジ(M_GET)
		C_DELETE_PLAN = 13	// 項目6: 計画の削除(撮影中なら停止してから)。data=計画id。スマホ→エッジ。スマホで停止した計画をエッジからも消す用

	};

	// 解釈結果
	struct packet
	{
		uint16_t    cmd    = 0;
		uint16_t    method = 0;
		std::string data;	// data 部(末尾の詰め空白は除去済み。通常 JSON 文字列)
	};

	// cmd/method と data(JSON等) から 1 パケットのバイト列を組み立てる。
	std::vector<uint8_t> encode(uint16_t cmd, uint16_t method, const std::string& data);

	// バイト列の先頭から 1 パケットを解釈する(TCP ストリームのフレーミングに使える)。
	//  return : >0 = 消費バイト数(out に格納)、0 = データ不足(続きを待つ)、
	//           -1 = ヘッダ/チェックサム不正(1バイト進めて再同期する)
	int decode(const uint8_t* buf, size_t len, packet& out);
}

#endif // _ETP_H_
