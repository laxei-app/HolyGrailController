#include "common.h"
#include "apiCanonCCAPI.h"
#include "netThread.h"
#include "exposureMath.h"
#include "jpegLuma.h"	// 撮影画像サムネイルの輝度ヒストグラム化(方式A測光)
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

    std::string response;
    // cameradisplay は撮影自体に影響しない(カメラ背面表示の扱い)が、受理値が機種で異なる。
    // 例: EOS R10 は "keep" を受理、EOS R100 は "Invalid parameter"(HTTP 400)で拒否。
    // 機種名で分岐せず候補を順に試し、最初に成功(HTTP 2xx)した値を使う(将来機種にも実応答で追従)。
    // 記憶はしない(毎回試す)。撮影に影響しない部分なので順次試行のコストは許容。
    const char* displays[] = { "keep", "on", "off" };
    for (const char* disp : displays)
    {
        json request_json;
        request_json["liveviewsize"] = "small";         // live view サイズ small(両機種で受理)
        request_json["cameradisplay"] = disp;           // カメラ背面表示(keep→on→off 順次フォールバック)
        if (netThread::httpPost(funcList[funcNum::LIVE_SET].url, request_json.dump(), response))
        {
            return ERR_HGC_OK;
        }
    }
    return ERR_HGC_HTTP_POST;
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

    // 測光用のAPEX換算テーブルも自前で構築する(2026-07-27 setExpoTables廃止)。
    // 設定可能値の中身も表記もカメラ依存なので、この層が ability から作るのが自然な置き場。
    tables_.iso = expo::buildTable(settings.iso,  expo::expoKind::iso);
    tables_.ss  = expo::buildTable(settings.ss,   expo::expoKind::ss);
    tables_.fn  = expo::buildTable(settings.fNum, expo::expoKind::fn);

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

// funcList に登録済み(カタログに存在)で指定 verb を持つか。
bool apiCanonCCAPI::hasFunc(funcNum n, verb::type v)
{
    auto it = funcList.find(n);
    return it != funcList.end() && it->second.verb == v;
}

// 撮影モードをマニュアル(M)にする。元の値を保存し restoreShootingMode で戻す(仕様8/CCAPI 4.8.3/4.9)。
//  ダイアル搭載機: ignoreshootingmodedialmode を on にしてから shootingmodedial="m"。
//  ダイアル非搭載機: shootingmode="m" を直接 PUT。
//  どちらの API も無い機種は NOT_SUPPORTED(致命的ではない。呼び出し側で握る)。
errCode apiCanonCCAPI::setupShootingModeManual(void)
{
    shootModeChanged_ = false;
    savedShootMode_.clear();

    // --- オートパワーオフ抑止(撮影中は disable) ---
    // CCAPIはステートレスHTTPで持続セッションを握らない。撮影開始〜撮影窓までの待機(captureRunner
    // の now<startSec ループ)や「撮影周期 > autopoweroff秒」のコマ間は無通信になり、カメラが
    // オートパワーオフ(スリープ)し得る。これを防ぐため開始時に disable へ。元値は restoreShootingMode で戻す。
    // ここ(setup段=待機ループより前)で行うことで待機中・コマ間の両ギャップをカバーする。
    autoPowerOffChanged_ = false;
    savedAutoPowerOff_.clear();
    if (hasFunc(funcNum::AUTOPOWEROFF, verb::PUT))
    {
        std::string ans;
        if (netThread::httpGet(funcList[funcNum::AUTOPOWEROFF].url, ans))
        {
            try { savedAutoPowerOff_ = json::parse(ans).value("value", std::string()); } catch (...) {}
        }
        if (savedAutoPowerOff_ != "disable")	// 既に disable なら何もしない(restore不要)
        {
            json b; b["value"] = "disable"; std::string resp;
            if (netThread::httpPut(funcList[funcNum::AUTOPOWEROFF].url, b.dump(), resp)) { autoPowerOffChanged_ = true; }
            else { DBGLN(col::RED, "autopoweroff disable NG %s", resp.c_str()); }
        }
    }

    // --- ダイアル搭載機 ---
    if (hasFunc(funcNum::SHOOTMODE_DIAL, verb::PUT))
    {
        std::string ans;
        if (netThread::httpGet(funcList[funcNum::SHOOTMODE_DIAL].url, ans))
        {
            try { savedShootMode_ = json::parse(ans).value("value", std::string()); } catch (...) {}
        }
        // ダイアル無視モード ON(これをしないと shootingmodedial の PUT は 503 になる)
        if (hasFunc(funcNum::IGNORE_DIAL, verb::POS))
        {
            json b; b["action"] = "on"; std::string resp;
            netThread::httpPost(funcList[funcNum::IGNORE_DIAL].url, b.dump(), resp);
        }
        if (savedShootMode_ == "m") { shootModeChanged_ = true; savedIsDial_ = true; return ERR_HGC_OK; } // 既にM
        json b; b["value"] = "m"; std::string resp;
        if (!netThread::httpPut(funcList[funcNum::SHOOTMODE_DIAL].url, b.dump(), resp))
        {
            DBGLN(col::RED, "setMode(dial) NG %s", resp.c_str());
            return ERR_HGC_HTTP_PUT;
        }
        savedIsDial_ = true; shootModeChanged_ = true;
        return ERR_HGC_OK;
    }

    // --- ダイアル非搭載機 ---
    if (hasFunc(funcNum::SHOOTMODE, verb::PUT))
    {
        std::string ans;
        if (netThread::httpGet(funcList[funcNum::SHOOTMODE].url, ans))
        {
            try { savedShootMode_ = json::parse(ans).value("value", std::string()); } catch (...) {}
        }
        if (savedShootMode_ == "m") { shootModeChanged_ = true; savedIsDial_ = false; return ERR_HGC_OK; }
        json b; b["value"] = "m"; std::string resp;
        if (!netThread::httpPut(funcList[funcNum::SHOOTMODE].url, b.dump(), resp))
        {
            DBGLN(col::RED, "setMode NG %s", resp.c_str());
            return ERR_HGC_HTTP_PUT;
        }
        savedIsDial_ = false; shootModeChanged_ = true;
        return ERR_HGC_OK;
    }

    return ERR_HGC_NOT_SUPPORTED;	// モード変更APIを持たない機種
}

