#ifndef _API_BASE_H_
#define _API_BASE_H_
#include "common.h"
#include "device.h"
#include "hgcCommon.h"		// hgc::exposure(測光露出の申告に使う)
#include "exposureMath.h"	// expo::expoTables(APEX換算。カメラ非依存の数学)
#include <functional>
#include <string>

class apiBase
{
public:
	// 撮影の設定可能な値

public:
	apiBase(void) {};
	virtual ~apiBase(void) {};
	virtual errCode init(class device& device) = 0;
	virtual errCode startShooting(void) { return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode rdyShutter(const cmdt::shotSet& shotSet) { return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode actShutter(void)						{ return ERR_HGC_NOT_SUPPORTED; }
	// 露出を1項目ずつ設定する(周期正確化のタイマ方式で、変更のあった項目だけを適用するため)。
	virtual errCode setFNumber(const std::string& fNumber)	{ (void)fNumber; return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode setSS(const std::string& ss)			{ (void)ss;      return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode setIso(const std::string& iso)			{ (void)iso;     return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode getSettings(cmdt::shotRange& settings)	{ return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode rdyMetering(void)						{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode alzMetering(cmdt::HISTOGRAM& hist)		{ return ERR_HGC_NOT_SUPPORTED; };
	// 直近 alzMetering が解析したライブビューフレームの「カメラ側取得時刻」[ms]。0=不明。
	// 露光後に撮られた新鮮なフレームか、露光前の古いフレームかの判定に使う。
	virtual uint64_t lastLvTimeMs(void)						{ return 0; };
	// 撮影開始時にカメラを当アプリ都合(マニュアル露出)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	virtual errCode setupShootingModeManual(void)			{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode restoreShootingMode(void)				{ return ERR_HGC_NOT_SUPPORTED; };
	// 接続維持用の無害なGET。撮影窓まで待機中などに定期送出し、無通信でカメラの
	// Wi-Fi/CCAPIセッションがタイムアウト切断するのを防ぐ。return ERR_HGC_OK で到達。
	virtual errCode keepAlive(void)							{ return ERR_HGC_NOT_SUPPORTED; };

	// === 測光(場面のリニア輝度の取得) ===
	// 「測光してリニア輝度を得る」という機能はどのカメラでも同じで、**どう測るか**が
	// カメラに依存する(CCAPI=LVヒストグラム+暗所では測光ssへの一時切替、等)。そのため
	// 測り方の実装詳細はこの層(apiBaseの実装クラス)に閉じ、上位(captureRunner)は結果だけを使う。
	// 別方式(撮影画像サムネイル等)への差し替えもこの層の実装交換で行う(2026-07-27 構造見直し)。
	struct meterResult
	{
		bool          ok       = false;	// 測光値が得られたか(false=露出は据え置きを推奨)
		double        sceneRef = -1.0;	// 露出非依存の「場面の明るさ」(=linear/2^測光段)。投影・ev0の基礎
		double        linear   = -1.0;	// 測光露出で写るリニア輝度(中央値)
		double        x        = -1.0;	// ヒストグラム中央値(0..1 sRGB)
		hgc::exposure meterExp;			// 実際に測光した露出(ev0の逆算はこれを使う)
		// --- カメラ露出状態の申告(差分適用キャッシュとの整合用。露出を触ったら必ず申告する) ---
		std::string   appliedSs;		// この測光でカメラへ適用したss(空=カメラの露出に触れていない)
		bool          ssSwitchFailed = false;	// 測光ss切替を送ったが失敗(遅延適用の恐れ→呼び出し側はssを必ず再送)
		// --- 診断(ログ用) ---
		std::string   meterSsUsed;		// 切替に使った測光ss(空=撮影露出のまま測った)
		double        p99 = -1.0, pMax = -1.0;	// ヒストの明るい側(99%点/最大ビン)
		uint32_t      histSum  = 0;		// ヒスト内容チェックサム(前コマ一致=古いフレーム検出)
		uint64_t      lvTimeMs = 0;		// フレームのカメラ側取得時刻[ms]
		int           staleSkip = 0;	// 古いフレームを捨てた回数
		int           tries     = 0;	// 取得試行回数
		int           settleMs  = -1;	// ss切替→LV反映の待ち[ms](-1=切替なし)
		int           rdyMs     = -1;	// 測光全体の実測[ms](下の内訳の合計)
		// 内訳(2026-07-28 計測用): 遅くなっているのがカメラの記録待ちか通信かを切り分けるため。
		int           waitMs    = -1;	// 新しい画像の登録通知を待った時間[ms]
		int           fetchMs   = -1;	// サムネイル取得(HTTP GET)の時間[ms]
		int           decodeMs  = -1;	// JPEG復号+ヒストグラム計算の時間[ms]
		int           fetchTries = 0;	// サムネイル取得の試行回数(1=一発成功)
		bool          pinned    = false;	// 張り付き検出(測光値は信用しない)
		int           failStage = 0;	// 失敗した工程(0=成功/実装定義の段番号。ログで原因を特定するため)
		// 新規画像待ちの内訳(2026-07-30 診断用)。failStage=1(待ちで空)のとき、どの通信で
		// つまずいたかを残す。0=該当なし
		//  1=/contents が取れない  2=カード配下(ディレクトリ一覧)が取れない
		//  3=総数(kind=number)が取れない  4=時間内に総数が増えない  5=最新パス(kind=list)が取れない
		int           waitStep  = 0;
		int           waitHttp  = 0;	// その通信のHTTPステータス(0=応答なし/接続失敗)
		std::string   waitBody;			// 応答本文の先頭(CCAPIは理由をここに返す)
	};
	// 撮影露出 shotExp を基準に測光し場面輝度を返す(測光ssの選択・切替・適応は実装側の責務)。
	//  keepGoing: 中断判定(falseを返したら速やかに諦める)。
	virtual errCode meterScene(const hgc::exposure& shotExp, meterResult& out,
	                           const std::function<bool()>& keepGoing)
	{ (void)shotExp; (void)out; (void)keepGoing; return ERR_HGC_NOT_SUPPORTED; }
	// カメラの現在の露出のまま測る(初期収束などシャッター前の反復用。露出には触れない)。
	virtual errCode meterHere(meterResult& out, const std::function<bool()>& keepGoing)
	{ (void)out; (void)keepGoing; return ERR_HGC_NOT_SUPPORTED; }
	// 「いつ測光(meterScene)を呼んでほしいか」の申告(2026-07-27)。captureRunner はこの指示に従って
	// スケジュールする。値は実装(カメラ/方式)ごとに変えてよい:
	//  ・撮影画像フィードバック系: ソースは直前の撮影画像=呼び出し時刻に依存しない
	//    → afterShutterClose=true(露光が閉じ次第すぐ呼んでよい。最速)
	//  ・ライブビュー系: シャッターの一定時間前の輝度を見たい
	//    → afterShutterClose=false + leadMs(シャッターの何ms前に呼ぶか)
	struct meterTiming
	{
		bool afterShutterClose = false;	// true=露光終了直後に測光してよい
		int  leadMs            = 5000;	// false時: シャッターの何ms前に測光を開始するか
	};
	virtual meterTiming meterTimingHint(void) const { return meterTiming{}; }
	// 測光の適応状態(測光ssの学習・張り付き天井・フレーム鮮度基準)を捨てる(セッション確立/再接続時)。
	virtual void meterReset(void) {}

};

#endif // _API_BASE_H_
