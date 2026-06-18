#include "common.h"
#include "apiCanonCCAPI.h"
#include "netThread.h"
#include "exposureMath.h"
#include <json/nlohmann/json.hpp>
#include <cmath>

using json = nlohmann::json;

namespace
{
	// シャッター速度の文字列フォーマット変換。
	// 内部表記(秒は "8" "0.5" "3.2"、高速は "1/4000"、バルブ "Bulb") と
	// Canon CCAPI 表記(秒は 8" 0"5 3"2、高速は 1/4000、バルブ bulb) を相互変換する。
	// EOS R10 の tv ability は秒を二重引用符付きで返す(例 8 秒 = 8")ため、
	// 内部の "8" をそのまま送ると "Invalid parameter" で拒否される。
	std::string ssToCcapi(const std::string& s)
	{
		if (s.empty())                          { return s; }
		if (s.find('/') != std::string::npos)   { return s; }	// 1/4000 等はそのまま
		if (s == "Bulb" || s == "bulb")         { return "bulb"; }
		std::string t = s;
		auto dot = t.find('.');
		if (dot != std::string::npos) { t[dot] = '"'; return t; }	// 0.5 -> 0"5 / 3.2 -> 3"2
		return t + "\"";											// 8 -> 8" / 30 -> 30"
	}

	std::string ssFromCcapi(const std::string& s)
	{
		if (s.find('/') != std::string::npos)   { return s; }
		if (s == "bulb")                        { return "Bulb"; }
		auto q = s.find('"');
		if (q == std::string::npos)             { return s; }
		std::string t = s;
		t[q] = '.';											// 0"5 -> 0.5 / 8" -> 8.
		if (!t.empty() && t.back() == '.') { t.pop_back(); }	// 整数秒 8. -> 8
		return t;
	}

	// F値: EOS R10 の av ability は f/10 未満の整数開放値を ".0" 付きで返す(例 f/2 = "2.0")。
	// 内部表記が整数("2")でも CCAPI が受け付けるよう、10未満の整数には ".0" を補う(フォールバック用)。
	std::string fnToCcapi(const std::string& s)
	{
		if (s.empty() || s.find('.') != std::string::npos) { return s; }	// 既に小数 or 空はそのまま
		bool allDigit = true;
		for (char ch : s) { if (ch < '0' || ch > '9') { allDigit = false; break; } }
		if (!allDigit) { return s; }
		return (std::stoi(s) < 10) ? (s + ".0") : s;	// 2 -> 2.0 / 16 -> 16
	}

	// av ability の接頭(例 'f')を1文字除いた表示値。"f1.4" -> "1.4"
	std::string fnStrip(const std::string& s) { return s.empty() ? s : s.substr(1); }

	// ability の生文字列 raw を内部表記(表示・計算用)へ正規化する。
	std::string toDisp(const std::string& raw, expo::expoKind k)
	{
		if (k == expo::expoKind::ss) { return ssFromCcapi(raw); }	// 8" -> 8
		if (k == expo::expoKind::fn) { return fnStrip(raw); }		// f1.4 -> 1.4
		return raw;													// iso はそのまま
	}
}


// コンストラクタ
apiCanonCCAPI::apiCanonCCAPI(void) {}

// デストラクタ
apiCanonCCAPI::~apiCanonCCAPI(void) {}


// device についての情報を取得する
// device : カメラの情報。この呼び出し時には location が入っていること。
//          location を読み取りdevice discovery の内容、コマンド一覧を取得する。
// return : ERR_HGC_OK 成功、それ以外:失敗
errCode apiCanonCCAPI::init(class device& device)
{
    // Device Descriptor の内容を取得する
    auto err = getDeviceDescriptor(device);
    if (err != ERR_HGC_OK) { return err; }
    
    // ★ダイジェスト認証が必要。
	// カメラの設定で認証無しにしてあれば今のところ取得できる。	
    std::string catlog;
	auto success = netThread::httpGet(device.urlAccess, catlog);
    if(!success)                { return ERR_HGC_API_LIST; }	
	if (catlog.length() == 0)   { return ERR_HGC_API_LIST; }	

    // 使用する api の path を保存する。
    err = analizeUseFunction(device, catlog);
    this->device = device;
    liveViewInfo.resize(1024*8);
    return err;
}

