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

class device;	// 接続時のシリアル/設定名の自動保存に使う

class dataManager
{
public:
	// 出荷時設定の撮影制御方法一式(夜間/朝日/夕日/日中)。
	static astro::ccmSet factoryCcmSet(void);

	// 出荷時設定の月の影響への対処(データ構造仕様書43 §3.6)。

	// --- 撮影制御方法の初期値(参照専用。コード上の出荷時設定。仕様書43 §7.6) ---
	// 初期値の撮影制御方法一式(=出荷時設定)。スケジュール生成・プリセット種まきに使う。
	static astro::ccmSet currentCcmSet(void);
	// 初期値を JSON 文字列で取得(プリセット種まき・計画ccm初期化用)。
	static std::string ccmDefaultsJson(void);

	// 出荷時設定の露出平滑化(ヒステリシス1段, 移動平均5フレーム)。
	static hgc::exposureSmoothing factorySmoothing(void);

	// --- 全体設定の露出平滑化(/asset/settings.json の "smoothing") ---
	// 現在の全体平滑化(ファイルがあればそれ、無ければ出荷時)。撮影制御方法で個別未設定時に使う。
	static hgc::exposureSmoothing currentSmoothing(void);
	static std::string smoothingJson(void);              // {"hysteresis":double,"movingAverage":int}
	static bool        setSmoothingJson(const std::string& json);

	// 起動時のログ整理: 当日以外のログが5件以上なら古い順に削除し、当日以外を最新4件まで残す。
	// offMin: ローカル日付判定用 UTCオフセット[分]。return: 削除した件数。
	static int pruneOldLogs(int offMin);

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
	// 一覧を読み直す(公開リポジトリから新しいものを取り込んだ後に呼ぶ)。
	//  次に一覧を求められたときに /master から読み直す。
	static void reloadMaster(void);

	// --- 所持機材(ユーザー資産。/asset/ownedCameras.json・ownedLenses.json) ---
	static std::string ownedCamerasJson(void);
	static std::string ownedLensesJson(void);
	// マスタ(名称一致)から所持へ追加して保存する。return: 成功(追加 or 既存)。
	static bool addOwnedCameraFromMaster(const std::string& name);
	static bool addOwnedLensFromMaster(const std::string& name);
	// 所持から削除して保存する。return: 成功(削除した)。
	static bool removeOwnedCamera(const std::string& name);
	static bool removeOwnedLens(const std::string& name);
	// 所持カメラを読み込むだけ(起動時に1回)。読み込むと、その資格情報が
	//  ダイジェスト認証の候補として登録される(csjson::cameraFromJson)。カメラを探す前に
	//  済ませておかないと、最初の 401 に対応できず発見に1回失敗する。
	static void preloadOwned(void);

	// 所持カメラのダイジェスト認証パスワードを**平文で**返す(編集画面の表示用)。
	//  JSON(ownedCamerasJson / 撮影計画)には暗号文しか載せないため、UI で中身を見せるには
	//  この経路が要る。見つからない/未設定なら空文字。
	static std::string ownedCameraAuthPass(const std::string& name);

	// 所持カメラの撮影計画への自動挿入フラグを設定して保存する。
	static bool setOwnedCameraAutoInsert(const std::string& name, bool autoInsert);

	// --- 撮影場所(ユーザー資産。/asset/places.json。§5.1/§7.9) ---
	static std::string placesJson(void);                 // 登録済み場所の配列 JSON
	static bool addPlace(const std::string& name);       // 空名可(自動採番)。追加して保存
	static bool removePlace(const std::string& name);
	static bool setPlaceAutoInsert(const std::string& name, bool autoInsert);
	// 項目10: 「撮影計画に自動的に挿入する」場所は全体でただ1つ。全体設定(settings.json の
	// "autoInsertPlace")に場所の名称を1つだけ保存する。未設定なら空文字。
	static std::string autoInsertPlaceName(void);
	// 場所詳細(name/memo/latitude/longitude/altitude/autoInsert)を JSON で更新/新規作成。
	// origName 一致を置換、無ければ新規追加。改名は json の "name" を新名にする。
	static bool setPlaceDetailJson(const std::string& origName, const std::string& json);
	// 名称で場所を引く。見つからなければ false。
	static bool findPlace(const std::string& name, hgc::place& out);
	// 自動挿入フラグが立っている最初の場所を返す(新規撮影計画の初期値)。true=あり。
	static bool autoInsertPlace(hgc::place& out);

