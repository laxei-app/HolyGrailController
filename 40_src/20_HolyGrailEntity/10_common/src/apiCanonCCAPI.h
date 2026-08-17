#ifndef _API_CANON_CCAPI_H_
#define _API_CANON_CCAPI_H_

#include "apiBase.h"

class apiCanonCCAPI : public apiBase
{
public:
	// getStrage で得られる内容
	struct strageInfo
	{
		std::string		name;					// ストレージ名
		std::string		path;					// ストレージの path
		std::string		acce;					// アクセス可否
		uint64_t		maxS = 0;				// ストレージ最大容量(byte)
		uint64_t		spaS = 0;				// 捨てレージ空き容量(byte)
		uint64_t		conN = 0;				// 総コンテンツ数
	};

protected:
	// 機能番号
	enum class funcNum
	{
		SHOT = 0,					// 静止画撮影
		F_NUMBER,					// 絞り
		SS,							// シャッター速度
		ISO,						// ISO
		LIVE_SET,					// live view 設定
		LIVE_DETAIL,				// ライブビュー 付帯情報
		STRAGE_ACT,					// 保存先に指定されている strage path を取得する
		STRAGE_STA,					// strage 全体の状態取得
		DIR_ACT,					// 保存先のディレクトリ path を取得する
		L_FILE,						// ファイルリスト
		IGNORE_DIAL,				// 撮影モードダイアル無視モード(control/ignoreshootingmodedialmode)
		SHOOTMODE_DIAL,				// 撮影モード(ダイアル搭載機: settings/shootingmodedial)
		SHOOTMODE,					// 撮影モード(ダイアル非搭載機: settings/shootingmode)
		AUTOPOWEROFF,				// オートパワーオフ(functions/autopoweroff)。撮影中は disable に抑止
		EVENT_POLL,					// イベント取得(event/polling)。撮影画像の登録通知(addedcontents)に使う

		// 全体の要素数
		NUM,
		NON,				// 要素：無し
	};

	class verb
	{
	public:
		enum type
		{
			NON = 0,
			GET = 0x01,
			PUT = 0x02,
			POS = 0x04,
			DEL = 0x08,
		};
	protected:
		uint8_t value = (uint8_t)NON;
	public:
		verb(verb::type type)
		{
			value = static_cast<uint8_t>(type);
		}

		// 有効な動作を指定する
		verb& operator |=(verb::type type)
		{
			this->value |= static_cast<uint8_t>(type);
			return *this;
		}

		// 指定の動作が有効か判断する
		bool operator ==(verb::type type)
		{
			return value & static_cast<uint8_t>(type);
		}

		// すべてクリアする
		void clear(void)
		{
			value = static_cast<uint8_t>(NON);
		}
	};

	// カメラインターフェースを保存する形式
	class func
	{
	public:
		apiCanonCCAPI::funcNum	funcNum = funcNum::NON;     // funcNum　機能番号。この番号で機能を指定する
		std::string             url;				        // アクセス url
		class verb		        verb = verb::NON;			// get/put/push/delete の機能

	};

	// デバイスに関する情報。
	class device			device;		// デバイス。コピーして持つ。
	std::unordered_map<apiCanonCCAPI::funcNum, class func> funcList;	// 機能リスト

	// 送信用テーブル: 撮影開始時(getSettings)にカメラの ability から作る。
	// real(実数)→ カメラが実際に広告した設定値文字列(send)。送信時はこの文字列を
	// そのまま PUT するため、ファーム差・機種差の書式("8" vs 8" 等)に左右されない。
	struct sendMap { std::string send; double real = 0.0; };
	std::vector<sendMap> ssSend_;	// シャッター速度
	std::vector<sendMap> isoSend_;	// ISO
	std::vector<sendMap> fnSend_;	// F値

	// 撮影モード変更の復元用(setupShootingModeManual で保存)。
	std::string savedShootMode_;	// 元の撮影モード値("av"等)
	bool        savedIsDial_  = false;	// ダイアル搭載機(shootingmodedial)で変更したか
	bool        shootModeChanged_ = false;	// 変更したか(restore要否)

	// オートパワーオフ抑止の復元用(setupShootingModeManual で保存)。
	std::string savedAutoPowerOff_;		// 元の autopoweroff 値("30"等)
	bool        autoPowerOffChanged_ = false;	// disable に変更したか(restore要否)

protected:

public:
	apiCanonCCAPI(void);
	virtual ~apiCanonCCAPI(void);
	errCode init(class device & device);	// 初期化(SSDP/UPnP記述子経由)
	// 身元だけ(デバイス記述のみ。CCAPI は叩かない)。理由は apiBase::identify を参照。
	errCode identify(class device & device) override { return getDeviceDescriptor(device); }
	errCode initManual(class device & device);	// 手動初期化(device.urlAccessを直接使用。記述子取得をスキップ)

