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
// スマホから発見中のオンラインカメラ一覧(§6 cameraInfo)を受け取り、既知カメラテーブルを更新する。
// json = [{"serial","model","ip","online"}]。エッジ役の発見が IP直結の最優先ヒントに使う。
int32_t hge_setKnownCameras(const char* json, int32_t len);

// --- カメラ台帳(スマホ→エッジ。挨拶に要る最小限のカメラ情報) ---
//
// 【なぜ計画と別に要るか】エッジがカメラの資格情報を持てるのは撮影計画を受け取ったときだけだった。
//  ところがカメラに「繋がったよ」と知らせる挨拶(初回Wi-Fi参加を成立させる 200)は、計画を作る
//  **前**に要る。資格情報が無いまま投げると素の 401 にしかならず、繰り返すとカメラが 403 で
//  締め出す(2026-08-28 に実機で1台やってしまった)。だから計画とは無関係に台帳を預ける。
//
// 【全量で置き換える】送られてきた配列がその時点の全量。エッジは持っている台帳を捨てて入れ替える。
//  カメラを消した・作り直した・パスワードを変えた、がこの1本で伝わる。消すためのコマンドは要らない。

// スマホ役: いま所持しているカメラから台帳 JSON を作る。パスワードは暗号文で入る。
//  エッジ役では空配列。内容が変わったかどうかの判定にもこの文字列を使ってよい。
const char* hge_cameraBookJson(void);

// 台帳の**中身の指紋**。暗号化する前の値から作るので、同じ内容なら必ず同じになる。
//  【なぜ要るか(2026-08-29 実機で判明)】secret::encrypt は毎回ちがう nonce を使うため、
//   同じパスワードでも暗号文が変わる。台帳 JSON をそのまま比べると毎回「変わった」と
//   見えてしまい、30秒ごとに送信・保存・候補の作り直しが走っていた。比べるのはここ。
const char* hge_cameraBookSig(void);

// エッジ役: 台帳を受け取り、保存して認証候補を入れ替える。json = 台帳 JSON。
//  台帳から消えたカメラの資格情報は落とすが、手元にまだ計画が残っている分は足し直す
//  (撮影中に認証できなくなるのを避ける)。
int32_t hge_setCameraBook(const char* json, int32_t len);

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

// 同期撮影(2026-08-25)。camera で測光した露出を追加カメラへも配り全台で撮る。
int32_t hge_setPlanSyncShot(int32_t on);
// 追加カメラを所持カメラの名前配列 ["name",...] で差し替える。
int32_t hge_setPlanSubCameras(const char* namesJson);

// この端末がAPモードで動いているかを Entity へ伝える(エッジのUI層が起動時に呼ぶ)。
//  APモードは SoftAP と DHCP が内部RAMを食うため、扱えるカメラ台数が少ない。
//  スマホ役は呼ばなくてよい(既定=false=STA相当の上限)。
int32_t hge_setApMode(int32_t ap);

// 機材マスタ(/master のカメラ/レンズ一覧)を読み直す。
//  公開リポジトリから新しいものを取り込んだ直後に呼ぶ。次に一覧を求められたときに読み直す。
int32_t hge_reloadMaster(void);

// この端末が同期撮影で扱えるカメラ台数(主カメラを含む合計)。
int32_t hge_maxSyncShotCameras(void);

// 直前の hge_captureStartPlan が失敗した理由。成功していれば code=0。
//  code = hgc::notice の値、n1 = その付随数値。UI へ理由を返すために使う。
int32_t hge_lastStartNotice(int32_t* code, int32_t* n1);


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

// 指定 id の撮影計画(cs)を自己完結 JSON で取得(バッファ規約)。
// hge_getPlanJson との違いは「編集対象(=画面が表示している計画)を動かさない」こと。
// エッジ端末へ別の計画を送るときに、いちいち編集対象を切り替えずに済ませるために使う。
int32_t hge_getPlanJsonById(const char* id, char* buf, int32_t* inoutLen);