	// 所持カメラの詳細(全項目)を JSON で更新/新規作成して保存する。origName 一致を置換、
	// 無ければ新規追加(マスタに無い手動カメラ)。json キー:
	//  maker/model/name/assignedName/serial/sensorSize/sensorSizeV/sensorPixel/
	//  isoMin/isoMax/ssMin/ssMax(設定可能範囲。変更時は標準1/3段で再生成)/autoInsert/lensNames[]
	static bool setOwnedCameraDetailJson(const std::string& origName, const std::string& json);

	// 所持レンズの詳細を JSON で更新/新規作成して保存する。json キー:
	//  maker/name/focalLength/fn(F最小=開放)/fnMax(F最大)/hasContact
	static bool setOwnedLensDetailJson(const std::string& origName, const std::string& json);

	// --- システム共通の色(全体設定。/asset/settings.json の "colors") ---
	// 撮影制御方法ごとの文字色/背景色。型キー: night/sunrise/sunset/day/preNight/postNight。
	// {"night":{"text":int,"bg":int},...}。未設定は出荷時の既定色で補完する。
	static std::string colorsJson(void);
	static bool        setColorsJson(const std::string& json);

	// --- 撮影制御方法の初期値プリセット(/asset/ccmPresets.json。型ごとに複数) ---
	// 型キー: night/sunrise/sunset/day。各型は ccm の配列。
	// 初回は撮影制御方法の初期値(ccmDefaults)から型別名(星景/朝日/夕日/日中/月)のプリセットを
	// 生成して種まきする。旧種まき名「標準」が残る既存データは読み込み時に型別名へ改名する。
	static std::string ccmPresetsJson(const std::string& type);          // 指定型のプリセット配列
	// 追加/更新(origName 一致を置換、無ければ追加)。ccmJson は ccm 1件の JSON。
	static bool        setCcmPresetJson(const std::string& type, const std::string& origName, const std::string& ccmJson);
	static bool        removeCcmPreset(const std::string& type, const std::string& name);
	// 優先的な初期値(型ごとに1つ。settings.json の "preferredCcm")。
	static std::string preferredCcmName(const std::string& type);
	static bool        setPreferredCcm(const std::string& type, const std::string& name);
	// 新規撮影計画に使う撮影制御方法一式(項目14)。型ごとに「優先的な初期値」のプリセットを採用し、
	// 未指定/不在の型は内蔵初期値(factoryCcmSet)で補う。返り値は ccmSet 相当の JSON。
	static std::string preferredCcmSetJson(void);

	// --- 撮影計画への機材選択(所持から g_plan へ反映するのは UI/holyGrailEntity 側) ---
	// 所持カメラ/レンズを名称で引く。見つからなければ false。
	static bool findOwnedCamera(const std::string& name, hgc::camera& out);
	static bool findOwnedLens(const std::string& name, hgc::lens& out);

	// --- 接続時のシリアル/設定名の自動保存(§5.2拡張) ---
	// モデル一致の所持カメラへ serial/assignedName を保存。一致が無ければ
	// master(model)＋device から所持カメラを自動作成して保存(1台運用で無設定OK)。
	static bool recordConnectedCamera(const device& dev);

	// 発見/接続したカメラ識別情報を所持リストへ反映する共通コア。返り値=処理区分。
	//  UPDATED(0): serial一致の所持へ assignedName を反映(§1)。
	//  FILLED (1): serial未定義の同機種プレースホルダへ serial+assignedName を確定(§2)。
	//  ISNEW  (2): 一致する所持が無い(新規個体)。allowAdd なら新規追加済み、非 allowAdd なら未追加(呼び手が登録可否を問う)。
	// allowAdd=false は「裏の発見」用(登録するか UI で聞く)。true は撮影接続/明示登録用(自動追加)。
	enum class camApply { updated = 0, filled = 1, isNew = 2 };
	static int recordConnectedCameraStatus(const device& dev, bool allowAdd);