// 手動初期化。device.urlAccess(例: http://<ip>:8080/ccapi)を呼び出し側で設定済みとし、
// UPnP記述子の取得をスキップしてCCAPIカタログから機能URLを構築する。
// SSDPマルチキャストが使えない環境(エミュレータ等)でIP直指定接続するために使う。
errCode apiCanonCCAPI::initManual(class device& device)
{
    if (device.urlAccess.length() == 0) { return ERR_HGC_NET_URL_FAIL; }

    std::string catlog;
    auto success = netThread::httpGet(device.urlAccess, catlog);
    if (!success)               { return ERR_HGC_API_LIST; }
    if (catlog.length() == 0)   { return ERR_HGC_API_LIST; }

    errCode err = analizeUseFunction(device, catlog);

    // CCAPI の deviceinformation からモデル名/シリアルNo.等を補完する(UPnP記述子の代替)。
    // 失敗しても手動接続自体は続行する(機能取得が済んでいれば撮影は可能)。
    std::string info;
    if (netThread::httpGet(device.urlAccess + "/ver100/deviceinformation", info) && info.length() > 0)
    {
        try {
            auto j = json::parse(info);
            if (j.contains("manufacturer")) { device.manufacturer = j.value("manufacturer", std::string()); }
            if (j.contains("productname"))  { device.model        = j.value("productname", std::string()); }
            if (j.contains("serialnumber")) { device.serialno     = j.value("serialnumber", std::string()); }
        } catch (const std::exception&) { /* deviceinformation 無し/解析失敗は無視 */ }
    }

    this->device = device;
    liveViewInfo.resize(1024 * 8);
    return err;
}

// DeviceDescriptor の内容を取得する。
// device : location が入っていること。
// return : ERR_HGC_OK:成功、それ以外:失敗
errCode apiCanonCCAPI::getDeviceDescriptor(class device& device)
{
    // DeviceDescriptor を取得する。
    std::string deviceDescriptor;
    auto success = netThread::httpGet(device.location, deviceDescriptor);
    if(!success)                        { return ERR_HGC_DEVICE_DESCRIPTOR;}
    if (deviceDescriptor.length() == 0) { return ERR_HGC_DEVICE_DESCRIPTOR; }       // 取得できない
    DBGLN(col::WHT, "Detect Device Descriptor.");

    // device descriptor の内容を取り出す
    device.model = tool::getXmlTagValue(deviceDescriptor, "modelName");
    device.friendName = tool::getXmlTagValue(deviceDescriptor, "friendlyName");
    device.manufacturer = tool::getXmlTagValue(deviceDescriptor, "manufacturer");
    device.urlAccess = tool::getXmlTagValue(deviceDescriptor, "ns:X_accessURL");
    device.urlbase = tool::getXmlTagValue(deviceDescriptor, "URLBase");
    device.serialno = tool::getXmlTagValue(deviceDescriptor, "serialNumber");
    return ERR_HGC_OK;
}