	// カメラとの直接の指示
public:
	// 動作を指示する
	errCode rdyShutter(const cmdt::shotSet& shotSet);	// シャッター設定
	errCode actShutter(void);							// シャッターを切る動作
	errCode startShooting(void);						// 撮影開始
	errCode stopLiveView(void) override;				// ライブビュー停止(撮影ループ中は掴まない)
	// 撮影ループ中にライブビューが要るか。サムネ測光では不要(初期収束のときだけ使う)が、
	//  ライブビュー主体方式では毎コマ測るので掴んだままにする。
	bool    liveViewNeededWhileCapturing(void) const override { return meterLv_; }
	bool    liveViewAlive(void);						// ライブビューが実際に流れているか(?kind=info。接続の生存確認)
	errCode setupShootingModeManual(void) override;		// 撮影モードをM(ダイアル無視ON)へ。元値を保存
	errCode restoreShootingMode(void) override;			// 保存した撮影モードへ戻す(ダイアル無視OFF)
	errCode keepAlive(void) override;					// 接続維持用の無害なGET(ついでに登録通知も流す)

	// 情報を知る
	errCode getSettings(cmdt::shotRange& settings);		// 設定値を取得する
	errCode rdyMetering(void);							// 測光準備
	errCode alzMetering(cmdt::HISTOGRAM& histoOut);		    // 測光解析

	// 露出を1項目ずつ設定する(送信用テーブルで real に最も近いカメラ広告値を選んで送る)。
	// 周期正確化リアーキ(タイマ方式)では rdyShutter を使わず、変更のあった項目だけを個別に呼ぶ。
	errCode setFNumber(const std::string& fNumber) override;		// f 値を設定する
	errCode setSS(const std::string& ss) override;					// シャッター速度を設定する
	errCode setIso(const std::string& iso) override;				// ISO を設定する