	// --- §4b 撮影開始時の特定カメラ照合(同機種が複数あっても serial/assignedName で1台を選ぶ) ---
	// 計画カメラの assignedName から所持リストを引き実シリアルを解決(接続済みなら serial が入る)。true=解決。
	static bool serialForAssignedName(const std::string& assignedName, std::string& outSerial);
	// 所持カメラに登録済みのシリアル一覧(空は除く)。「それ以外のカメラ」判定に使う。
	static void ownedCameraSerials(std::vector<std::string>& out);
	// 撮影画像から読めたセンサー実寸/画素数を所持カメラへ入れる(空のときだけ)。true=書いた。
	//  機材マスターに無い機種の穴埋め。ユーザーが手で入れた値は上書きしない。
	static bool fillOwnedCameraSensor(const std::string& serial, double sensorWmm, double sensorHmm, uint32_t pixelW);
	// 同じ機種として登録されている所持カメラの台数(個体が確定しているものだけ)。
	static int ownedCountForModel(const hgc::camera& cam);
	// device のモデルが計画/所持カメラ cam と同機種か(メーカー名差を吸収)。
	static bool cameraModelMatches(const device& dev, const hgc::camera& cam);

	// --- 撮影計画の永続化(案A /plan。1計画1ファイル plan_<id>.json。§7.4) ---
	// 撮影計画(cs)の自己完結JSONを保存/読込/削除する(撮影制御方法も cs が持つ)。
	// id は作成時刻 yyyyMMdd-HHmmss(衝突時 -NN)。dataManager は id の規則は関知しない。
	static bool savePlanFile(const std::string& id, const std::string& wrappedJson);
	static bool loadPlanFile(const std::string& id, std::string& out);
	static bool deletePlanFile(const std::string& id);
	// 保存済み撮影計画の id 一覧(昇順)。plan_<id>.json から id 部分を取り出す。
	static std::vector<std::string> listPlanIds(void);

	// --- 撮影実行状況の永続化(/asset/capturing.json。電源復帰/再起動時の再開用。item2) ---
	// 現在撮影中の計画id一覧を保存/読込する。撮影開始/停止時に更新し、起動時に読んで再開する。
	static bool saveCapturingIds(const std::vector<std::string>& ids);
	static bool loadCapturingIds(std::vector<std::string>& out);

	// (廃止 2026-07-05) saveCameraHost/loadCameraHost(cameraHosts.json)は削除。
	// 既知IP直結フォールバックの廃止(誤接続防止)に伴い不要。発見は M-SEARCH(再送化済み)のみ。

	// --- 旧単一ファイル(plan.json)の移行用(後方互換) ---
	// 撮影計画(cs)の自己完結JSONを /plan/plan.json へ保存する。
	static bool savePlanJson(const std::string& json);
	// 保存済み撮影計画JSONを読み込む。無ければ false。
	static bool loadPlanJson(std::string& out);
	// 旧 plan.json を削除する(移行後の後始末)。return: 成功。
	static bool removeLegacyPlan(void);

	// 任意の ccmSet を JSON 化/復元する(初期値ccmと計画固有ccmで共用)。
	static std::string ccmSetToJson(const astro::ccmSet& set);
	static bool parseCcmSetJson(const std::string& json, astro::ccmSet& set);


	// --- 動作ログ(データ構造仕様書43 §8) ---
	// 固定長128Bのテキストレコードを日付ごとのファイル(hg_YYYY-MM-DD.log)へ追記する。
	// 保存は osFile 抽象を介す(M5=SD/LittleFS, Android=外部ファイル領域)。

	// ログのタイムスタンプに使う UTCオフセット[分]を設定する(撮影開始時などに呼ぶ)。
	static void setLogOffset(int utcOffsetMin);

	// 露出を伴わないイベント(START/STOP/CCMSW/NET/ERR/INFO)を記録する。
	// event: §8.3 のイベント種別(6文字以内)。detail: 補足(55文字以内)。
	static void logEvent(const char* event, const char* detail, bool error = false);

	// --- デバッグログの取捨(2026-08-29 UI依頼) ---
	//
	// 【なぜ要るか】撮影1コマごとの SHOT/LVHIST と、電池の定期記録は量が多い。
	//  一晩で数千行になり、後から読むときに肝心の出来事が埋もれる。エッジでは
	//  書き込み自体が仕事を奪うし、内蔵の保存領域も食う。既定は**採らない**。
	//  必要なときだけ画面で入れてもらう。
	// 既定値は「両方とも採らない」。設定はスマホが持ち、エッジへは C_LOG_OPT で送る。
	static void setLogOptions(bool shot, bool batt);
	static bool logShotEnabled(void);
	static bool logBattEnabled(void);