// 使用するコマンドを探す
// コマンド一覧から使用するコマンドとその機能を取得する
// catalog:json形式のコマンド一覧。device.urlAccessから取得する。
errCode apiCanonCCAPI::analizeUseFunction(class device& device, std::string& catalog)
{
    try {
        auto data = json::parse(catalog);

        for (auto& [version, endpoints] : data.items()) 
        {
            for (const auto& entry : endpoints) 
            {
                // path 取得
                std::string path = entry.value("path", "");
                if (path.length() == 0) { continue; }
                class func func;
                for (auto& fncRef : useFunction)
                {   // path で必要な機能を探す
                    if (fncRef.find)                  { continue; }   // すでに見つけている
                    auto ix = path.rfind(fncRef.surfix);
                    if (ix == -1)                   { continue; }   // 違う
                    if (ix + fncRef.surfix.length() != path.length()) { continue; }   // 最後ではない

                    // url にする
                    auto httpIx = path.find("http");
                    if (httpIx != -1) 
                    {   // full path が入ってるのでそのまま使う
                        func.funcNum = fncRef.funcNum;
                        func.url = path;
                        fncRef.find = true;
                        break;
                    }
                    
                    func.funcNum = fncRef.funcNum;

                    // urlAccess とつなぐ
                    // "/"の有無に関わらずつなぐ
                    auto ccapiIxTail = path.find("ccapi");
                    auto ccapiIxHead = device.urlAccess.rfind("ccapi");
                    func.url = device.urlAccess.substr(0, ccapiIxHead + 5) + path.substr(ccapiIxTail + 5);
                    fncRef.find = true;
                    break;
                }
                if (func.funcNum == funcNum::NON) { continue; }

                // 動作を設定
                if (entry.value("get", false))      { func.verb |= verb::GET; }
                if (entry.value("post", false))     { func.verb |= verb::POS; }
                if (entry.value("put", false))      { func.verb |= verb::PUT; }
                if (entry.value("delete", false))   { func.verb |= verb::DEL; }

                funcList[func.funcNum] = func;      // 機能リストに登録
                if (funcList.size() == useFunction.size())     { break; }    // すべて登録された
            }
        }
    }
    catch (const std::exception& e) 
    {
        DBGLN(col::RED,"%s",e);
        return ERR_HGC_API_LIST;
    }
    return ERR_HGC_OK;
}

// 登録された機能を削除する
void apiCanonCCAPI::useFunctionClear(void)
{
    for (auto& func : useFunction)
    {
        func.find = false;
    }
}

// 撮影開始
// live view の開始をおこなう
errCode apiCanonCCAPI::startShooting(void)
{
    if (!(funcList[funcNum::LIVE_SET].verb == verb::POS)) { return ERR_HGC_NOT_SUPPORTED; }

    json request_json;
    request_json["liveviewsize"] = "small";             // live view サイズ small
    request_json["cameradisplay"] = "keep";             // カメラの表示
    std::string body = request_json.dump();             // 文字列に変換
    std::string response;

    auto success = netThread::httpPost(funcList[funcNum::LIVE_SET].url, body, response);

    if (success == false) { return ERR_HGC_HTTP_POST; }
    return ERR_HGC_OK;
}

// シャッターを切る準備
// shotSet : シャッター設定
// return  : ERR_HGC_OK:成功
errCode apiCanonCCAPI::rdyShutter(const cmdt::shotSet& shotSet)
{
    errCode err = ERR_HGC_OK;
    err = setFNumber(shotSet.fNum);
    if (err != ERR_HGC_OK) { return err; }
    err = setSS(shotSet.ss);
    if (err != ERR_HGC_OK) { return err; }
    err = setIso(shotSet.iso);
    if (err != ERR_HGC_OK) { return err; }

    return err;
}
// シャッターを切る
errCode apiCanonCCAPI::actShutter(void)
{
    if (!(funcList[funcNum::SHOT].verb == verb::POS)) { return ERR_HGC_NOT_SUPPORTED; }

    json request_json;
    request_json["af"] = false;             // AFを無効にする
    std::string body = request_json.dump(); // 文字列に変換
    std::string response;

    auto success = netThread::httpPost(funcList[funcNum::SHOT].url, body, response);
    
    if(success == false) { return ERR_HGC_HTTP_POST; }
	return ERR_HGC_OK;
}

// 指定可能な f 値(文字列)を取得する。CCAPI の ability は先頭1文字(接頭)を除いた表示値("1.4","16")。
errCode apiCanonCCAPI::ascCanFNumber(std::vector<std::string>& fNumber)
{
    std::vector<std::string> abiStr;
    errCode err = getJsonAbility(funcNum::F_NUMBER, abiStr);
    if (err != ERR_HGC_OK)          { return err; }

    std::vector<std::string> out;
    for (const auto& a : abiStr)
    {   // 接頭の "f" 等を1文字除いた表示値文字列
        out.push_back(a.empty() ? a : a.substr(1));
    }
    fNumber = out;
    return ERR_HGC_OK;
}