	// === 測光(apiBase の CCAPI 実装。2026-07-27 captureRunner から移設) ===
	//
	// 【方式(2026-08-13 これ1本に確定)】直前に撮れた**最新の撮影画像のサムネイル**から輝度を出す。
	//  1コマの流れ:
	//    シャッター → 露光(ss) → event/polling で登録通知 → 最新ファイルのサムネイル取得
	//    → リニア輝度 → 露出設定 → 撮影周期の残りを待つ → 次のシャッター
	//  ・本露光そのものの積分なので、ライブビューが約1.6秒相当で頭打ちになる夜間でも真値が出る
	//    (7/26 布かぶせ実験で頭打ちを実証、8/08 実写突合で中央値が最大4.35段暗いことを確認)。
	//  ・測光のためにカメラの露出を触らないので、測光ss切替のPUTも反映待ち(実測2.6秒)も消える。
	//  ・サムネイルは JPG があれば JPG、無ければ RAW から取る(pickThumbPath)。
	//
	// 【ライブビュー方式(旧)は廃止した】撮影ループ中はライブビューを掴みもしない。
	//  唯一の例外が meterHere で、これは撮影窓の手前(まだ1コマも撮っていない=サムネイルの元に
	//  なる画像が無い)の初期収束専用。ここだけは他に測る手段が無いのでライブビューを使う。
	//  旧方式の測光ss切替・張り付き学習・反映待ちは、この方式では不要になったので削除した
	//  (履歴が要るときは 2026-08-13 以前の apiCanonCCAPI を参照)。
	//
	// 【EOS R10 の不具合について】この方式は機種を選ばない汎用の測光である。ただし EOS R10 は
	//  生成直後のファイルに触ると撮影エンジンが固着するという**カメラ側の不具合**を持つ
	//  (取得元 CR3 で約49回、JPG で約179回。R100 は同条件で300コマ完走=2026-08-13 実測)。
	//  そのため取得元は JPG を優先する(R10 の持ちが約3.7倍になり、壊れ方も浅くなる)。
	//  固着したカメラは「シャッターは通るのに画像が記録されない」状態になり、情報系も
	//  ライブビューも生きているので接続の生死では見分けられない。そこで新しい画像が現れなかった
	//  ことを meterResult::shotMissing で上位へ申告し、captureRunner がオフライン提示へ回す
	//  (captureRunner::kMaxNoRecordFrames)。R10 を外すのではなく、壊れたら気づける形にしてある。
	errCode meterScene(const hgc::exposure& shotExp, meterResult& out,
	                   const std::function<bool()>& keepGoing) override;
	errCode meterHere(meterResult& out, const std::function<bool()>& keepGoing) override;
	void    resetMeterCadence(void) override { lvFallbackSkip_ = 0; }	// 間引きを捨てる(初期収束用)
	// 測光をいつ呼んでほしいか: 露光が閉じ次第すぐ。ソースは直前の撮影画像なので待つ理由が無く、
	// 早く測るほど「露出設定→撮影周期まで待つ」の余裕が増える(=周期を守りやすい)。
	meterTiming meterTimingHint(void) const override { return meterTiming{ true, 0 }; }
	void    setMeterLv(bool useLv) override { meterLv_ = useLv; }
	void    meterReset(void) override;
	void    meterArm(void) override;		// 1枚目の直前に溜まった通知を捨てる

protected:
	// 測光方式(所持カメラの設定。setMeterLv で受ける)。false=サムネイルだけ(既定・最も正確)、
	//  true=ライブビュー主体(サムネイル取得に回数上限がある機種の逃げ道)。
	bool             meterLv_ = false;
	// --- ライブビュー主体方式の状態(セッション単位。meterReset で捨てる) ---
	//  ライブビューが効かない暗さのときだけサムネイルへ落ちる。そのサムネイルは
	//  **1コマ前**のファイルから取る(生成直後のファイルに触ると R10 が固まるため。
	//  2026-08-13 実測で 4/4 停止)。そのため直近2コマぶんのパスを覚えておく。
	std::string      lvShotPrev_;			// 1コマ前の撮影ファイル(サムネイルの取得先)
	std::string      lvShotLast_;			// 直近の通知で現れたファイル
	int              lvFallbackSkip_ = 0;	// 間引き用の残りコマ数(0でサムネイルを取る)
	double           lvHeldSceneRef_ = 0.0;	// 間引き中に返す、直近サムネイルの場面基準
	int              lvFallbackShots_ = 0;	// このセッションでサムネイルを取った回数(診断用)
	// 直近のライブビュー測光が「底に張り付いたまま、ss も ISO も伸ばしきった」で終わったか。
	//  = ライブビューではこの暗さが見えない。サムネイルへ落ちる唯一の判断材料。
	bool             lvStretchedOut_ = false;