	// 1枚撮影(SHOT)を記録する。frame/iso/ss/fn/lum と適用中ccm名。
	// meteredLinear: 測光したリニア輝度(自動補正時のみ。<0=測光なしで detail に出力しない)。
	// rdyMeteringMs/rdyShutterMs: 測光/露出設定の実測ms(>=0で detail 末尾に rdy=/set= を付与。計測用)。
	// meterTry/applyTry: それぞれの試行回数(1=一発成功。>1=リトライした)。rdy=/set= に try として付与。
	// prepMs: 準備(測光→露出計算→露出設定)の合計ms。リードに収まったかの判定用。
	// lateMs: シャッターが「前コマ+撮影周期」からどれだけ遅れたか[ms](0=周期ぴったり)。周期維持の主指標。
	// shutterEpochMs: actShutter直前の壁時計(UTCエポックms)。>0 で detail 末尾に sh=HH:MM:SS.mmm を付与(発光時刻の精密検証用)。
	static void logShot(int frame, const hgc::exposure& e, double lumStops, const char* ccmName,
	                    double meteredLinear = -1.0, int rdyMeteringMs = -1, int rdyShutterMs = -1, int prepMs = -1,
	                    int lateMs = -1, bool rdyOk = true, bool setOk = true, int meterTry = 0, int applyTry = 0,
	                    uint32_t histSum = 0, uint64_t lvTimeMs = 0, int staleSkip = 0, uint64_t shutterEpochMs = 0,
	                    int busyMs = -1, int firstApplyTries = 0);

	// 現在の(本日の)ログファイルのフルパスを返す(検証・ログ転送用)。
	static std::string currentLogPath(void);

