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
	errCode initManual(class device & device);	// 手動初期化(device.urlAccessを直接使用。記述子取得をスキップ)

	// カメラとの直接の指示
public:
	// 動作を指示する
	errCode rdyShutter(const cmdt::shotSet& shotSet);	// シャッター設定
	errCode actShutter(void);							// シャッターを切る動作
	errCode startShooting(void);						// 撮影開始
	errCode stopLiveView(void) override;				// ライブビュー停止(撮影ループ中は掴まない)
	// 撮影ループ中にライブビューが要るか。サムネ測光では不要(初期収束のときだけ使う)。
	bool    liveViewNeededWhileCapturing(void) const override { return !kUseShotThumbMetering; }
	bool    liveViewAlive(void);						// ライブビューが実際に流れているか(?kind=info)
	errCode setupShootingModeManual(void) override;		// 撮影モードをM(ダイアル無視ON)へ。元値を保存
	errCode restoreShootingMode(void) override;			// 保存した撮影モードへ戻す(ダイアル無視OFF)
	errCode keepAlive(void) override;					// 接続維持用の無害なGET(/ccapi カタログ取得)

	// 情報を知る
	errCode getSettings(cmdt::shotRange& settings);		// 設定値を取得する
	errCode rdyMetering(void);							// 測光準備
	errCode alzMetering(cmdt::HISTOGRAM& histoOut);		    // 測光解析
	uint64_t lastLvTimeMs(void) override { return lvSysTimeMs_; }	// 直近フレームのカメラ側取得時刻[ms]

	// 露出を1項目ずつ設定する(送信用テーブルで real に最も近いカメラ広告値を選んで送る)。
	// 周期正確化リアーキ(タイマ方式)では rdyShutter を使わず、変更のあった項目だけを個別に呼ぶ。
	errCode setFNumber(const std::string& fNumber) override;		// f 値を設定する
	errCode setSS(const std::string& ss) override;					// シャッター速度を設定する
	errCode setIso(const std::string& iso) override;				// ISO を設定する

	// === 測光(apiBase の CCAPI 実装。2026-07-27 captureRunner から移設) ===
	// 測り方は2方式(meterScene が kUseShotThumbMetering で切り替える):
	//  A) 撮影画像フィードバック(既定): 直前に撮れた画像のサムネイルから輝度を得る。本露光の
	//     積分そのものなので夜間でも真値(LVは~1.6秒相当で頭打ち=7/26布かぶせ実験で実証)。
	//     測光ssの切替が不要になり、切替PUT/settle待ち(2.6秒)も消える。
	//  B) LVヒストグラム(旧方式・即復活可): 暗所ではLVが積分できないため測光ssへ一時切替し、
	//     場面の明るさへ割り戻す。測光ssの学習・張り付き検出などの適応状態もこの層が持つ。
	// meterHere(現在露出のまま測る)は常にLV方式(シャッター前の初期収束用=まだ撮影画像が無い)。
	// 2026-07-31: サムネ測光は採用を取り下げた。R10 で撮影と併用すると数十分で撮影エンジンが
	//  固まり、バッテリを抜くまで戻らない(カード交換・接続方式・LV解放をすべて試して否定済み。
	//  読み出しのみ1000回4時間は完走したので、書き込みとの競合が原因と切り分けた)。
	//  速度面の利点も出ていない(サムネ取得に15秒かかることがある)。LV方式へ戻す。
	static constexpr bool kUseShotThumbMetering = false;	// true でサムネ方式(採用しない)
	errCode meterScene(const hgc::exposure& shotExp, meterResult& out,
	                   const std::function<bool()>& keepGoing) override;
	errCode meterHere(meterResult& out, const std::function<bool()>& keepGoing) override;
	// 測光をいつ呼んでほしいか(captureRunnerがこの申告に従う):
	//  方式A(サムネ)=露光終了直後(ソースは直前の撮影画像なので最速で呼べる) /
	//  方式B(LV)=シャッターの kMeterLeadLvMs 手前(一定時間前の輝度を見る設計)。
	static constexpr int kMeterLeadLvMs = 5000;
	meterTiming meterTimingHint(void) const override
	{
		return kUseShotThumbMetering ? meterTiming{ true, 0 } : meterTiming{ false, kMeterLeadLvMs };
	}
	void    meterReset(void) override;
	// busy計測(2026-08-05): LV方式の測光はライブビューが流れていることが前提なので、
	// その一点だけを ?kind=info の1往復で見る(カードには触らない)。
	int     meterReadyProbe(void) override;

