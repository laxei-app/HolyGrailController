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
#include <vector>

class device;	// 接続時のシリアル/フレンドリ自動保存に使う

class dataManager
{
public:
	// 出荷時設定の撮影制御方法一式(夜間/朝日/夕日/日中)。
	static astro::ccmSet factoryCcmSet(void);

	// 出荷時設定の月の影響への対処(データ構造仕様書43 §3.6)。
	static std::shared_ptr<hgc::ccmMoon> factoryMoon(void);

	// --- 撮影制御方法の初期値(ユーザー資産。/asset/ccmDefaults.json。仕様書43 §7.6) ---
	// 現在の初期値(ファイルがあればそれ、無ければ出荷時設定)。スケジュール生成に使う。
	static astro::ccmSet currentCcmSet(void);
	// 現在の初期値を JSON 文字列で取得(編集画面表示用)。
	static std::string ccmDefaultsJson(void);
	// 初期値を JSON から更新し /asset/ccmDefaults.json へ保存する。return: 成功。
	static bool setCcmDefaultsJson(const std::string& json);

	// 出荷時設定の露出平滑化(ヒステリシス1段, 移動平均5フレーム)。
	static hgc::exposureSmoothing factorySmoothing(void);

	// 固定撮影計画の出荷時設定部分を plan に書き込む。
	// name/place/camera/lens/interval/azimuth/elevation/landscape を設定する。
	// start/end と events/ccmList は呼び出し側が設定する。
	static void factoryFixedPlan(hgc::cs& plan);

	// ========================================================================
	//  機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6)
	// ========================================================================
	// --- 機材マスタ(読取専用。/master/cameras.json・lenses.json。インストール同梱) ---
	// UI の「追加リスト」表示用 JSON(配列)。読込失敗時は出荷時フォールバック。
	static std::string masterCamerasJson(void);
	static std::string masterLensesJson(void);

	// --- 所持機材(ユーザー資産。/asset/ownedCameras.json・ownedLenses.json) ---
	static std::string ownedCamerasJson(void);
	static std::string ownedLensesJson(void);
	// マスタ(名称一致)から所持へ追加して保存する。return: 成功(追加 or 既存)。
	static bool addOwnedCameraFromMaster(const std::string& name);
	static bool addOwnedLensFromMaster(const std::string& name);
	// 所持から削除して保存する。return: 成功(削除した)。
	static bool removeOwnedCamera(const std::string& name);
	static bool removeOwnedLens(const std::string& name);
	// 所持カメラの撮影計画への自動挿入フラグを設定して保存する。
	static bool setOwnedCameraAutoInsert(const std::string& name, bool autoInsert);

	// 所持カメラの詳細(全項目)を JSON で更新/新規作成して保存する。origName 一致を置換、
	// 無ければ新規追加(マスタに無い手動カメラ)。json キー:
	//  maker/model/name/friendly/serial/sensorSize/sensorSizeV/sensorPixel/
	//  isoMin/isoMax/ssMin/ssMax(設定可能範囲。変更時は標準1/3段で再生成)/autoInsert/lensNames[]
	static bool setOwnedCameraDetailJson(const std::string& origName, const std::string& json);

	// 所持レンズの詳細を JSON で更新/新規作成して保存する。json キー:
	//  maker/name/focalLength/fn(F最小=開放)/fnMax(F最大)/hasContact
	static bool setOwnedLensDetailJson(const std::string& origName, const std::string& json);

	// --- システム共通の色(全体設定。/asset/settings.json の "colors") ---
	// 撮影制御方法ごとの文字色/背景色。型キー: night/sunrise/sunset/day/moon/preNight/postNight。
	// {"night":{"text":int,"bg":int},...}。未設定は出荷時の既定色で補完する。
	static std::string colorsJson(void);
	static bool        setColorsJson(const std::string& json);

	// --- 撮影計画への機材選択(所持から g_plan へ反映するのは UI/holyGrailEntity 側) ---
	// 所持カメラ/レンズを名称で引く。見つからなければ false。
	static bool findOwnedCamera(const std::string& name, hgc::camera& out);
	static bool findOwnedLens(const std::string& name, hgc::lens& out);

	// --- 接続時のシリアル/フレンドリ自動保存(§5.2拡張) ---
	// モデル一致の所持カメラへ serial/friendly を保存。一致が無ければ
	// master(model)＋device から所持カメラを自動作成して保存(1台運用で無設定OK)。
	static bool recordConnectedCamera(const device& dev);

	// --- 撮影計画の永続化(案A /plan、当面は単一ファイル plan.json) ---
	// 撮影計画(cs)の自己完結JSONを /plan/plan.json へ保存する。
	static bool savePlanJson(const std::string& json);
	// 保存済み撮影計画JSONを読み込む。無ければ false。
	static bool loadPlanJson(std::string& out);

	// 任意の ccmSet+moon を JSON 化/復元する(初期値ccmと計画固有ccmで共用)。
	static std::string ccmSetToJson(const astro::ccmSet& set, const std::shared_ptr<hgc::ccmMoon>& moon);
	static bool parseCcmSetJson(const std::string& json, astro::ccmSet& set, std::shared_ptr<hgc::ccmMoon>& moon);

	// 保存ラッパー {"plan":..,"planCcm":..} を分解する。plan必須、planCcmは任意(空文字)。
	// 旧形式(素のcs JSON)もそのまま plan として返す。
	static bool splitSavedPlan(const std::string& wrapped, std::string& planOut, std::string& ccmOut);

	// --- 動作ログ(データ構造仕様書43 §8) ---
	// 固定長128Bのテキストレコードを日付ごとのファイル(hg_YYYY-MM-DD.log)へ追記する。
	// 保存は osFile 抽象を介す(M5=SD/LittleFS, Android=外部ファイル領域)。

	// ログのタイムスタンプに使う UTCオフセット[分]を設定する(撮影開始時などに呼ぶ)。
	static void setLogOffset(int utcOffsetMin);

	// 露出を伴わないイベント(START/STOP/CCMSW/NET/ERR/INFO)を記録する。
	// event: §8.3 のイベント種別(6文字以内)。detail: 補足(55文字以内)。
	static void logEvent(const char* event, const char* detail, bool error = false);

	// 1枚撮影(SHOT)を記録する。frame/iso/ss/fn/lum と適用中ccm名。
	// meteredLinear: 測光したリニア輝度(自動補正時のみ。<0=測光なしで detail に出力しない)。
	static void logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName,
	                    double meteredLinear = -1.0);

	// 現在の(本日の)ログファイルのフルパスを返す(検証・ログ転送用)。
	static std::string currentLogPath(void);
};

#endif // _DATA_MANAGER_H_
