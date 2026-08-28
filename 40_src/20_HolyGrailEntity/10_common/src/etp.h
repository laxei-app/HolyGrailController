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
		C_DELETE_PLAN = 13,	// 項目6: 計画の削除(撮影中なら停止してから)。data=計画id。スマホ→エッジ。スマホで停止した計画をエッジからも消す用
		// --- 撮影レポートの回収(2026-08-05) ---
		// エッジで撮った撮影のレポートはエッジ側のディスクに溜まり、従来スマホから見る道が無かった。
		// スマホの常時スイープ(30秒)が検索応答の "reports" 件数を見て、1件以上あるときだけ取りに行く。
		// 保存できたことを確かめてから削除を指示するので、途中で通信が切れてもレポートは失われない
		// (次のスイープで再取得する)。1件は約1KBなので、定常の通信量には実質影響しない。
		C_REPORT_LIST = 14,	// レポート一覧の取得。応答 data=JSON配列(reportListJson と同じ形)。スマホ→エッジ(M_GET)
		C_REPORT_READ = 15,	// レポート1件の取得。data=ファイル名。応答 data=そのJSON本文。スマホ→エッジ(M_GET)
		C_REPORT_DELETE = 16,	// 受領済みレポートの削除。data=ファイル名。スマホ→エッジ(M_DELETE)
		C_CAMERA_BOOK = 17,	// スマホの所持カメラ台帳(識別+認証)。data=JSON配列。スマホ→エッジ(M_PUT)
		//  [{"serial","model","name","assignedName","authUser","authPass"}]。authPass は暗号文。
		//  【送った内容がその時点の全量】エッジは受け取った配列でそっくり置き換える。
		//   追加・変更・削除がこの1本で片付き、消すためのコマンドを別に持たなくてよい。
		//  【なぜ要るか】エッジは撮影計画を受け取るまでカメラの資格情報を1つも持てなかった。
		//   ところがカメラへの挨拶(初回Wi-Fi参加を成立させる200)は計画を作る**前**に要る。
		//   isoList/ssList は挨拶に不要なので載せない(4台で3.5KB→571B。BLEでも一瞬で済む)。
		C_LOG_OPT = 18	// デバッグログの取捨。data={"shot":真偽,"batt":真偽}。スマホ→エッジ(M_PUT)
		//  撮影1コマごとの記録(SHOT/LVHIST)と電池の定期記録は量が多く、一晩で数千行になる。
		//  肝心の出来事が埋もれ、エッジでは書き込みが仕事を奪い保存領域も食う。**既定は採らない**。
		//  必要なときだけスマホの「デバッグログ」画面で入れてもらい、その設定をここで送る。
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