// --- 撮影シミュレーション(スマホ専用・画面360)。実装 30_role/10_phone/src/skySim.cpp ---
// 恒星リスト(fixed_star.json 配列 [{name,ra,dec,mag,color_code}])を一度読み込む。戻り=星数,負=エラー。
int32_t hge_simLoadStars(const char* starsJson);
// params から画角内の天体を投影して返す(バッファ規約)。
//  params = {datetime:"YYYY/MM/DD HH:MM:SS",offMin,lat,lon,alt,az,el,landscape,fisheye,focal,sensorW,sensorH,magLimit}
//  返り   = {"objects":[{name,x,y,mag,color,kind}],"aspect","landscape","fisheye"}  x,y∈[-1,1](x右+ y上+)
int32_t hge_simulateSky(const char* paramsJson, char* buf, int32_t* inoutLen);

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
// 初期値(プリセット)のエディタ用。カメラに依らない標準 1/3 段(iso/ss)と F1.0〜32 の目盛り。
int32_t hge_getStandardExpoValuesJson(char* buf, int32_t* inoutLen);

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
// 所持カメラのダイジェスト認証パスワードを平文で取得(バッファ規約)。編集画面の表示用。
//  JSON には暗号文しか載せないので、UI へ中身を見せるにはこの経路を使う。
int32_t hge_ownedCameraAuthPassJson(const char* name, char* buf, int32_t* inoutLen);
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
// 撮影場所(緯度経度)を設定しスケジュールを再生成する。name は地名(空可)。標高は変更しない。
int32_t hge_setPlanLocation(double latitude, double longitude, const char* name);

// 所持カメラ/レンズの詳細(全項目)を JSON で更新/新規作成して保存する(620/630 詳細編集)。
//  origName 一致を置換、無ければ新規追加。json キーは dataManager の同名関数を参照。
int32_t hge_setOwnedCameraDetail(const char* origName, const char* json);
int32_t hge_setOwnedLensDetail(const char* origName, const char* json);

// --- 撮影場所(§7.9)。登録した場所を撮影計画で選択する。 ---
int32_t hge_getPlacesJson(char* buf, int32_t* inoutLen);   // 登録済み場所の配列 JSON
int32_t hge_addPlace(const char* name);                    // 空可(自動採番)

// --- 撮影計画ひな形(2026-09-04 UI依頼)。計画と同じ形で tpl_<id>.json に置く ---
int32_t hge_listTemplatesJson(char* buf, int32_t* inoutLen); // 名前順
int32_t hge_selectTemplate(const char* id);                  // ひな形を編集対象にする
int32_t hge_saveTemplateFromPlan(const char* name);          // 今の計画をひな形へ(空=計画名)
int32_t hge_copyTemplate(const char* id);
int32_t hge_deleteTemplate(const char* id);
int32_t hge_renameTemplate(const char* id, const char* name);
int32_t hge_newPlanFromTemplate(const char* id);             // 開始日=今日 / 名前は連番回避
int32_t hge_updatePlanFromTemplate(const char* planId, const char* tplId); // 名前・時刻は据え置き

// 与えた撮影計画(JSON)をひな形として保存する。**同じ名前のひな形が既にあれば何もしない**。
//  端末ごとに中身が変わるひな形(スマホ内蔵カメラ用など)を、役割側から作るための口。
//  一度作った後に利用者が消したものを、起動のたびに作り直さないための「あれば何もしない」。
int32_t hge_saveTemplateJsonIfAbsent(const char* csJson);
int32_t hge_removePlace(const char* name);
int32_t hge_setPlaceAutoInsert(const char* name, int32_t autoInsert);
// 場所詳細(name/memo/latitude/longitude/altitude/autoInsert)を JSON で更新/新規作成。origName 一致を置換。
int32_t hge_setPlaceDetail(const char* origName, const char* json);
// 登録済みの撮影場所を名称で撮影計画へ反映し、スケジュールを再生成して通知する。
int32_t hge_setPlanPlace(const char* name);

// システム共通の色(全体設定)。型ごとの文字色/背景色を JSON で取得/設定する。
//  {"night":{"text":int,"bg":int},...}  型: night/sunrise/sunset/day/moon/preNight/postNight
int32_t hge_getColorsJson(char* buf, int32_t* inoutLen);
int32_t hge_setColorsJson(const char* json);

// 全体設定の露出平滑化(ヒステリシス/移動平均)。{"hysteresis":double,"movingAverage":int}
int32_t hge_getSmoothingJson(char* buf, int32_t* inoutLen);
int32_t hge_setSmoothingJson(const char* json);

// 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。offMin=ローカルTZ[分]。
int32_t hge_pruneOldLogs(int32_t offMin);

// --- 撮影レポート(撮影1回=1件。設定→「撮影レポート」画面で表示する) ---
// 一覧(新しい順): [{"name","plan","camera","shotAt","frames","noteCount"},...]
int32_t hge_reportListJson(char* buf, int32_t* inoutLen);
// 溜まっているレポートの件数だけを返す(中身は読まない)。エッジ役が検索応答へ載せ、スマホの
// 常時スイープが「取りに行く必要があるか」をこれ1つで判断する。負値=エラー。
int32_t hge_reportCount(void);
// 1件の中身(保存した JSON をそのまま)。name は一覧の "name"(ファイル名)。
int32_t hge_reportJson(const char* name, char* buf, int32_t* inoutLen);
// 1件削除する。
int32_t hge_removeReport(const char* name);

