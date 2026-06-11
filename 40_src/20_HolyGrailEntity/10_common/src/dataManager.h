#ifndef _DATA_MANAGER_H_
#define _DATA_MANAGER_H_
// データの保持・取得を集約するモジュール。
// 現状は出荷時設定(データ構造仕様書43 §7.2: ファイルではなくコード上に保持し、
// 読み込み失敗時やリセット時のフォールバックとして使用する)を提供する。
// 将来はマスタ/ユーザー資産/撮影計画(cs)の永続化もここに集約する。

#include "hgcCommon.h"
#include "ccm.h"
#include "cs.h"
#include "astroSched.h"		// astro::ccmSet

class dataManager
{
public:
	// 出荷時設定の撮影制御方法一式(夜間/朝日/夕日/日中)。
	static astro::ccmSet factoryCcmSet(void);

	// 出荷時設定の露出平滑化(ヒステリシス1段, 移動平均5フレーム)。
	static hgc::exposureSmoothing factorySmoothing(void);

	// 固定撮影計画の出荷時設定部分を plan に書き込む。
	// name/place/camera/lens/interval/azimuth/elevation/landscape を設定する。
	// start/end と events/ccmList は呼び出し側が設定する。
	static void factoryFixedPlan(hgc::cs& plan);

	// --- 動作ログ(データ構造仕様書43 §8) ---
	// 固定長128Bのテキストレコードを日付ごとのファイル(hg_YYYY-MM-DD.log)へ追記する。
	// 保存は osFile 抽象を介す(M5=SD/LittleFS, Android=外部ファイル領域)。

	// ログのタイムスタンプに使う UTCオフセット[分]を設定する(撮影開始時などに呼ぶ)。
	static void setLogOffset(int utcOffsetMin);

	// 露出を伴わないイベント(START/STOP/CCMSW/NET/ERR/INFO)を記録する。
	// event: §8.3 のイベント種別(6文字以内)。detail: 補足(55文字以内)。
	static void logEvent(const char* event, const char* detail, bool error = false);

	// 1枚撮影(SHOT)を記録する。frame/iso/ss/fn/lum と適用中ccm名。
	static void logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName);

	// 現在の(本日の)ログファイルのフルパスを返す(検証・ログ転送用)。
	static std::string currentLogPath(void);
};

#endif // _DATA_MANAGER_H_