	// --- 撮影結果レポート(1撮影=1ファイル。ログと同じディレクトリへ JSON で出す) ---
	// 撮影中に1コマずつ積算し、撮影終了時に要約を書く。
	// 目的: 「この機材/設定で無理が無かったか」をユーザーが自分で判断できる材料を残す。
	//  例) stale が多い=撮影周期がカメラのライブビュー更新に追いつかれていない → 周期を伸ばす判断ができる。
	// 所見は「文言」ではなく noteCode の数値で残し、表示する文言は UI 側が持つ(多言語化・
	// 文言変更をファイル形式から切り離すため)。
	enum noteCode : int
	{
		NOTE_SET_FAIL       = 1,	// 露出設定に失敗したコマがある(実機とアプリの露出がズレる)
		NOTE_STALE_MANY     = 2,	// ライブビューの更新が撮影周期に追いついていない
		NOTE_LATE_MANY      = 3,	// 撮影周期を守れないコマが多い
		NOTE_METER_FAIL     = 4,	// 測光できないコマが多い(明るさが追従しない)
		NOTE_BUSY_STUCK     = 5,	// (欠番。旧busyプローブ用。番号は再利用しない)
		NOTE_INTERVAL_TIGHT = 6,	// 目安の最短周期に対して設定周期の余裕がない
		NOTE_INTERVAL_ROOM  = 7,	// 周期にまだ余裕がある(もっと詰められる)
		NOTE_BUSY_NO_DATA   = 8,	// busy を1コマも測れなかった(周期が露光で埋まっている等)
		NOTE_CONVERGE_NONE  = 9,	// 撮影前の初期収束で一度も測れず、基準値のまま撮り始めた
		NOTE_CONVERGE_PART  = 10,	// 初期収束が終わりきらないまま撮り始めた(最良推定)
	};
	struct captureReport
	{
		int      frames    = 0;		// 撮影したコマ数
		int      meterTried = 0;	// 測光を試みたコマ数(自動露出区間のみ。夜間の固定露出は測光しないので含まない)
		int      meterFail = 0;		// 測光を試みたが取得できなかったコマ(露出は据え置き)
		int      setFail   = 0;		// リトライしても露出設定できなかったコマ(実機とアプリの露出がズレる)
		int      shootFail = 0;		// シャッターに失敗したコマ
		int      staleFrames = 0;	// 古いライブビューを捨てたコマ数
		long     staleTotal  = 0;	// 捨てた延べ回数
		int      meterRetryFrames = 0;	// 測光をリトライしたコマ数
		// 何で測ったかの内訳(コマ数)。サムネイルだけの方式では全コマが thumbFrames になり、
		//  ライブビュー主体方式では lvFrames が主で、thumbFrames が「ライブビューでは足りず
		//  落ちた回数」= 取得回数の予算をどれだけ使ったかになる。
		//  heldFrames は測らずに直近値を据え置いたコマ(間引き)。3つの合計が測光したコマ数。
		int      thumbFrames      = 0;	// 撮影画像のサムネイルで測った
		int      lvFrames         = 0;	// ライブビューで測った
		int      heldFrames       = 0;	// 測らず直近値を据え置いた
		int      applyRetryFrames = 0;	// 露出設定をリトライしたコマ数
		int      lateOk    = 0;		// 撮影周期を守れたコマ(遅れ <= captureRunner::kLateOkMs)
		int      lateCnt   = 0;		// 遅れを計測できたコマ(1枚目を除く)
		long     lateSum   = 0;		// 遅れの合計[ms]
		int      lateMax   = 0;		// 最大の遅れ[ms]
		// 「間に合わなかったコマ」だけの集計(ユーザー指示 2026-08-13)。
		// 準備(登録通知待ち→サムネイル取得→露出設定)が撮影周期に間に合わないと、その回は
		// 終わり次第すぐシャッターを切る=そのぶん遅れる。何回・何ms遅れたのかを載せる。
		// lateSum は守れたコマ(遅れ0)も含む合計で平均が薄まるため、分けて数える。
		int      lateOverCnt   = 0;	// 遅れたコマ数(= lateCnt - lateOk)
		long     lateOverSumMs = 0;	// そのコマだけの遅れの合計[ms]
		long     prepSum   = 0;		// 準備(測光→計算→設定)の合計[ms]
		int      prepMax   = 0;		// 準備の最大[ms]
		int      prepOver  = 0;		// 準備がリードに収まらなかったコマ数
		uint64_t firstShutterMs = 0;	// 最初/最後のシャッター(実周期の算出用)
		uint64_t lastShutterMs  = 0;
		// --- busy と準備の内訳(2026-08-05。「周期をどこまで SS へ詰められるか」を知るため) ---
		// busy = 露光が終わってからカメラが測光を受け付けるまで。撮影周期の下限を決める本体。
		// 従来は SS が速いコマほど busy が明けてから測光していたので一度も測れていなかった。
		int      busyCnt    = 0;	// busy を計測できたコマ数
		long     busySumMs  = 0;	// その合計[ms]
		int      busyMaxMs  = 0;	// その最大[ms]
		int      busyStuck  = 0;	// 準備開始までに明けなかったコマ数(下限しか分からない)
		int      meterCnt   = 0;	// 測光時間を計測できたコマ数
		long     meterSumMs = 0;	// 測光の合計[ms]
		int      meterMaxMs = 0;	// 測光の最大[ms]
		int      applyCnt   = 0;	// 露出設定の時間を計測できたコマ数
		long     applySumMs = 0;	// 露出設定の合計[ms]
		int      applyMaxMs = 0;	// 露出設定の最大[ms]
		double   maxSsSec   = 0.0;	// この撮影で使った最長シャッター[秒](最短周期の目安に使う)
		int      leadMs     = 0;	// 準備に与えられていたリード[ms](0=不明。測光が間に合ったかの基準)
		// 撮影開始前の初期収束の結果(セッション単位)。ここが済んでいないと1枚目から露出が外れる。
		int      cvSteps    = 0;	// 測光値を採用できた回数(0=一度も測れていない)
		int      cvApplyNg  = 0;	// 露出を適用できずやり直した回数
		int      cvMeterNg  = 0;	// 測光できずやり直した回数
		int      cvOutcome  = 3;	// 0=収束 / 1=収束しきらず最良推定 / 2=一度も測れず基準値のまま / 3=収束不要
		// 露出を合わせるために撮影窓の前で余分に撮ったコマ数。frames には入らないので、
		//  カードの枚数がレポートのコマ数より多くなる。その差の説明として出す。
		int      cvShots    = 0;
	};
	// レポートをファイルへ書く(JSON)。planName/planId/カメラ名と窓・周期は呼び出し側から渡す。
	// return: 書けたファイルのパス(空=失敗)。
	static std::string writeCaptureReport(const captureReport& r, const hgc::cs& plan, const char* planId);

	// --- 撮影レポートの取り出し(UI表示用 / エッジからの回収用) ---
	// レポートのファイル名一覧(中身は読まない)。件数だけ知りたいエッジの30秒応答でも使う。
	static std::vector<std::string> reportNames(void);
	// 一覧の JSON。撮影日時の新しい順。[{"name","plan","camera","shotAt","frames","noteCount"},...]
	static std::string reportListJson(void);
	// 1件の中身(保存した JSON をそのまま)。空=読めない。
	static std::string reportJson(const std::string& name);
	// 1件削除する。return: 成功。
	static bool removeReport(const std::string& name);
};

#endif // _DATA_MANAGER_H_