// 指定可能な シャッター速度(文字列)を取得する。
// CCAPI 表記(8" 等)を内部表記("8" 等)へ変換して返す。
errCode apiCanonCCAPI::ascCanSS(std::vector<std::string>& ss)
{
    std::vector<std::string> raw;
    errCode err = getJsonAbility(funcNum::SS, raw);
    if (err != ERR_HGC_OK) { return err; }
    ss.clear();
    for (const auto& v : raw) { ss.push_back(ssFromCcapi(v)); }
    return ERR_HGC_OK;
}

// 指定可能な ISO(文字列)を取得する。CCAPI の値("100","3200"等)をそのまま使う。
errCode apiCanonCCAPI::ascCanIso(std::vector<std::string>& iso)
{
    return getJsonAbility(funcNum::ISO, iso);
}

// 送信用テーブルから real に最も近い「カメラが広告した文字列」を返す。
std::string apiCanonCCAPI::sendFor(const std::vector<sendMap>& map, double real) const
{
    if (map.empty() || real <= 0.0) { return std::string(); }
    const sendMap* best = &map[0];
    double bestDiff = 1e300;
    for (const auto& e : map)
    {
        double d = std::fabs(e.real - real);
        if (d < bestDiff) { bestDiff = d; best = &e; }
    }
    return best->send;
}

// 設定値を取得する。撮影開始時に呼ばれ、表示用(settings=正規化)と
// 送信用テーブル(real→カメラ生文字列)の両方を ability から構築する。
errCode apiCanonCCAPI::getSettings(cmdt::shotRange& settings)
{
    std::vector<std::string> isoRaw, ssRaw, fnRaw;
    errCode err = getJsonAbility(funcNum::ISO, isoRaw);
    if (err != ERR_HGC_OK) { return err; }
    err = getJsonAbility(funcNum::SS, ssRaw);
    if (err != ERR_HGC_OK) { return err; }
    err = getJsonAbility(funcNum::F_NUMBER, fnRaw);
    if (err != ERR_HGC_OK) { return err; }

    // 生文字列 raw から { 表示用 settings, 送信用テーブル } を作る。
    auto build = [](const std::vector<std::string>& raw, expo::expoKind k,
                    std::vector<std::string>& disp, std::vector<sendMap>& send)
    {
        disp.clear(); send.clear();
        for (const auto& r : raw)
        {
            std::string d = toDisp(r, k);
            disp.push_back(d);
            double val = expo::parseValue(d, k);	// auto/Bulb 等は負
            if (val > 0.0) { send.push_back({ r, val }); }	// send=カメラ生文字列, real=実数
        }
    };
    build(isoRaw, expo::expoKind::iso, settings.iso,  isoSend_);
    build(ssRaw,  expo::expoKind::ss,  settings.ss,   ssSend_);
    build(fnRaw,  expo::expoKind::fn,  settings.fNum, fnSend_);

    return ERR_HGC_OK;
}

// f 値を設定する
// fNumber : 値の f/xx.x の xx.x 部
// return  : ERR_HGC_OK:成功、それ以外は失敗
errCode apiCanonCCAPI::setFNumber(const std::string& fNumber)
{
    funcNum func = funcNum::F_NUMBER;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    // 送信用テーブルからカメラ広告値(例 "f1.4")をそのまま使う。未構築時のみ規則変換。
    std::string val = sendFor(fnSend_, expo::parseValue(fNumber, expo::expoKind::fn));
    if (val.empty()) { val = "f" + fnToCcapi(fNumber); }
    json json;
    json["value"] = val;
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "setFNumber NG %s %s", body.c_str(), resp.c_str());
    return ERR_HGC_HTTP_PUT;	// 失敗を握りつぶさず返す
}