// setupShootingModeManual で変更した撮影モードを元に戻す。
errCode apiCanonCCAPI::restoreShootingMode(void)
{
    errCode rc = ERR_HGC_OK;

    // 1) 変更した撮影モード値を元へ戻す(保存値が有効で M でないとき)。
    if (shootModeChanged_)
    {
        funcNum fn = savedIsDial_ ? funcNum::SHOOTMODE_DIAL : funcNum::SHOOTMODE;
        if (!savedShootMode_.empty() && savedShootMode_ != "m" && hasFunc(fn, verb::PUT))
        {
            json b; b["value"] = savedShootMode_; std::string resp;
            if (!netThread::httpPut(funcList[fn].url, b.dump(), resp)) { rc = ERR_HGC_HTTP_PUT; }
        }
        shootModeChanged_ = false;
    }

    // 2) ダイアル無視モードは撮影開始で ON にしているので、終了時は必ず OFF へ戻す。
    //    フラグ状態に依らず確実に解除する(ダイアルが効かないまま残るのを防ぐ)。失敗時は一度だけ再試行。
    if (hasFunc(funcNum::IGNORE_DIAL, verb::POS))
    {
        json b; b["action"] = "off"; std::string resp;
        if (!netThread::httpPost(funcList[funcNum::IGNORE_DIAL].url, b.dump(), resp))
        {
            std::string resp2; netThread::httpPost(funcList[funcNum::IGNORE_DIAL].url, b.dump(), resp2);
        }
    }

    // 3) オートパワーオフを元の値へ戻す(撮影開始で disable にしていた場合のみ)。
    if (autoPowerOffChanged_)
    {
        if (!savedAutoPowerOff_.empty() && savedAutoPowerOff_ != "disable" && hasFunc(funcNum::AUTOPOWEROFF, verb::PUT))
        {
            json b; b["value"] = savedAutoPowerOff_; std::string resp;
            if (!netThread::httpPut(funcList[funcNum::AUTOPOWEROFF].url, b.dump(), resp)) { rc = ERR_HGC_HTTP_PUT; }
        }
        autoPowerOffChanged_ = false;
    }
    return rc;
}