	// --- 測光の内部状態(セッション単位。meterReset で捨てる) ---
	expo::expoTables tables_;			// 設定可能値テーブル(getSettingsでabilityから自前構築。
										//  中身も表記もカメラ依存なのでこの層が作る。共通層は渡さない)
	// いまカメラに乗っている露出。getSettings(ability応答の value)で初期化し、以後 setSS 等の
	// 成功で更新する。meterHere が測光露出の出発点に使う(上位から渡してもらう必要をなくすため)。
	hgc::exposure    camExp_;
	// 初期収束(meterHere)が使っている測光露出。呼ぶたびに白飛び/黒潰れを見てずらし、次回へ引き継ぐ。
	hgc::exposure    hereExp_;
	// 初期収束のライブビュー測光でフレームの鮮度を見る基準(露光前の古い映像を掴まないため)。
	uint64_t         lvFreshPrevMs_  = 0;		// 直近採用フレームのカメラ側時刻
	void*            lvFreshPrevAt_  = nullptr;	// その採用時点の実時刻アンカー
	// 中断可能な待ち(keepGoing が false になったら早期に戻る)。
	void        meterSleep(int ms, const std::function<bool()>& keepGoing) const;
	// 測光露出をテーブル上で delta 段ずらす(結果の ss は capSec を超えない)。
	void        shiftMeterSs(hgc::exposure& me, double delta, double capSec) const;
	// 同上。ss だけで届かないぶんを ISO で補う(ライブビューの限界は ss の長さなので、
	//  ss を詰めて ISO で戻すと同じ明るさを短い ss で見られる)。詳しくは .cpp の説明。
	void        shiftMeterExp(hgc::exposure& me, double delta, double capSec) const;
	// ライブビュー測光の本体。seed が有効ならその露出のまま測る(何も送らない)。
	//  張り付いたときだけ測光専用の露出へ乗せ替える。詳しくは .cpp の説明を参照。
	errCode     meterLvAt(const hgc::exposure& seed, meterResult& out,
	                      const std::function<bool()>& keepGoing);
	// ライブビューのヒストグラムを1回読む(取れるまで上限まで粘る)。初期収束の下請け。
	errCode     readLvHistogram(meterResult& out, const std::function<bool()>& keepGoing);
	// --- 撮影画像フィードバック測光 ---
	// 直前に撮れた画像のサムネイルから測光する(本体)。
	errCode meterSceneShot(const hgc::exposure& shotExp, meterResult& out,
	                       const std::function<bool()>& keepGoing);
	// ライブビュー主体方式の測光。詳しくは .cpp の説明を参照。
	errCode meterSceneLvFirst(const hgc::exposure& shotExp, meterResult& out,
	                          const std::function<bool()>& keepGoing);
	// 中核: 新規画像を待ち→サムネイル取得→復号→輝度ヒスト統計まで(露出非依存の部分)。
	//  pathOverride を渡すと「待ち」を飛ばしてそのファイルを測る(1コマ前を取るときに使う)。
	errCode thumbMeterCore(meterResult& out, int budgetMs, const std::function<bool()>& keepGoing,
	                       const std::string& pathOverride = std::string());
	// 新規画像待ちの診断(どの通信でつまずいたか)。meterResult へそのまま載せる。
	struct waitDiag { int step = 0; int http = 0; std::string body; };
	// 新規画像の検知は event/polling(カメラからの登録通知)。ディレクトリを一切読まないので、
	// 記録中のカードを叩かずに済む。2026-08-12〜13 の実測ではこの流れで R100 が 300コマ完走した。
	//  ※総数ポーリング方式(1コマ10〜13回ディレクトリを読む)は 2026-08-13 に削除した。
	std::string waitAddedByEvent(int budgetMs, const std::function<bool()>& keepGoing,
	                             int& triesOut, waitDiag& diag);
	// addedcontents の中から取得元を選ぶ。JPG+RAW記録だと1コマで2つ通知されるため。
	//  JPG があれば JPG(埋め込みサムネイルをそのまま返せる)、無ければ RAW。
	static std::string pickThumbPath(const std::vector<std::string>& names);
	// 登録通知の引き方は「クエリ無し(待たずに即返る)を間隔をあけて繰り返す」に固定する。
	//
	// 【なぜ長ポール(?continue=on / ?timeout=short)を使わないか(2026-08-13 実機で確定)】
	//  ・?continue=on に対し CCAPI はまず **100 Continue** を返してから、イベントが起きるまで
	//    接続を保持する。当方の httpGet は最終ステータスしか見ないので 100 を失敗と判定し、
	//    間隔をあけて次の要求を出す。**カメラは保持していた接続へイベントを流すので、
	//    こちらが捨てた接続と一緒に通知が消える**。
	//  ・実測(StickS3+R100 80コマ): 測光失敗 52コマ=65%、通知待ちの平均 7.5秒。
	//    同じカメラをクエリ無しで引いた CoreS3 は 36コマ中1コマ(2.8%)・平均2.0秒だった。
	//  ・どの方式に落ち着くかは「判定を試した瞬間に通知が溜まっていたか」で決まるため、
	//    走行ごとに結果が変わる(再現しないバグになる)。方式の自動判定そのものをやめる。
	//  ・クエリ無しは1コマ6〜10往復になるが、カードには触らない軽い要求である。
	//    中断(keepGoing)を効かせられる利点もあり、固まったカメラの検出が速い。
	std::string pollUrl(void) const;	// クエリ無しの event/polling URL
	void        stopEventPolling(void);		// DELETE /event/polling(イベント取得の停止。セッション終了時)
	// 溜まっている通知を1回で流す(セッション開始時)。前の撮影の登録通知が残っていると、
	// 1枚目で「古い画像のサムネイル」を掴んでしまう。
	void        flushEventPolling(void);
	// funcList のURLから "http://host:port" 部分を得る(コンテンツパスの絶対URL化に使う)。
	std::string apiHostBase(void) const;

	// カメラの情報を取得する
	errCode getDeviceDescriptor(class device& device);
	errCode ascCanFNumber(std::vector<std::string>& fNumber);	// 指定可能な f 値(文字列)を取得する
	errCode ascCanSS(std::vector<std::string>& ss);				// 指定可能な シャッター速度(文字列)を取得する
	errCode ascCanIso(std::vector<std::string>& iso);			// 指定可能な ISO(文字列)を取得する
	errCode getStrageAct(std::string& path);			// 保存先の(activeな)strage 情報を取得する
	errCode getStrageSta(std::vector<strageInfo>& strageInfo);	// 全体のstrage 情報を取得する
	errCode getDirAct(std::string& path);				// 保存先のディレクトリ
	errCode getLastFile(std::string& path, std::string& file); //最後のファイル名を取得する
//	errCode getHistoGram(cmdt::HISTOGRAM & hist);			// live view のヒストグラムを取得する
	errCode getShotPicture(std::vector<std::byte>& jpg);

