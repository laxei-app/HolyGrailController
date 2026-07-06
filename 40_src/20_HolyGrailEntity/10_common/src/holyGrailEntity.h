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
	HGE_EV_ERROR    = 6,	// エラー         {"code":int,"msg":string}
	HGE_EV_PRESENCE = 7		// プレゼンス変化 [{"serial","model","ip","online"}](スマホ常駐マップ)
};

// 撮影状態 (47 §2.3)
enum hgeState
{
	HGE_ST_IDLE      = 0,
	HGE_ST_SEARCHING = 1,
	HGE_ST_READY     = 2,
	HGE_ST_CAPTURING = 3,
	HGE_ST_STOPPING  = 4,
	HGE_ST_ERROR     = 5,
	HGE_ST_DISCONNECTED = 6,	// 撮影中にカメラ接続が切れた(再接続試行中/接続不可)。NOCAMERAの旧同義。UIは✖点灯
	HGE_ST_WAITING   = 7,	// 撮影要求済・撮影窓前・カメラOKで待機中。UIはカメラアイコン点灯(点滅しない)
	HGE_ST_NOCAMERA  = 8	// 武装/撮影中いずれでもカメラ未検出。UIは✖カメラアイコン点灯。1分ごと再検索+SSDP待受で探索継続
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

// --- デバイス接続 ---
// カメラを自動検索する(SSDP。非同期)。結果は HGE_EV_DEVICE で通知し、
// 見つかれば状態は READY になる。以降の hge_captureStart は検索を省略してこのカメラを使う。
// 実機ではこちらが既定。見つからなければ HGE_EV_ERROR を通知し IDLE に戻る。
int32_t hge_searchDevices(void);

// IP直指定でカメラに接続する(SSDP不使用。エミュレータ等での手動接続用)。
// 成功すると以降の hge_captureStart は検索を省略してこのカメラを使う。
// host : カメラのIPアドレス(例 "192.168.1.4")。
int32_t hge_connectManual(const char* host);

// スマホから発見中のオンラインカメラ一覧(§6 cameraInfo)を受け取り、既知カメラテーブルを更新する。
// json = [{"serial","model","ip","online"}]。エッジ役の発見が IP直結の最優先ヒントに使う。
int32_t hge_setKnownCameras(const char* json, int32_t len);

// --- スマホ役: 常駐プレゼンスマップ(§3.2/§5.4。スマホ役のみ実体、エッジ役は no-op) ---
// 起動時M-SEARCH+NOTIFY+定期疎通でオンラインカメラを常時把握。変化のたび HGE_EV_PRESENCE を通知する
// (UIが最新IPを撮影中/待機中のエッジへプッシュ)。MulticastLock は Android UI 側で保持すること。
int32_t hge_presenceStart(void);
int32_t hge_presenceStop(void);
int32_t hge_presenceJson(char* buf, int32_t* inoutLen);	// [{"serial","model","ip","online"}]

// --- 撮影計画(MVP は固定データ) ---
int32_t hge_loadFixedPlan(void);	// 固定データの撮影計画を生成し保持する

// 時刻のUTCオフセット[分]を設定する(エッジ端末がtimeコマンド受信時に呼ぶ)。
// 受信した撮影計画(cs)のローカル時刻を Unix時刻へ変換する基準になる。
int32_t hge_setUtcOffset(int32_t offMin);

// 受信した撮影計画(cs)を JSON から投入する(エッジ端末がcapturePlan受信時に呼ぶ)。
// cs は events/ccmList を含む自己完結形式(データ構造仕様書43 §4.5)。
int32_t hge_setPlanJson(const char* json, int32_t len);

// 受信した計画を指定 id の計画ファイルとして取り込む(エッジが複数計画を蓄積)。id 空なら新規採番。
int32_t hge_importPlan(const char* id, const char* json, int32_t len);

// 撮影開始/終了時刻を設定し、スケジュール(events/ccmList)を再生成する。
// start/end は "YYYY-MM-DDThh:mm:ss"(ローカル)、offMin はUTCオフセット[分]。
// 出荷時設定の撮影制御方法・場所・機材を用いて astro::buildSchedule で自動生成し、
// HGE_EV_SCHEDULE で通知する。
int32_t hge_setPlanTimes(const char* startIso, const char* endIso, int32_t offMin);

// 開始時の撮影方向(方位[°] 0=北,90=東)と仰角[°]を設定し、スケジュールを再生成する。
// 撮影方向で「太陽が画角に入る時刻」が変わるため、朝日/夕日の区間が更新される。
// 結果は HGE_EV_SCHEDULE で通知する。
int32_t hge_setPlanDirection(double azimuth, double elevation);

// 撮影周期[秒]を設定する。最小撮影周期(全ccmの最長ss+2)未満は ERR_HGC_INVALID_ARG(UIで警告)。
int32_t hge_setPlanInterval(double seconds);

// 撮影計画(id指定)の名称を変更する(リストで直接リネーム)。
int32_t hge_renamePlan(const char* id, const char* name);

// 横向き(ランドスケープ)を設定する。画角が変わるためスケジュールを再生成し通知する。
int32_t hge_setPlanLandscape(int32_t landscape);

// 計画のセンサー/レンズ定数を上書きする(機材リストに無い値の参考用。仕様 7.3 §センサーレンズ定数)。
// JSONキー: sensorW/sensorH/pixelW/focalLength/fn。スケジュールを再生成して通知する。
int32_t hge_setPlanGearConstJson(const char* json);

// --- スケジュール手動編集(7.3.2) ---
// 朝日/夕日の帯モード(0=自動判定, 1=挿入(強制), 2=排除(日中))。再生成して通知する。
int32_t hge_setBandMode(int32_t sunriseMode, int32_t sunsetMode);
// 境目の時刻上書きを追加/置換する。before/after=種別(ccmType), occ=同型ペアの出現順,
// whenIso="YYYY-MM-DDThh:mm:ss"(ローカル)。再生成して通知する。
int32_t hge_setBoundary(int32_t beforeType, int32_t afterType, int32_t occ, const char* whenIso);
// 境目を太陽高度で指定して上書きする(高度軸スケジュールUIの2本指ドラッグ用)。
// altDeg=太陽高度[°], rising!=0 で朝方(上昇)側、=0 で夕方(下降)側の到達時刻を使う。
int32_t hge_setBoundaryByAlt(int32_t beforeType, int32_t afterType, int32_t occ, double altDeg, int32_t rising);
// スケジュール手動編集をすべて解除する(帯モード=自動, 境目上書き消去)。再生成して通知する。
int32_t hge_clearScheduleEdits(void);

// 撮影計画のスケジュールを JSON で取得(バッファ規約)。
//  buf が null か容量不足なら必要バイト数を *inoutLen に格納し ERR_HGC_BUF_SHORT。
int32_t hge_getScheduleJson(char* buf, int32_t* inoutLen);

// 現在の撮影計画(cs)を自己完結 JSON で取得(バッファ規約)。
// スマホがエッジ端末へ capturePlan 転送する際に使う(csjson::toJson)。
int32_t hge_getPlanJson(char* buf, int32_t* inoutLen);

// 現在(編集対象)の撮影計画を永続化する(/plan/plan_<id>.json。1計画1ファイル §7.4)。
// id 未割当なら作成時刻から採番する。計画固有ccmも一緒に保存する。
int32_t hge_savePlan(void);

// --- 複数撮影計画(§7.4) ---
// 保存済み撮影計画の一覧を JSON 配列で取得(バッファ規約)。
//  [{"id","name","start","end","capturable":bool,"state":int},...]
//  capturable=撮影終了が現在より未来。state=その計画の撮影状態(hgeState)。
int32_t hge_listPlansJson(char* buf, int32_t* inoutLen);

// 新規撮影計画を作成する(presetName で初期化。空/未知は出荷時設定)。
// 採番→保存→編集対象に切替し、HGE_EV_SCHEDULE を通知する。作成した id は
// hge_getCurrentPlanId で取得できる。
int32_t hge_newPlan(const char* presetName);

// 既存計画(id)を複製して新規作成する(名前に「 コピー」を付す)。編集対象に切替。
int32_t hge_copyPlan(const char* id);

// 撮影計画(id)を削除する。編集対象だった場合は別計画(無ければ新規出荷時)へ切替。
int32_t hge_deletePlan(const char* id);

// 撮影計画(id)を編集対象として読み込む。HGE_EV_SCHEDULE を通知する。
int32_t hge_selectPlan(const char* id);

// 現在(編集対象)の撮影計画 id を取得(バッファ規約)。
int32_t hge_getCurrentPlanId(char* buf, int32_t* inoutLen);

// 計画固有の撮影制御方法(初期値ccmとは別)の取得/設定。形式は ccmDefaults と同じ。
// setPlanCcm はスケジュールを再生成して HGE_EV_SCHEDULE で通知する。
int32_t hge_getPlanCcmJson(char* buf, int32_t* inoutLen);
int32_t hge_setPlanCcmJson(const char* json, int32_t len);

// --- 撮影制御方法の初期値(参照専用。コード上の出荷時設定。データ構造仕様書43 §7.6) ---
// 初期値(夜間/朝日/夕日/日中)を JSON で取得(バッファ規約)。ファイルは持たない。
int32_t hge_getCcmDefaultsJson(char* buf, int32_t* inoutLen);

// 露出編集用の設定可能値(文字列配列)を取得する(バッファ規約)。
//  {"iso":["100",...],"ss":["1/8000",...,"30"],"fn":["1.4",...,"32"]}
//  iso/ss は標準1/3段、fn は計画のレンズf範囲。スライダーの選択肢に使う。
int32_t hge_getExpoValuesJson(char* buf, int32_t* inoutLen);

// 接続中カメラが実際に受け付ける設定値の一覧(CCAPIのability)を取得する(バッファ規約)。
//  {"iso":[...],"ss":[...],"fn":[...]}。露出文字列フォーマットの実機検証に使う。
//  カメラ未接続なら検索を試み、見つからなければ ERR_HGC_NOT_FOUND。
int32_t hge_getCameraAbilityJson(char* buf, int32_t* inoutLen);

// 固定露出太陽高度の start(日没側で指定高度に達する時刻)/end(日の出側) を計算する(バッファ規約)。
// altitudeDeg: 太陽高度[°]。現在の撮影計画の日付・場所で計算する。
//  {"start":"MM/dd HH:mm","end":"MM/dd HH:mm"}(見つからなければ "--:--")
int32_t hge_sunAltitudeTimes(int32_t altitudeDeg, char* buf, int32_t* inoutLen);

// --- 機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6) ---
// 機材マスタ(読取専用。インストール同梱)を JSON 配列で取得(バッファ規約)。
//  cameras: [{"camera":{...}},...]  lenses: [{...},...]。UI の「追加リスト」表示用。
int32_t hge_getMasterCamerasJson(char* buf, int32_t* inoutLen);
int32_t hge_getMasterLensesJson(char* buf, int32_t* inoutLen);
// 所持機材(ユーザー資産)を JSON 配列で取得(バッファ規約)。
int32_t hge_getOwnedCamerasJson(char* buf, int32_t* inoutLen);
int32_t hge_getOwnedLensesJson(char* buf, int32_t* inoutLen);
// マスタ(名称一致)から所持へ追加して保存する。
int32_t hge_addOwnedCamera(const char* name);
int32_t hge_addOwnedLens(const char* name);
// 所持から削除して保存する。
int32_t hge_removeOwnedCamera(const char* name);
int32_t hge_removeOwnedLens(const char* name);
// 所持カメラの撮影計画への自動挿入フラグを設定して保存する(0/1)。
int32_t hge_setOwnedCameraAutoInsert(const char* name, int32_t autoInsert);
// 所持機材を撮影計画(cs)へ反映し、スケジュールを再生成して通知する。
int32_t hge_setPlanCamera(const char* name);
int32_t hge_setPlanLens(const char* name);

// 所持カメラ/レンズの詳細(全項目)を JSON で更新/新規作成して保存する(620/630 詳細編集)。
//  origName 一致を置換、無ければ新規追加。json キーは dataManager の同名関数を参照。
int32_t hge_setOwnedCameraDetail(const char* origName, const char* json);
int32_t hge_setOwnedLensDetail(const char* origName, const char* json);

// システム共通の色(全体設定)。型ごとの文字色/背景色を JSON で取得/設定する。
//  {"night":{"text":int,"bg":int},...}  型: night/sunrise/sunset/day/moon/preNight/postNight
int32_t hge_getColorsJson(char* buf, int32_t* inoutLen);
int32_t hge_setColorsJson(const char* json);

// 全体設定の露出平滑化(ヒステリシス/移動平均)。{"hysteresis":double,"movingAverage":int}
int32_t hge_getSmoothingJson(char* buf, int32_t* inoutLen);
int32_t hge_setSmoothingJson(const char* json);

// 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。offMin=ローカルTZ[分]。
int32_t hge_pruneOldLogs(int32_t offMin);

// 撮影制御方法の初期値プリセット(型ごとに複数)。型: night/sunrise/sunset/day/moon。
int32_t hge_getCcmPresetsJson(const char* type, char* buf, int32_t* inoutLen);  // 型のプリセット配列
int32_t hge_setCcmPreset(const char* type, const char* origName, const char* json); // 追加/更新
int32_t hge_removeCcmPreset(const char* type, const char* name);
int32_t hge_getPreferredCcm(const char* type, char* buf, int32_t* inoutLen);    // 優先初期値の名前
int32_t hge_setPreferredCcm(const char* type, const char* name);

// 接続カメラ検索(同期)。検出した全カメラの一覧 JSON を返す(バッファ規約)。
//  [{"model","friendly","serial"},...]。続けて hge_addOwnedDetected で所持へ追加する。
int32_t hge_searchDevicesListJson(char* buf, int32_t* inoutLen);
// 直近の検索で見つかったカメラ(index)を所持カメラへ追加/更新する。
int32_t hge_addOwnedDetected(int32_t index);

// 現在の進捗スナップショットを JSON で取得(バッファ規約)。
//  {"state","frame","total","remainSec","elapsedSec","ccm","iso","ss","fn"}
//  エッジ端末が progress(get) 応答に使う。
int32_t hge_getProgressJson(char* buf, int32_t* inoutLen);

// --- 撮影実行 ---
int32_t hge_captureStart(void);		// 撮影開始(非同期。編集対象計画)。進捗は HGE_EV_PROGRESS
int32_t hge_captureStop(void);		// 撮影停止(編集対象計画)
int32_t hge_getState(void);			// 集約状態(hgeState)を即時返す

// --- 並行撮影(Phase3。計画id指定。planId 空文字=編集対象計画) ---
// 通知(HGE_EV_STATE/PROGRESS/CAPTURED)には "planId" が付与される。同時実行は上限あり。
int32_t hge_captureStartPlan(const char* planId);
int32_t hge_captureStopPlan(const char* planId);
int32_t hge_getStatePlan(const char* planId);	// 指定計画の状態。planId 空=集約状態
// カメラ未検出(NOCAMERA)で待機中の計画に即再探索を促す(継続ボタン/SSDP出現)。planId 空=全て。
// 取得フェーズの60秒待ちを前倒しするだけで、見つからなければ引き続き NOCAMERA を維持する。
int32_t hge_pokeAcquire(const char* planId);
// 電源復帰/アプリ再起動時の撮影再開(item2)。/asset/capturing.json を読み、撮影中だった
// 計画を再開する。return: 再開を試みた計画数。エッジは起動時、スマホはアプリ起動時に呼ぶ。
int32_t hge_resumeCapture(void);

#ifdef __cplusplus
}
#endif

#endif // _HOLY_GRAIL_ENTITY_H_
