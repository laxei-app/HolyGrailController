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
	errCode setupShootingModeManual(void) override;		// 撮影モードをM(ダイアル無視ON)へ。元値を保存
	errCode restoreShootingMode(void) override;			// 保存した撮影モードへ戻す(ダイアル無視OFF)

	// 情報を知る
	errCode getSettings(cmdt::shotRange& settings);		// 設定値を取得する
	errCode rdyMetering(void);							// 測光準備
	errCode alzMetering(cmdt::HISTOGRAM& histoOut);		    // 測光解析

protected:
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

	// 設定する(送信用テーブルで real に最も近いカメラ広告値を選んで送る)
	errCode setFNumber(const std::string& fNumber);		// f 値を設定する
	errCode setSS(const std::string& ss);				// シャッター速度を設定する
	errCode setIso(const std::string& iso);				// ISO を設定する

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
	};

};

#endif // _API_CANON_CCAPI_H_