	// (setFNumber/setSS/setIso は public へ移動)

	// 送信用テーブルから real に最も近い「カメラが広告した文字列」を返す。空=該当なし。
	std::string sendFor(const std::vector<sendMap>& map, double real) const;

	// funcList に登録済み(カタログに存在)で指定 verb を持つか。
	bool hasFunc(funcNum n, verb::type v);

	// データ解析
	errCode analizeUseFunction(class device& device, std::string& catalog);			// 使用するコマンドを探す
	// ability(設定可能値)を取る。curRaw != nullptr なら同じ応答に入っている現在値(生文字列)も返す。
	// 現在値は「いまカメラに何が乗っているか」(camExp_)の初期化に使う。GETは1回のままで済む。
	errCode getJsonAbility(funcNum number, std::vector <std::string>& abilitys, std::string* curRaw = nullptr);
	errCode setJsonvalue(funcNum number, float val);

protected:
	// 必要な機能リスト
	// accessURL から取得した機能一覧より必要な機能の URL を取得する際に使用する
	class useFunction
	{
	public:
		bool				    find;				// 取得できたか否か
		apiCanonCCAPI::funcNum	funcNum;			// funcNum　機能番号。この番号で機能を指定する
		std::string			    surfix;				// url のキーワード。この文字列で後ろから探す。「control/shutterbutton」「control/shutterbutton/manual」を区別するため。

	public:
		useFunction(bool find, apiCanonCCAPI::funcNum	funcNum, std::string surfix)
		{
			this->find = find;
			this->funcNum = funcNum;
			this->surfix = surfix;
		}
	};
	
    std::string liveViewInfo;                       // rdyMetering() から alzMetering() に引き継ぐデータ
    uint64_t    lvSysTimeMs_ = 0;                   // 直近 alzMetering が解析したフレームの liveviewdata.systemtime [ms]

	// useFunction に登録された機能を削除する
	void useFunctionClear(void);

	// カタログ(/ccapi)が使えないカメラのために、必要な機能のパスを直接叩いて funcList を作る。
	//  詳しくは .cpp の説明を参照(EOS R50 V 対応)。
	errCode probeUseFunction(class device& device);

	// 使う機能の一覧
	std::vector<class useFunction> useFunction =
	{
		{false, funcNum::SHOT,				"control/shutterbutton"},		// 1発で撮る
		{false, funcNum::F_NUMBER,			"/shooting/settings/av"},		// 絞りを設定、取得
		{false, funcNum::SS,				"/shooting/settings/tv"},		// シャッター速度を設定、取得
		{false, funcNum::ISO,				"/shooting/settings/iso"},		// ISO 設定、取得
		{false, funcNum::LIVE_DETAIL,		"/shooting/liveview/flipdetail"},	// ライブビュー付帯情報
		{false, funcNum::LIVE_SET,			"/shooting/liveview"},				// live view 設定

		{false, funcNum::STRAGE_STA,		"/devicestatus/storage"},		// ストレージ状態取得
		{false, funcNum::STRAGE_ACT,		"/devicestatus/currentstorage"},// 現在のストレージパス取得
		{false, funcNum::DIR_ACT,			"/devicestatus/currentdirectory"},		// ディレクトリパス取得
		{false, funcNum::L_FILE,			"/contents"},					// 状態取得
		{false, funcNum::IGNORE_DIAL,		"control/ignoreshootingmodedialmode"},	// 撮影モードダイアル無視
		{false, funcNum::SHOOTMODE_DIAL,	"settings/shootingmodedial"},	// 撮影モード(ダイアル機)
		{false, funcNum::SHOOTMODE,			"settings/shootingmode"},		// 撮影モード(ダイアル無し機)
		{false, funcNum::AUTOPOWEROFF,		"functions/autopoweroff"},		// オートパワーオフ(撮影中 disable)
		{false, funcNum::EVENT_POLL,		"/event/polling"},				// 撮影画像の登録通知(サムネ測光)
	};

};

#endif // _API_CANON_CCAPI_H_
