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

protected:

public:
	apiCanonCCAPI(void);
	virtual ~apiCanonCCAPI(void);
	errCode init(class device & device);	// 初期化

	// カメラとの直接の指示
public:
	// 動作を指示する
	errCode rdyShutter(const cmdt::shotSet& shotSet);	// シャッター設定
	errCode actShutter(void);							// シャッターを切る動作
	errCode startShooting(void);						// 撮影開始

	// 情報を知る
	errCode getSettings(cmdt::shotRange& settings);		// 設定値を取得する
	errCode rdyMetering(void);							// 測光準備
	errCode alzMetering(cmdt::HISTOGRAM& histoOut);		    // 測光解析

protected:
	// カメラの情報を取得する
	errCode getDeviceDescriptor(class device& device);
	errCode ascCanFNumber(std::vector<float>& fNumber);	// 指定可能な f 値を取得する
	errCode ascCanSS(std::vector<float>& ss);			// 指定可能な シャッター速度を取得する
	errCode ascCanIso(std::vector<float>& iso);			// 指定可能な ISO を取得する
	errCode getStrageAct(std::string& path);			// 保存先の(activeな)strage 情報を取得する
	errCode getStrageSta(std::vector<strageInfo>& strageInfo);	// 全体のstrage 情報を取得する
	errCode getDirAct(std::string& path);				// 保存先のディレクトリ
	errCode getLastFile(std::string& path, std::string& file); //最後のファイル名を取得する
//	errCode getHistoGram(cmdt::HISTOGRAM & hist);			// live view のヒストグラムを取得する
	errCode getShotPicture(std::vector<std::byte>& jpg);

	// 設定する
	errCode setFNumber(float fNumber);					// f 値を設定する
	errCode setSS(float ss);							// シャッター速度を設定する
	errCode setIso(float iso);							// ISO を設定する

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
	};

};

#endif // _API_CANON_CCAPI_H_