// シャッター速度を設定する。送信用テーブルからカメラ広告値(例 8")をそのまま送る。
errCode apiCanonCCAPI::setSS(const std::string& ss)
{
    funcNum func = funcNum::SS;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string val = sendFor(ssSend_, expo::parseValue(ss, expo::expoKind::ss));
    if (val.empty()) { val = ssToCcapi(ss); }	// 未構築時のみ規則変換(フォールバック)
    json json;
    json["value"] = val;
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "setSS NG %s %s", body.c_str(), resp.c_str());
    return ERR_HGC_HTTP_PUT;	// 失敗を握りつぶさず返す
}

// ISO を設定する。送信用テーブルからカメラ広告値をそのまま送る。
errCode apiCanonCCAPI::setIso(const std::string& iso)
{
    funcNum func = funcNum::ISO;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string val = sendFor(isoSend_, expo::parseValue(iso, expo::expoKind::iso));
    if (val.empty()) { val = iso; }	// 未構築時はそのまま
    json json;
    json["value"] = val;
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "setIso NG %s %s", body.c_str(), resp.c_str());
    return ERR_HGC_HTTP_PUT;	// 失敗を握りつぶさず返す
}

// 撮影した画像を取得する。
// jpg    : 撮影した画像データ
// return : ERR_HGC_OK:成功
errCode apiCanonCCAPI::getShotPicture(std::vector<std::byte>& jpg)
{
    auto tmStart = tool::startElapse();

#if 0
    std::vector<strageInfo> info;
    errCode err = getStrageSta(info);
    if (err != ERR_HGC_OK) { return err; };
    for (const auto& ifo : info)
    {
        DBGLN(col::CYN, "access       %s", ifo.acce.c_str());
        DBGLN(col::CYN, "contents num %u", ifo.conN);
        DBGLN(col::CYN, "name         %s", ifo.name.c_str());
        DBGLN(col::CYN, "path         %s", ifo.path.c_str());
    }
#endif

    // アクティブなディレクトリを取得
    std::string dirPath;
    errCode err = getDirAct(dirPath);
    if (err != ERR_HGC_OK) { return err; };
    DBGLN(col::CYN, "dir          %s", dirPath.c_str());
    DBGLN(col::CYN, "%5ums", tool::getElapse(tmStart));

    // ファイル一覧を取得
    // '/'の有無に関わらずつながるようにする
    auto ccapiIxTail = dirPath.find("ccapi");
    auto ccapiIxHead = device.urlAccess.rfind("ccapi");
    auto fullPath = device.urlAccess.substr(0, ccapiIxHead+5) + dirPath.substr(ccapiIxTail + 5);
    std::string lastFile;
    err = getLastFile(fullPath, lastFile);
    DBGLN(col::YEL, "%s", lastFile.c_str());
    DBGLN(col::CYN, "%5ums", tool::getElapse(tmStart));
    return ERR_HGC_OK;
}


