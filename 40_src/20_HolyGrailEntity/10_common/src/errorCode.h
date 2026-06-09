#ifndef  _ERROR_CODE_H_
#define  _ERROR_CODE_H_

enum errCode : uint32_t
{
	ERR_HGC_OK = 0,             // OK

	// Device Discovery 
	ERR_HGC_NOT_FOUND,			// デバイスが見つからない
	ERR_HGC_API_LIST,			// api リスト取得失敗
	ERR_HGC_HTTP_POST,			// http post 失敗
	ERR_HGC_HTTP_PUT,			// http put 失敗

	// camera interface
	ERR_HGC_DEVICE_DESCRIPTOR,	// device descriptor 解析失敗
	ERR_HGC_NOT_SUPPORTED,		// サポートしていない api 等
	ERR_HGC_API_ANALIZE,		// API からの回答の解析失敗
    ERR_HGC_NOT_LIVE_DETAIL,    // live view の付帯情報ではない
    ERR_HGC_NOT_LIVE_FORMAT,    // live view のフォーマット異常
	ERR_HGC_TAKE_FAIL,			// 取得できなかった
	ERR_HGC_READY,				// 準備不足
	ERR_HGC_NO_ELEMENT,			// 要素が無い
    ERR_HGC_HTTP_GET,           // http get の失敗
    ERR_HGC_STRAGE_ACT,         // アクティブなストレージ情報取得失敗
    ERR_HGC_STRAGE_INF,         // ストレージ情報取得失敗
    ERR_HGC_DIR_ACT,            // アクティブなディレクトリ取得失敗
    ERR_HGC_LAST_FILE,          // 最終ファイル取得失敗
    ERR_HGC_RDY_METARING,       // rdyMetaring()失敗
    ERR_HGC_HISTO_ELEMENT,      // ヒストグラムの要素数が異なる
    ERR_HGC_HISTO_BIN,          // ヒストグラムの bin の数が異なる

	// net 関連
	ERR_HGC_NET_INIT,			// net の初期化失敗
	ERR_HGC_NET_NO_RECEIPT,		// 受付拒否
	ERR_HGC_NET_URL_FAIL,		// URL 誤り
	ERR_NET_SSDP,				// SSDP 失敗	

	// その他汎用
	ERR_HGC_TIMEOUT,			// タイムアウト
	ERR_HGC_NET_THREAD,			// 同期エラー

	// UI インターフェース
	ERR_HGC_BUF_SHORT,			// 取得用バッファの容量不足(必要バイト数を返す)
	ERR_HGC_INVALID_ARG,		// 引数不正
	ERR_HGC_INVALID_STATE,		// 状態不正(その操作が許されない状態)
	ERR_HGC_JSON_PARSE,			// JSON 解析失敗

	//
};


#endif // _ERROR_CODE_H_