// 接続維持用の無害なGET。/ccapi カタログを1回取得するだけ。撮影窓まで待機中などに定期送出して
// 無通信でカメラのWi-Fi/CCAPIセッションが切れるのを防ぐ。到達できれば ERR_HGC_OK。
errCode apiCanonCCAPI::keepAlive(void)
{
    std::string resp;
    if (netThread::httpGet(device.urlAccess, resp) && resp.length() > 0) { return ERR_HGC_OK; }
    return ERR_HGC_HTTP_GET;
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

// ③測光メモリ削減: liveviewdata.histogram を nlohmann の SAX で直接 histoRaw へ抽出する。
// 従来の json::parse は 14KB の JSON を DOM 化して多数の小確保を内部DRAMに積み、2カメラ同時で
// 内部が瞬間枯渇(minFree)する主因だった。SAX は DOM を作らずヒストグラム値だけを拾うので
// 内部DRAM をほぼ消費しない。抽出対象: {"liveviewdata":{"histogram":[[y...],[r...],[g...],[b...]]}}。
namespace
{
	struct HistoSax
	{
		using number_integer_t  = json::number_integer_t;
		using number_unsigned_t = json::number_unsigned_t;
		using number_float_t    = json::number_float_t;
		using string_t          = json::string_t;
		using binary_t          = json::binary_t;

		std::vector<std::vector<uint32_t>>& out;
		std::string curKey;
		bool inLiveview = false;
		bool inHisto    = false;
		bool inSysTime  = false;
		int  depth      = 0;
		// liveviewdata.systemtime = このライブビューフレームをカメラが取得した時刻。
		// これを見れば「露光後に撮られた新鮮なフレームか / 露光前の古いフレームか」が
		// 推測なしに一意に分かる(sec=Unix時刻相当・subsec=ミリ秒。実測で確認済み)。
		uint32_t sysSec = 0, sysSubsec = 0;

		explicit HistoSax(std::vector<std::vector<uint32_t>>& o) : out(o) {}

		bool key(string_t& v)          { curKey = v; return true; }
		bool start_object(std::size_t)
		{
			if (curKey == "liveviewdata")            { inLiveview = true; }
			else if (inLiveview && curKey == "systemtime") { inSysTime = true; }
			curKey.clear();
			return true;
		}
		bool end_object()              { if (inSysTime) { inSysTime = false; } return true; }
		bool start_array(std::size_t)
		{
			if (inHisto)                                       { ++depth; if (depth == 2) { out.emplace_back(); } }
			else if (inLiveview && curKey == "histogram")      { inHisto = true; depth = 1; }
			curKey.clear();
			return true;
		}
		bool end_array()               { if (inHisto) { --depth; if (depth == 0) { inHisto = false; } } return true; }
		void takeNum(uint64_t v)
		{
			if (inHisto && depth == 2)  { out.back().push_back(static_cast<uint32_t>(v)); }
			else if (inSysTime)
			{
				if      (curKey == "sec")    { sysSec    = static_cast<uint32_t>(v); }
				else if (curKey == "subsec") { sysSubsec = static_cast<uint32_t>(v); }
			}
		}
		bool number_unsigned(number_unsigned_t v) { takeNum(static_cast<uint64_t>(v)); return true; }
		bool number_integer(number_integer_t v)   { takeNum(static_cast<uint64_t>(v)); return true; }
		bool number_float(number_float_t, const string_t&) { return true; }
		bool boolean(bool)             { return true; }
		bool null()                    { return true; }
		bool string(string_t&)         { return true; }
		bool binary(binary_t&)         { return true; }
		bool parse_error(std::size_t, const std::string&, const json::exception&) { return false; }
	};
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
        // ③ SAX抽出: DOMを作らず histogram の 4×bin を直接取り出す(内部DRAMの測光スパイクを消す)。
        std::vector<std::vector<uint32_t>> histoRaw;
        HistoSax sax(histoRaw);
        bool ok = json::sax_parse(liveViewInfo.begin()+7, liveViewInfo.end()-2, &sax);
        if (!ok || histoRaw.empty())
        {
            DBGLN(col::RED, "parse error(sax)");
            return ERR_HGC_API_ANALIZE;
        }
        // このフレームをカメラが取得した時刻(ミリ秒)。呼び出し側が「露光後の新鮮なフレームか」を
        // 判定するのに使う。sec はカメラのローカル時刻をUnix時刻として持つ(スマホとは一定のTZ差)。
        lvSysTimeMs_ = static_cast<uint64_t>(sax.sysSec) * 1000ULL + sax.sysSubsec;
        DBGLN(col::CYN,"%s:elapse(%ums) sax parse.", __func__, tool::getElapse(ela));

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

// ============================================================================
//  測光(apiBase::meterScene / meterHere の CCAPI 実装)
//  2026-07-27 captureRunner から移設。「測光してリニア輝度(場面の明るさ)を得る」機能の
//  実装詳細(LVヒストグラム・暗所での測光ss切替・張り付き検出)はカメラ依存なのでこの層に置く。
//  別方式(撮影画像サムネイル等)への差し替えはこの2関数の実装交換で行う。
// ============================================================================
namespace
{
	// --- 測光の調整定数(実測から決定。captureRunner から移設) ---
	constexpr int    kMeterSettleMaxMs    = 2600;	// Tv変更がLVに反映されるまでの待ち上限[ms](実測R10=1.3〜2.1秒)
	constexpr double kMeterUsableLoX      = 0.020;	// 中央値がこれ未満は暗すぎて信用しない(sRGB)
	constexpr double kMeterUsableHiX      = 0.850;	// これ超は明るすぎ(飽和寄り)
	constexpr double kMeterRespondRatio   = 0.50;	// 「Δss段」に対しΔ測光段がこの比未満なら張り付き
	constexpr int    kMeterInitDropStops  = 5;		// 初回の測光ss=撮影ssから何段短くするか
	constexpr int    kMeterRetryMs        = 100;	// ヒスト取得リトライ間隔[ms]
	constexpr int    kMeterMaxMs          = 5000;	// ヒスト取得リトライ上限[ms]
	constexpr int    kLvFreshMarginMs     = 2000;	// 古いLVフレーム判定の許容[ms](生成周期+揺らぎ)
	constexpr double kMeterPinBackoffStops = 1.0;	// 張り付き検出時、天井をこの段数だけ短く下げる
	constexpr double kMeterMaxLenStep      = 1.0;	// 暗すぎるとき1コマで伸ばす上限[段](pin突入を防ぐ)
	constexpr double kMeterCeilRelaxStops  = 0.10;	// 天井を毎コマこれだけ緩め、条件変化へ追従
	// --- 撮影画像フィードバック測光(方式A)の予算 ---
	// 露光終了→カメラの記録完了→サムネイル取得 までを含む総予算。記録は実測2.0〜2.6秒だが、
	// カメラが混んでいると伸びる。取得が503等で弾かれても諦めず、この予算内でリトライする
	// (2026-07-28: 1回で諦めていたため測光失敗→誤って接続断と判定していた)。
	constexpr int    kThumbBudgetMs        = 10000;	// 測光全体(待ち+取得リトライ)の上限[ms]
	constexpr int    kThumbFetchRetryMs    = 200;	// サムネイル取得のリトライ間隔[ms]
}

void apiCanonCCAPI::meterSleep(int ms, const std::function<bool()>& keepGoing) const
{
	void* t0 = tool::startElapse();
	while (static_cast<int>(tool::getElapse(t0)) < ms)
	{
		if (keepGoing && !keepGoing()) { return; }
		tool::sleep(50);
	}
}

void apiCanonCCAPI::meterReset(void)
{
	meterSs_.clear();
	meterCeilStops_ = 1e9;
	meterPrevStops_ = 0.0;
	meterPrevLin_   = -1.0;
	// 再接続でLVセッションが作り直されるため鮮度基準も捨てる(前セッションと比べると誤判定する)。
	lvFreshPrevMs_  = 0;
	lvFreshPrevAt_  = nullptr;
}

// カメラの現在の露出のまま、LVヒストグラムからリニア輝度(中央値)を得る。
//  古いフレームは捨てて再取得し、上限まで粘る(長秒露光後はLVが使えるまで実測3.3秒かかる機種がある)。
errCode apiCanonCCAPI::meterHere(meterResult& out, const std::function<bool()>& keepGoing)
{
	void* t0 = tool::startElapse();
	out = meterResult{};
	cmdt::HISTOGRAM hist;
	for (;;)
	{
		++out.tries;
		void* mt = tool::startElapse();
		const bool got = (rdyMetering() == ERR_HGC_OK) && (alzMetering(hist) == ERR_HGC_OK);
		out.rdyMs = static_cast<int>(tool::getElapse(mt));
		if (got)
		{
			// 鮮度判定: LVフレーム時刻の進みが実経過に足りなければ古い映像(採用せず再取得)。
			const uint64_t lv = lvSysTimeMs_;
			if (lv != 0 && lvFreshPrevMs_ != 0 && lvFreshPrevAt_ != nullptr && lv > lvFreshPrevMs_)
			{
				const long long adv  = static_cast<long long>(lv - lvFreshPrevMs_);
				const long long wall = static_cast<long long>(tool::getElapse(lvFreshPrevAt_));
				if (adv < wall - static_cast<long long>(kLvFreshMarginMs))
				{
					++out.staleSkip;
					if (keepGoing && !keepGoing()) { return ERR_HGC_RDY_METARING; }
					if (static_cast<int>(tool::getElapse(t0)) >= kMeterMaxMs) { return ERR_HGC_RDY_METARING; }
					meterSleep(kMeterRetryMs, keepGoing);
					continue;
				}
			}
			if (lv != 0) { lvFreshPrevMs_ = lv; lvFreshPrevAt_ = tool::startElapse(); }
			// チェックサム+明側診断(p99/pMax)。
			uint32_t s = 0; double total = 0.0;
			for (int i = 0; i < cmdt::hist_bin; ++i) { s = s * 31u + hist.y[i]; total += hist.y[i]; }
			out.histSum = s;
			if (total > 0.0)
			{
				const double thr = total * 0.99; double cum = 0.0; int p99i = cmdt::hist_bin - 1, pmax = 0;
				for (int i = 0; i < cmdt::hist_bin; ++i) { cum += hist.y[i]; if (cum >= thr) { p99i = i; break; } }
				for (int i = cmdt::hist_bin - 1; i >= 0; --i) { if (hist.y[i] > 0) { pmax = i; break; } }
				out.p99  = static_cast<double>(p99i) / static_cast<double>(cmdt::hist_bin - 1);
				out.pMax = static_cast<double>(pmax) / static_cast<double>(cmdt::hist_bin - 1);
			}
			out.lvTimeMs = lvSysTimeMs_;
			out.x        = expo::histMedian(hist.y, cmdt::hist_bin);
			out.linear   = expo::srgbToLinear(out.x);
			out.ok       = (out.linear > 0.0);
			return ERR_HGC_OK;
		}
		if (keepGoing && !keepGoing()) { return ERR_HGC_RDY_METARING; }
		if (static_cast<int>(tool::getElapse(t0)) >= kMeterMaxMs) { return ERR_HGC_RDY_METARING; }
		meterSleep(kMeterRetryMs, keepGoing);
	}
}

// 使う測光ssを決める(未学習なら撮影ssから既定段数短く。学習値は撮影ssより長くしない)。
//  空を返したら「切替不要=撮影露出のまま測る」。
std::string apiCanonCCAPI::decideMeterSs(const hgc::exposure& shotExp) const
{
	if (tables_.ss.empty()) { return std::string(); }
	std::string want = meterSs_;
	if (want.empty())
	{
		const double target = expo::brightnessStops(shotExp, tables_) - static_cast<double>(kMeterInitDropStops);
		double best = 1e9;
		for (const auto& e : tables_.ss)
		{
			hgc::exposure t = shotExp; t.ss = e.value;
			const double d = std::fabs(expo::brightnessStops(t, tables_) - target);
			if (d < best) { best = d; want = e.value; }
		}
	}
	if (want.empty() || want == shotExp.ss) { return std::string(); }
	// 測光ssは撮影ssより長くしない(測光ssの存在意義は「LVが積分できる短さで忠実に測る」ことだけ。
	// 夜明けに学習値が縮まず撮影ssと5段逆転→窓切替で+4.7段の明るい1コマが撮れた 7/25実測)。
	const double wantSec = expo::parseValue(want, expo::expoKind::ss);
	const double shotSec = expo::parseValue(shotExp.ss, expo::expoKind::ss);
	if (wantSec <= 0.0 || (shotSec > 0.0 && wantSec >= shotSec)) { return std::string(); }
	return want;
}

// 測光結果から次コマの測光ssを学習し、張り付き(露出を変えても値が動かない)を判定する。
//  短い側・忠実優先: 暗すぎる時だけ控えめに伸ばし、明るすぎたら縮め、張り付いたら天井を下げる。
void apiCanonCCAPI::adaptMeterSs(const hgc::exposure& meterExp, double linear, bool& pinnedOut)
{
	pinnedOut = false;
	if (tables_.ss.empty()) { return; }
	const double curStops = expo::brightnessStops(meterExp, tables_);
	const double x = (linear > 0.0) ? linear : 0.0;

	if (meterPrevLin_ > 0.0 && x > 0.0)
	{
		const double dSs = curStops - meterPrevStops_;			// 指示した変化[段]
		if (std::fabs(dSs) >= 0.5)
		{
			const double dLin = std::log2(x / meterPrevLin_);	// 実際に動いた[段]
			if ((dLin / dSs) < kMeterRespondRatio) { pinnedOut = true; }
		}
	}
	meterPrevStops_ = curStops;
	meterPrevLin_   = x;

	double wantStops;
	if (pinnedOut)
	{	// 張り付き=このssは長すぎてLVが積分できない。天井として記録し短い側へ後退。
		meterCeilStops_ = curStops - kMeterPinBackoffStops;
		wantStops = meterCeilStops_;
	}
	else
	{
		const double loLin = expo::srgbToLinear(kMeterUsableLoX);
		const double hiLin = expo::srgbToLinear(kMeterUsableHiX);
		if (x > 0.0 && x < loLin)
		{	// 暗すぎ → 信号を得るため少しだけ伸ばす(pin突入を防ぐため1コマ1段まで)。
			double d = std::log2(loLin / x);
			if (d > kMeterMaxLenStep) { d = kMeterMaxLenStep; }
			wantStops = curStops + d;
		}
		else if (x > hiLin)
		{	// 明るすぎ(飽和寄り) → 縮める。
			double d = std::log2(hiLin / x);
			if (d < -3.0) { d = -3.0; }
			wantStops = curStops + d;
		}
		else
		{	// 十分な信号がある → これ以上伸ばさず短い側を維持。
			wantStops = curStops;
		}
		meterCeilStops_ += kMeterCeilRelaxStops;	// 天井は毎コマ少し緩めて条件変化へ追従
		if (wantStops > meterCeilStops_) { wantStops = meterCeilStops_; }
	}
	std::string pick; double best = 1e9;
	for (const auto& e : tables_.ss)
	{
		hgc::exposure t = meterExp; t.ss = e.value;
		const double d = std::fabs(expo::brightnessStops(t, tables_) - wantStops);
		if (d < best) { best = d; pick = e.value; }
	}
	if (!pick.empty()) { meterSs_ = pick; }
}

// 測光の入口。方式A(撮影画像フィードバック)/方式B(LVヒスト)を切り替える。
//  Aが失敗したときの自動フォールバックはしない(据え置き=従来の測光失敗時と同じ挙動。
//  こっそりBへ落ちると評価が濁り、測光ss切替の副作用も混入するため)。
errCode apiCanonCCAPI::meterScene(const hgc::exposure& shotExp, meterResult& out,
                                  const std::function<bool()>& keepGoing)
{
	if (kUseShotThumbMetering) { return meterSceneShot(shotExp, out, keepGoing); }
	return meterSceneLv(shotExp, out, keepGoing);
}

// 方式B: LVヒストグラム測光(旧方式。kUseShotThumbMetering=false で復活)。
//  1. 測光ssを決めて必要なら切替(失敗は ssSwitchFailed で申告=呼び出し側がssを必ず再送)
//  2. LV反映を待って測光(meterHere)
//  3. 場面の明るさへ割り戻し、次コマの測光ssを学習
errCode apiCanonCCAPI::meterSceneLv(const hgc::exposure& shotExp, meterResult& out,
                                    const std::function<bool()>& keepGoing)
{
	out = meterResult{};
	hgc::exposure meterExp = shotExp;

	const std::string want = decideMeterSs(shotExp);
	if (!want.empty())
	{
		if (setSS(want) != ERR_HGC_OK)
		{
			// 切替失敗。応答が返らないだけでカメラに遅延適用されることがある(IMG_3920事故)。
			// 「失敗=未適用」とは仮定せず申告し、呼び出し側に次の適用でssを必ず再送させる。
			out.ssSwitchFailed = true;
		}
		else
		{
			meterExp.ss     = want;
			out.appliedSs   = want;
			out.meterSsUsed = want;
			// 反映待ち: Tv変更直後はsystemtimeが新しくても中身が変更前のことがある(実測)。上限まで待つ。
			void* t0 = tool::startElapse();
			meterSleep(kMeterSettleMaxMs, keepGoing);
			out.settleMs = static_cast<int>(tool::getElapse(t0));
		}
	}

	meterResult here;
	const errCode e = meterHere(here, keepGoing);
	// meterHere の診断を統合(切替系のフィールドは維持)。
	out.linear = here.linear; out.x = here.x; out.p99 = here.p99; out.pMax = here.pMax;
	out.histSum = here.histSum; out.lvTimeMs = here.lvTimeMs; out.staleSkip = here.staleSkip;
	out.tries = here.tries; out.rdyMs = here.rdyMs;
	out.meterExp = meterExp;
	if (e != ERR_HGC_OK || here.linear <= 0.0) { out.ok = false; return (e != ERR_HGC_OK) ? e : ERR_HGC_RDY_METARING; }

	out.ok       = true;
	out.sceneRef = here.linear / std::pow(2.0, expo::brightnessStops(meterExp, tables_));
	// 測光ssを切替えて測ったコマだけ学習する(撮影露出のまま測ったコマは従来どおり学習しない)。
	if (!out.meterSsUsed.empty())
	{
		bool pinned = false;
		adaptMeterSs(meterExp, here.linear, pinned);
		out.pinned = pinned;
	}
	return ERR_HGC_OK;
}

// funcList のURL(絶対URL)から "http://host:port" を切り出す。
std::string apiCanonCCAPI::apiHostBase(void) const
{
	auto it = funcList.find(funcNum::EVENT_POLL);
	std::string url = (it != funcList.end()) ? it->second.url : std::string();
	if (url.empty())
	{
		auto it2 = funcList.find(funcNum::SHOT);
		if (it2 != funcList.end()) { url = it2->second.url; }
	}
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos) { return std::string(); }
	const size_t path = url.find('/', scheme + 3);
	return (path == std::string::npos) ? url : url.substr(0, path);
}

// event/polling で新規画像の登録(addedcontents)を待ち、最後(最新)のコンテンツパスを返す。
//  ・撮影→現像→SD書込の完了は露光終了から実測2.0〜2.6秒(7/26 R10)。ロングポールなので
//    既に登録済みならすぐ返り、未登録なら登録まで待つ。
//  ・複数たまっていた場合(夜間の測光なしコマ等)は最後の1件=最新を使う。
std::string apiCanonCCAPI::waitAddedContents(int budgetMs, const std::function<bool()>& keepGoing, int& triesOut)
{
	triesOut = 0;
	if (!(funcList[funcNum::EVENT_POLL].verb == verb::GET)) { return std::string(); }
	void* t0 = tool::startElapse();
	std::string last;
	while (static_cast<int>(tool::getElapse(t0)) < budgetMs)
	{
		if (keepGoing && !keepGoing()) { break; }
		++triesOut;
		std::string body;
		if (netThread::httpGet(funcList[funcNum::EVENT_POLL].url, body) && !body.empty())
		{
			// {"addedcontents":["/ccapi/.../IMG_xxxx.JPG", ...], ...} を軽量に抽出(DOM化しない)。
			const size_t key = body.find("\"addedcontents\"");
			if (key != std::string::npos)
			{
				size_t p = body.find('[', key);
				const size_t e = (p == std::string::npos) ? std::string::npos : body.find(']', p);
				while (p != std::string::npos && e != std::string::npos)
				{
					const size_t q1 = body.find('"', p + 1);
					if (q1 == std::string::npos || q1 > e) { break; }
					const size_t q2 = body.find('"', q1 + 1);
					if (q2 == std::string::npos || q2 > e) { break; }
					last = body.substr(q1 + 1, q2 - q1 - 1);
					p = q2;
				}
				if (!last.empty())
				{
					// CCAPIのJSONは "\/" とスラッシュをエスケープして返す。生抽出なので戻す
					// (戻さないと不正URLになりサムネ取得が404で全滅する。7/27実機で発生)。
					std::string un; un.reserve(last.size());
					for (size_t i = 0; i < last.size(); ++i)
					{
						if (last[i] == '\\' && i + 1 < last.size() && last[i + 1] == '/') { continue; }
						un.push_back(last[i]);
					}
					return un;
				}
			}
			// イベントはあったが addedcontents 無し(設定変更等) → 続けて待つ。
		}
		else
		{
			meterSleep(100, keepGoing);	// 取得失敗(503等) → 100ms置いて再試行(ユーザー指定粒度)
		}
	}
	return last;
}

// 方式Aの中核(露出非依存の部分): 新規画像の登録を待ち、サムネイルを取得・復号して
// 輝度ヒストグラム統計(中央値/p99/pMax/チェックサム)まで作る。
//  meterExp/sceneRef は呼び出し側(meterSceneShot)が撮影露出で確定する。
errCode apiCanonCCAPI::thumbMeterCore(meterResult& out, int budgetMs, const std::function<bool()>& keepGoing)
{
	out = meterResult{};
	void* t0 = tool::startElapse();

	// ① 新しい画像の登録通知を待つ(カメラが露光後の記録を終えるまで)。
	int tries = 0;
	const std::string path = waitAddedContents(budgetMs, keepGoing, tries);
	out.tries  = tries;
	out.waitMs = static_cast<int>(tool::getElapse(t0));
	if (path.empty()) { out.failStage = 1; out.rdyMs = out.waitMs; return ERR_HGC_RDY_METARING; }

	const std::string base = apiHostBase();
	if (base.empty()) { out.failStage = 2; out.rdyMs = out.waitMs; return ERR_HGC_RDY_METARING; }

	// ② サムネイル取得。記録直後のカメラは 503 で弾くことがあるので予算内でリトライする。
	void* tf = tool::startElapse();
	const std::string url = base + path + "?kind=thumbnail";
	std::string jpg;
	bool got = false;
	while (true)
	{
		++out.fetchTries;
		jpg.clear();
		if (netThread::httpGet(url, jpg) && !jpg.empty()) { got = true; break; }
		if (keepGoing && !keepGoing()) { break; }
		if (static_cast<int>(tool::getElapse(t0)) >= budgetMs) { break; }	// 総予算切れ
		meterSleep(kThumbFetchRetryMs, keepGoing);
	}
	out.fetchMs = static_cast<int>(tool::getElapse(tf));
	if (!got) { out.failStage = 3; out.rdyMs = static_cast<int>(tool::getElapse(t0)); return ERR_HGC_RDY_METARING; }

	// ③ 復号→輝度ヒストグラム(レターボックス黒帯は上下6%を捨てて除去)。
	void* td = tool::startElapse();
	uint16_t hist[256];
	int w = 0, h = 0;
	const bool dec = jpglm::lumaHistogram(reinterpret_cast<const uint8_t*>(jpg.data()), jpg.size(),
	                                      hist, w, h, 0.06);
	out.decodeMs = static_cast<int>(tool::getElapse(td));
	out.rdyMs    = static_cast<int>(tool::getElapse(t0));
	if (!dec) { out.failStage = 4; return ERR_HGC_API_ANALIZE; }

	// ④ 中央値→リニア。診断(p99/pMax/チェックサム)も同型で埋める。
	uint32_t sum = 0; double total = 0.0;
	for (int i = 0; i < 256; ++i) { sum = sum * 31u + hist[i]; total += hist[i]; }
	out.histSum = sum;
	if (total > 0.0)
	{
		const double thr = total * 0.99; double cum = 0.0; int p99i = 255, pmax = 0;
		for (int i = 0; i < 256; ++i) { cum += hist[i]; if (cum >= thr) { p99i = i; break; } }
		for (int i = 255; i >= 0; --i) { if (hist[i] > 0) { pmax = i; break; } }
		out.p99  = p99i / 255.0;
		out.pMax = pmax / 255.0;
	}
	out.x      = expo::histMedian(hist, 256);
	out.linear = expo::srgbToLinear(out.x);
	out.ok     = (out.linear > 0.0 && total > 0.0);
	if (!out.ok) { out.failStage = 5; return ERR_HGC_API_ANALIZE; }
	return ERR_HGC_OK;
}

// 方式A: 撮影画像フィードバック測光。直前に撮れた画像のサムネイルから輝度を得る。
//  ・本露光の積分そのものなので、LVが頭打ちになる夜間でも真値が得られる(7/26実験: LVより約3.75段深い)。
//  ・露出には一切触れない(ss切替なし=appliedSs空・settleなし)。
//  ・呼び出しタイミングは meterTimingHint の申告どおり(露光終了直後)。captureRunner が従う。
//  ・shotExp = このサムネイルを撮った露出(呼び出し側の直前コマ)。sceneRef の割り戻しに使う。
errCode apiCanonCCAPI::meterSceneShot(const hgc::exposure& shotExp, meterResult& out,
                                      const std::function<bool()>& keepGoing)
{
	const errCode e = thumbMeterCore(out, kThumbBudgetMs, keepGoing);
	out.meterExp = shotExp;
	if (e != ERR_HGC_OK) { return e; }
	if (!out.ok) { return ERR_HGC_API_ANALIZE; }
	out.sceneRef = out.linear / std::pow(2.0, expo::brightnessStops(shotExp, tables_));
	return ERR_HGC_OK;
}
