#ifndef _HOLY_GRAIL_ENTITY_H_
#define _HOLY_GRAIL_ENTITY_H_
// モジュール構造仕様書(47) §2 UIインターフェース。
// UI(Kotlin/Swift/C++) と Entity(C++) の境界となる extern "C" の C-ABI。
// 全関数は即 return し、長処理はワーカースレッドで実行、結果は通知CBで返す。
// 開発ステップ2.1 MVP サブセット(固定データで撮影開始/停止)。

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 通知イベント種別 (47 §2.3)
enum hgeEvent
{
	HGE_EV_STATE    = 1,	// 状態変化       {"state":int}
	HGE_EV_PROGRESS = 2,	// 撮影進捗       {"frame","total","remainSec","elapsedSec"}
	HGE_EV_CAPTURED = 3,	// 1枚撮影完了    {"frame","iso","ss","fn","luminance"}
	HGE_EV_DEVICE   = 4,	// デバイス検出   [{"uuid","model","serialno"},...]
	HGE_EV_SCHEDULE = 5,	// スケジュール   {"name","events":[...],"windows":[...]}
	HGE_EV_ERROR    = 6		// エラー         {"code":int,"msg":string}
};

// 撮影状態 (47 §2.3)
enum hgeState
{
	HGE_ST_IDLE      = 0,
	HGE_ST_SEARCHING = 1,
	HGE_ST_READY     = 2,
	HGE_ST_CAPTURING = 3,
	HGE_ST_STOPPING  = 4,
	HGE_ST_ERROR     = 5
};

// 通知コールバック型。json は呼び出し中のみ有効(Entity 所有)。
// UI 側は自スレッドへ post して即 return する責務(47 §1.3)。
typedef void (*hgeNotifyCb)(int32_t event, const char* json, int32_t len, void* user);

// --- ライフサイクル ---
int32_t     hge_init(void);			// 初期化(ネット準備等)
int32_t     hge_term(void);			// 終了・解放
const char* hge_version(void);		// バージョン文字列

// --- 通知登録 ---
int32_t hge_setNotify(hgeNotifyCb cb, void* user);

// --- 撮影計画(MVP は固定データ) ---
int32_t hge_loadFixedPlan(void);	// 固定データの撮影計画を生成し保持する

// 撮影計画のスケジュールを JSON で取得(バッファ規約)。
//  buf が null か容量不足なら必要バイト数を *inoutLen に格納し ERR_HGC_BUF_SHORT。
int32_t hge_getScheduleJson(char* buf, int32_t* inoutLen);

// --- 撮影実行 ---
int32_t hge_captureStart(void);		// 撮影開始(非同期)。進捗は HGE_EV_PROGRESS
int32_t hge_captureStop(void);		// 撮影停止
int32_t hge_getState(void);			// 現在状態(hgeState)を即時返す

#ifdef __cplusplus
}
#endif

#endif // _HOLY_GRAIL_ENTITY_H_