// 機能番号の ability を取得する
// nunber  : 機能番号
// ability : 取得した ability
errCode apiCanonCCAPI::getJsonAbility(funcNum number, std::vector<std::string>& abilitys)
{
    if (!(funcList[number].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string answer;
    auto success = netThread::httpGet(funcList[number].url, answer);
    if(!success){ return ERR_HGC_HTTP_GET;}
    try
    {
        auto key = "ability";
        auto json = json::parse(answer);
        if (!json.contains(key)) { return ERR_HGC_TAKE_FAIL; }
        auto takeAbi = json.at(key).get<std::vector<std::string>>();
        if (takeAbi.size() == 0) { return ERR_HGC_TAKE_FAIL; }
        abilitys = takeAbi;
    }
    catch(json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        return ERR_HGC_API_ANALIZE;
    }
    
    return ERR_HGC_OK;
}

// 機能番号の value に実数を設定する
// number : 機能番号
// val    : 設定する値
// return : ERR_HGC_OK:成功、それ以外は失敗
errCode apiCanonCCAPI::setJsonvalue(funcNum number, float val)
{
    if (!(funcList[number].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    json json;
    if (number == funcNum::F_NUMBER)
    {
        char buff[16];
        snprintf(buff, sizeof(buff), "f%1.1f", val);
        json["value"] = buff;

    }
    else
    {
        json["value"] = "f " + std::to_string(val);
    }
    std::string value = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[number].url, value, resp)) { return ERR_HGC_OK;}

    DBGLN(col::RED, "%s", resp.c_str());

    return ERR_HGC_OK;

}


// 保存先のストレージ情報を取得する
errCode apiCanonCCAPI::getStrageAct(std::string& path)
{
    if (!(funcList[funcNum::STRAGE_ACT].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string  answer;
    auto success = netThread::httpGet(funcList[funcNum::STRAGE_ACT].url, answer);
    if(!success)    { return ERR_HGC_STRAGE_INF;   }
    try
    {
        auto json = json::parse(answer);
        std::string key = "path";

        // "patht" キーが存在するか
        if (!json.contains(key))
        {
            DBGLN(col::RED, "%s", answer.c_str());
            return ERR_HGC_API_ANALIZE;
        }
        path = json.at(key).get<std::string>(); 

    }
    catch (json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        return ERR_HGC_API_ANALIZE;
    }
    return ERR_HGC_OK;
}

// ストレージ情報を取得する
errCode apiCanonCCAPI::getStrageSta(std::vector<strageInfo>& strageInfo)
{
    if (!(funcList[funcNum::STRAGE_STA].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string answer;
    auto success = netThread::httpGet(funcList[funcNum::STRAGE_STA].url, answer);
    if(!success)                { return ERR_HGC_STRAGE_INF;}
    try
    {
        auto json = json::parse(answer);
        std::string storageList = "storagelist";

        // "storage list" キーが存在するか、かつ配列かを確認
        if (!json.contains(storageList) || !json[storageList].is_array()) 
        {
            DBGLN(col::RED, "%s", answer.c_str());
            return ERR_HGC_API_ANALIZE;
        }

        for (const auto& item : json[storageList])
        {
            struct strageInfo info;
            auto name = "name";
            auto path = "path";
            auto acce = "accesscapability";
            auto maxS = "maxsize";
            auto spaS = "spacesize";
            auto conN = "contentsnumber";

//            auto json = json::parse(item);
            if (item.contains(name)) { info.name = item.at(name).get<std::string>(); }
            if (item.contains(path)) { info.path = item.at(path).get<std::string>(); }
            if (item.contains(acce)) { info.acce = item.at(acce).get<std::string>(); }
            if (item.contains(maxS)) { info.maxS = item.at(maxS).get<uint64_t>(); }
            if (item.contains(spaS)) { info.spaS = item.at(spaS).get<uint64_t>(); }
            if (item.contains(conN)) { info.conN = item.at(conN).get<uint64_t>(); }
            strageInfo.push_back(info);
        }
    }
    catch (json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        return ERR_HGC_API_ANALIZE;
    }
    return ERR_HGC_OK;
}

// 保存先のディレクトリ情報を取得する
errCode apiCanonCCAPI::getDirAct(std::string& path)
{
    if (!(funcList[funcNum::DIR_ACT].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string answer;
    auto success = netThread::httpGet(funcList[funcNum::DIR_ACT].url, answer);
    if(!success)                    {return ERR_HGC_DIR_ACT;}
    try
    {
        auto json = json::parse(answer);
        std::string key = "path";

        // "patht" キーが存在するか
        if (!json.contains(key))
        {
            DBGLN(col::RED, "%s", answer.c_str());
            return ERR_HGC_API_ANALIZE;
        }
        path = json.at(key).get<std::string>();

    }
    catch (json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        return ERR_HGC_API_ANALIZE;
    }
    return ERR_HGC_OK;
}

// ファイルのリストを取得する
// kind=chunked にすると 100 件ごとの塊が連なって送られてくる
// kind=list だと 100 件の配列
// order=desc でディセンディング。ファイルの後ろから。このパラメータを付けるとエラーになる。
// type=all はファイル形式。すべて。
// page=1 は 100 件のかたまりの何番目か
errCode apiCanonCCAPI::getLastFile(std::string& path, std::string& file)
{
    if (!(funcList[funcNum::L_FILE].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }

    try
    {
        std::vector<std::string> fileList;

        // まず ページ数を取得する
        std::string req = path + "?type=all&kind=number";
        std::string answer;
        auto success = netThread::httpGet(req, answer);
        if(!success)                {return ERR_HGC_LAST_FILE;}
        if (answer.length() == 0) { return ERR_HGC_API_ANALIZE; }
        auto json = json::parse(answer);
        std::string key = "contentsnumber";
        if (!json.contains(key)) { return ERR_HGC_API_ANALIZE; }
        auto number = json.at(key).get<uint32_t>();
        key = "pagenumber";
        if (!json.contains(key)) { return ERR_HGC_API_ANALIZE; }
        auto pagenumber = json.at(key).get<uint32_t>();


        // 最後のページのファイルリストを取得する
        req = path + "?type=all&kind=list" + "&page=" + std::to_string(pagenumber);
        //        DBGLN(col::RED, "%s", req.c_str());
        success = netThread::httpGet(req, answer);
        if(!success)                {return ERR_HGC_LAST_FILE;}
        //        DBGLN(col::RED, "%s", answer.c_str());
        if (answer.length() == 0) { return ERR_HGC_API_ANALIZE; }
        json = json::parse(answer);
        key = "url";

        if (!json.contains(key) || !json[key].is_array())
        {   // version によって "url" の場合と "path" の場合がある
            key = "path";
            if (!json.contains(key) || !json[key].is_array())
            {
                DBGLN(col::RED, "%s", answer);
                return ERR_HGC_API_ANALIZE;
            }
        }
        fileList = json.at(key).get<std::vector<std::string>>();
        if (fileList.size() == 0) { return ERR_HGC_NO_ELEMENT; }
        file = fileList.back();             // 最後のファイル名(path)を取得
    }
    catch (json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        return ERR_HGC_API_ANALIZE;
    }
    return ERR_HGC_OK;
}

// 測光の準備をする。
// タイムラプス撮影のショットの合間のわずかな時間を使ってデータを集めるため rdyMetering()
//  は最小限の機能だけを実行する。
// rdyMetering() で露光に関する一次情報を作成する。
// alzMetering()でその一次情報を使用して解析する。
// rdyMetering() 実行後 alzMetering() 呼び出しまでの間に再度の rdyMetering() 実行をおこな
// うと一次情報は上書きされる。
// 
// meteringParm : alzMetering に渡す一次情報
// return       : ERR_HCG_OK:成功
errCode apiCanonCCAPI::rdyMetering(void)
{
    auto elapse = tool::startElapse();
    if (!(funcList[funcNum::LIVE_DETAIL].verb == verb::GET)) { return ERR_HGC_NOT_SUPPORTED; }
    std::string req = funcList[funcNum::LIVE_DETAIL].url + "?kind=info";
//    std::string req = funcList[funcNum::LIVE_DETAIL].url + "?kind=image";
    bool success = netThread::httpGet(req, liveViewInfo);
    if(!success)    {return ERR_HGC_RDY_METARING;}
    DBGLN(col::YEL,"%s:%ums",__func__,  tool::getElapse(elapse));
    return ERR_HGC_OK;

}

// 測光解析
// rdyMetering()で得た情報を元に最適化されたヒストグラムを生成する
// meteringParm : rdyMetering()で取得した情報
// hist         : 最適化されたヒストグラム
// return       : ERR_HCG_OK:成功
errCode apiCanonCCAPI::alzMetering(cmdt::HISTOGRAM& histoOut)
{
    auto ela = tool::startElapse();
    // live view 付帯情報から histogram を取り出す
    if (liveViewInfo.length() == 0) { return ERR_HGC_API_ANALIZE; }
    try
    {
        if (    (static_cast<uint8_t>(liveViewInfo[0]) != 0xff)
            ||  (static_cast<uint8_t>(liveViewInfo[1]) != 0x00)
            ||  (static_cast<uint8_t>(liveViewInfo[2]) != 0x01))
        {   // live view 付帯情報ではない
            DBGLN(col::YEL,"len:%u",liveViewInfo.length());
            DUMP(0, liveViewInfo.c_str(),32);
//            DBGLN(col::WHT,"Free heap(%u)", ESP.getFreeHeap());
            return ERR_HGC_NOT_LIVE_DETAIL;
        }
        uint32_t len =  (static_cast<uint8_t>(liveViewInfo[3]) << (8 * 3))
                      + (static_cast<uint8_t>(liveViewInfo[4]) << (8 * 2))
                      + (static_cast<uint8_t>(liveViewInfo[5]) << (8 * 1))
                      + (static_cast<uint8_t>(liveViewInfo[6]) << (8 * 0));
        DBGLN(col::CYN, "length(%u)", len);

        if(len > (liveViewInfo.length() -9))
        {
            DBGLN(col::RED, "len(%u) histogramRaw(%u)",len, liveViewInfo.length());
            return ERR_HGC_NOT_LIVE_FORMAT;
        }
        auto json = json::parse(liveViewInfo.begin()+7, liveViewInfo.end()-2);
//        auto json = json::parse(liveViewInfo.begin()+7,answer.end()-2);
        std::string key0 = "liveviewdata";
        std::string key1 = "histogram";
        if (!json.contains(key0))
        {
            DBGLN(col::RED, "parse error");
            return ERR_HGC_API_ANALIZE;
        }
        std::vector<std::vector<uint32_t>> histoRaw;
        histoRaw = json[key0][key1];
        DBGLN(col::CYN,"%s:elapse(%ums) json parse.", __func__, tool::getElapse(ela));

        // yrgb の要素があること
        // yrgb の bin の数が一緒であること
        // bin の数は 256 の整数倍であること
        if(histoRaw.size() != 4)            {return ERR_HGC_HISTO_ELEMENT;}
        if( (histoRaw[0].size() != histoRaw[1].size()) || 
            (histoRaw[0].size() != histoRaw[2].size()) || 
            (histoRaw[0].size() != histoRaw[3].size()))
                                            {return ERR_HGC_HISTO_BIN;}
        uint32_t binCon = static_cast<uint32_t>(histoRaw[0].size()) / 256;     // bin をまとめる数
        if((histoRaw[0].size() % 256) !=0)  {return ERR_HGC_HISTO_BIN;}

        // ヒストグラムを最適化する
        // 全体の要素数を求める。
        // 全体の要素数が 65535 になるように各 bin の数を調整する
        uint64_t totalElem = 0;
        for(const auto& bin:histoRaw[0]) { totalElem += bin; }  //　全体の
        for(uint16_t yrgb = 0; yrgb < histoRaw.size() ;yrgb++)
        {
            for(uint16_t bin = 0;bin < 256; bin++)
            {
                for(uint32_t con = 0; con < binCon; con++)
                {
                    histoOut.raw[yrgb][bin] = (uint16_t)((uint64_t)histoRaw[yrgb][bin*binCon+con] * 65535LLU / totalElem);
                }
            }

        }
        DBGLN(col::CYN,"%s:elapse(%ums) histo gram.", __func__, tool::getElapse(ela));
    }
    catch (json::exception& e)
    {
        DBGLN(col::RED, "%s:%s", __func__, e.what());
        DUMP(0, liveViewInfo.c_str(),32);
        DBGLN(col::WHT, "%s",liveViewInfo.substr(0,32).c_str());
        return ERR_HGC_API_ANALIZE;
    }
    return ERR_HGC_OK;
}