protected:
	// --- 測光の内部状態(セッション単位。meterReset で捨てる) ---
	expo::expoTables tables_;			// 設定可能値テーブル(getSettingsでabilityから自前構築。
										//  中身も表記もカメラ依存なのでこの層が作る。共通層は渡さない)
	std::string      meterSs_;			// 次に使う測光ss(空=未決定→撮影ssから既定段数短く)
	bool             lvNeedSwitch_ = false;	// 撮影ssのままでは測れない(測光ssへ切替が要る)
	int              lvAsIsWait_   = 0;		// 切替なしを再試行するまでの残りコマ数
	int              lvPinStreak_  = 0;		// 切替なし経路で応答が無かった連続回数(張り付き確定用)
	// 撮影露出のままLV測光して使えるか(リニア輝度が使える範囲に入っているか)。
	bool             lvUsableAsIs(double linear) const;
	double           meterCeilStops_ = 1e9;	// 測光ssの長さ上限[段](張り付き検出で下がる天井)
	double           meterPrevStops_ = 0.0;	// 前回測光の明るさ[段](張り付き判定用)
	double           meterPrevLin_   = -1.0;	// 前回測光のリニア値(<0=無し)
	uint64_t         lvFreshPrevMs_  = 0;	// 直近採用フレームのカメラ側時刻(鮮度判定)
	void*            lvFreshPrevAt_  = nullptr;	// その採用時点の実時刻アンカー
	// 測光ssの決定と適応(旧 captureRunner::enterMeteringShutter / updateMeterShutter)。
	std::string decideMeterSs(const hgc::exposure& shotExp) const;
	// shotExp は測光ssの天井の「下限」を決めるのに使う(撮影ss-kMeterInitDropStops より下げない)。
	void        adaptMeterSs(const hgc::exposure& meterExp, const hgc::exposure& shotExp,
	                         double linear, bool& pinnedOut);
	// 中断可能な待ち(keepGoing が false になったら早期に戻る)。
	void        meterSleep(int ms, const std::function<bool()>& keepGoing) const;
	// 測光ss切替がライブビューへ反映されるまで待つ(反映を確かめて早く抜ける。上限は budgetMs)。
	int         waitLvReflect(double beforeLinear, double deltaStops,
	                          int budgetMs, const std::function<bool()>& keepGoing);
	// --- 撮影画像フィードバック測光(方式A) ---
	// LV方式の meterScene 本体(方式B。コード温存・kUseShotThumbMetering=false で復活)。
	errCode meterSceneLv(const hgc::exposure& shotExp, meterResult& out,
	                     const std::function<bool()>& keepGoing);
	// 直前に撮れた画像のサムネイルから測光する(方式A本体)。
	errCode meterSceneShot(const hgc::exposure& shotExp, meterResult& out,
	                       const std::function<bool()>& keepGoing);
	// 方式Aの中核: 新規画像を待ち→サムネイル取得→復号→輝度ヒスト統計まで(露出非依存の部分)。
	errCode thumbMeterCore(meterResult& out, int budgetMs, const std::function<bool()>& keepGoing);
	// 新規画像待ちの診断(どの通信でつまずいたか)。meterResult へそのまま載せる。
	struct waitDiag { int step = 0; int http = 0; std::string body; };
	// 新規画像の検知方式(2026-07-30 切り替え式に戻した。両方のコードを残してある)。
	//  contentsCount: コンテンツ総数の増加で検知。ディレクトリを1コマ10〜13回読む。
	//  eventPolling : カメラからの登録通知を待つ。**ディレクトリを一切読まない**。
	// 【2026-07-30 実機結果】eventPolling を試したところ症状が変わった:
	//  ・Canon の Err70(撮影処理の異常。電源かバッテリの入れ直しを促す表示)が一瞬出た
	//  ・「カメラが見つかりません」も出た。どちらもシャッターボタンで復帰したが、
	//    アプリからは復帰できず、元の症状と別物になった。
	// カード接触は減るはずだが改善しなかったので contentsCount へ戻す。
	// 切り替えはこの定数1つ。両方式のコードは残してある。
	enum class newImageDetect : uint8_t { contentsCount = 0, eventPolling = 1 };
	static constexpr newImageDetect kNewImageDetect = newImageDetect::contentsCount;

	// 露光終了からカードを触り始めるまでの待ち[ms](2026-07-30 の実験)。
	// カメラが記録中(busy)の間にこちらからアクセスするのが不具合の引き金か確かめるため。
	// 0 にすると従来どおり露光終了直後から取りに行く。測光の予算(kThumbBudgetMs)から引かれる。
	static constexpr int kCardSettleMs = 3000;

	// 新規画像が記録されるのを待ってそのパスを返す。空=時間内に現れなかった(理由は diag)。
	std::string waitAddedContents(int budgetMs, const std::function<bool()>& keepGoing,
	                              int& triesOut, waitDiag& diag);
	// 方式A: コンテンツ総数の増加で検知する
	std::string waitAddedByCount(int budgetMs, const std::function<bool()>& keepGoing,
	                             int& triesOut, waitDiag& diag);
	// 方式B: event/polling の登録通知で検知する
	std::string waitAddedByEvent(int budgetMs, const std::function<bool()>& keepGoing,
	                             int& triesOut, waitDiag& diag);
	// event/polling の待ち方(CCAPI Reference 4.13.1)。カメラのCCAPIバージョンで指定方法が違う:
	//  ver110〜: ?timeout=short(約10秒待つ) / ver100: ?continue=on(100 Continueで待つ)
	//  無指定は「待たずに即返る」が既定のため、こちらが連打してしまう(1コマ50回前後を実測)。
	enum class pollMode : uint8_t { unknown = 0, timeoutShort = 1, continueOn = 2, immediate = 3 };
	pollMode    pollMode_ = pollMode::unknown;
	std::string contentsDir_;	// 撮影画像の保存先(絶対URL。セッション中は使い回す)
	// 保存先を /contents から辿って得る(セッション中キャッシュ)。失敗時は step/http/body に理由。
	std::string contentsDirUrl(int& step, int& http, std::string& body);
	uint32_t    contentsBase_ = 0xFFFFFFFFu;	// 新規画像検知の基準となる総数(0xFFFFFFFF=未取得)
	std::string pollUrl(pollMode m) const;	// 方式に応じたURL(クエリ付き)を作る
	void        stopEventPolling(void);		// DELETE /event/polling(イベント取得の停止。セッション終了時)
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
	errCode getJsonAbility(funcNum number, std::vector <std::string>& abilitys);
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