// 撮影制御方法の初期値プリセット(型ごとに複数)。型: night/sunrise/sunset/day/moon。
int32_t hge_getCcmPresetsJson(const char* type, char* buf, int32_t* inoutLen);  // 型のプリセット配列
int32_t hge_setCcmPreset(const char* type, const char* origName, const char* json); // 追加/更新
int32_t hge_removeCcmPreset(const char* type, const char* name);
int32_t hge_getPreferredCcm(const char* type, char* buf, int32_t* inoutLen);    // 優先初期値の名前
int32_t hge_setPreferredCcm(const char* type, const char* name);

// 接続カメラ検索(同期)。検出した全カメラの一覧 JSON を返す(バッファ規約)。
//  [{"model","assignedName","serial"},...]。続けて hge_addOwnedDetected で所持へ追加する。
int32_t hge_searchDevicesListJson(char* buf, int32_t* inoutLen);
// 直近の検索で見つかったカメラ(index)を所持カメラへ追加/更新する。
int32_t hge_addOwnedDetected(int32_t index);

// 発見/接続したカメラ識別情報(model/serial/assignedName)を所持カメラへ反映する。
//  allowAdd=1: 未一致(新規)は自動追加(撮影接続/明示登録)。allowAdd=0: 追加せず ISNEW を返す(裏の発見→UIが登録可否を問う)。
//  返り値: >=0 は区分(0=既存にassignedName反映/1=未定義枠へserial確定/2=新規)、<0 はエラー。
int32_t hge_recordCameraIdentity(const char* model, const char* serial, const char* assignedName, int32_t allowAdd);

// 現在の進捗スナップショットを JSON で取得(バッファ規約)。
//  {"state","frame","total","remainSec","elapsedSec","ccm","iso","ss","fn"}
//  エッジ端末が progress(get) 応答に使う。
int32_t hge_getProgressJson(char* buf, int32_t* inoutLen);

// 計画id指定の進捗JSON(1エッジ複数カメラ対応)。planId 空/NULL は上の集約版へフォールバック。
// planId 指定時は該当セッションの状態/進捗を返し、該当セッションが無ければ state=IDLE を返す。
// エッジ端末が C_PROGRESS(data=planId) 応答に使う。計画別に状態を返すことで誤NOCAMERAポップを防ぐ。
int32_t hge_getProgressJsonFor(const char* planId, char* buf, int32_t* inoutLen);

// 実行中セッションの一覧 JSON 配列(バッファ規約)。[{"id":"<planId>","state":N},...] (最大8件)
// エッジ端末が C_SEARCH 応答(edgeInfo)の "sessions" に載せる。スマホの常時スイープが
// エッジ側ローカル開始/自動再開を検出して表示へ反映するために使う。
int32_t hge_getSessionsJson(char* buf, int32_t* inoutLen);

// 項目6: エッジが保有する全撮影計画の id 一覧 JSON 配列 ["id1","id2",...](最大32件)。
// エッジ端末が C_SEARCH 応答(edgeInfo)の "heldPlans" に載せる。スマホはこれと自分の
// エッジ担当割り当てを突き合わせ、エッジ側で削除された計画のロックを解除する。
int32_t hge_getHeldPlansJson(char* buf, int32_t* inoutLen);

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
// 遅延アームのポンプ(§7.4)。予約(将来窓)セッションの開始スレッドを期日(窓start-90秒)に生成する。
// 生成失敗(内部RAM断片化)も10秒間隔で再試行する。エッジ=メインloopから毎秒、スマホ=UIタイマから数秒毎に呼ぶ。
int32_t hge_pump(void);

// 項目1: いずれかのセッションがカメラI/O中(取得/撮影/停止処理)か。エッジの在否モニタが撮影中は探索を
// 止めるための判定に使う(時間厳守のシャッターI/Oと単一netThreadで競合させない)。
bool hge_anyActiveCameraSession(void);
// このシリアルの個体を今撮影に使っているか(在否監視が「触らない判定」に使う)。
bool hge_isCameraInUse(const char* serial);

// バッテリ切れによる自動シャットダウンの直前に呼ぶ。全セッションを NOCAMERA(✖)にして
// スマホのポーリングが1回拾えるようにする。撮影の意図(capturing.json)は残すので、
// 充電して電源を入れ直せば hge_resumeCapture で再開される。
void hge_markAllNoCameraForShutdown(void);

#ifdef __cplusplus
}
#endif

#endif // _HOLY_GRAIL_ENTITY_H_
