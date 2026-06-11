#include "common.h"
#include "apiCanonCCAPI.h"
#include "netThread.h"
#include <json/nlohmann/json.hpp>

using json = nlohmann::json;


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

// 指定可能な シャッター速度(文字列)を取得する。CCAPI の値("1/4000","8"等)をそのまま使う。
errCode apiCanonCCAPI::ascCanSS(std::vector<std::string>& ss)
{
    return getJsonAbility(funcNum::SS, ss);
}

// 指定可能な ISO(文字列)を取得する。CCAPI の値("100","3200"等)をそのまま使う。
errCode apiCanonCCAPI::ascCanIso(std::vector<std::string>& iso)
{
    return getJsonAbility(funcNum::ISO, iso);
}

// 設定値を取得する
// settings : 設定値保存場所
errCode apiCanonCCAPI::getSettings(cmdt::shotRange& settings)
{
    std::vector<std::string> iso, ss, fNum;

    auto err = ascCanIso(iso);
    if (err != ERR_HGC_OK) { return err; }
    err = ascCanSS(ss);
    if (err != ERR_HGC_OK) { return err; }
    err = ascCanFNumber(fNum);
    if (err != ERR_HGC_OK) { return err; }

    settings.fNum = fNum;
    settings.ss   = ss;
    settings.iso  = iso;

    return ERR_HGC_OK;
}

// f 値を設定する
// fNumber : 値の f/xx.x の xx.x 部
// return  : ERR_HGC_OK:成功、それ以外は失敗
errCode apiCanonCCAPI::setFNumber(const std::string& fNumber)
{
    funcNum func = funcNum::F_NUMBER;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    json json;
    json["value"] = "f" + fNumber;	// 表示値("1.4","16")に接頭 'f' を付けてカメラへ
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "%s %s", body.c_str(), resp.c_str());
    return ERR_HGC_OK;
}

// シャッター速度を設定する(カメラ設定値の文字列をそのまま指示)
errCode apiCanonCCAPI::setSS(const std::string& ss)
{
    funcNum func = funcNum::SS;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    json json;
    json["value"] = ss;
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "%s %s", body.c_str(), resp.c_str());
    return ERR_HGC_OK;
}

// ISO を設定する(カメラ設定値の文字列をそのまま指示)
errCode apiCanonCCAPI::setIso(const std::string& iso)
{
    funcNum func = funcNum::ISO;
    if (!(funcList[func].verb == verb::PUT)) { return ERR_HGC_NOT_SUPPORTED; }
    json json;
    json["value"] = iso;
    std::string body = json.dump();
    std::string resp;
    if (netThread::httpPut(funcList[func].url, body, resp)) { return ERR_HGC_OK; }

    DBGLN(col::RED, "%s %s", body.c_str(), resp.c_str());
    return ERR_HGC_OK;
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
