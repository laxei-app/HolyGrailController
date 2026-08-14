#include "common.h"
#include "apiCanonCCAPI.h"
#include "netThread.h"
#include "exposureMath.h"
#include "jpegLuma.h"	// 撮影画像サムネイルの輝度ヒストグラム化(測光)
#include <json/nlohmann/json.hpp>
#include <cmath>
#include <cstring>	// pickThumbPath の拡張子比較

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
    // 全部断られた。ただし「既に開始済み」なだけなら成功として扱う。
    //
    // 【2026-07-30 R10 実機】開始済みの liveview へ再度 POST すると 503 {"message":"Device busy"}
    //  を返す(event/polling の "Already started" と同じ構図)。従来はここで失敗として返し、
    //  撮影中の再接続がこの一手だけで完全に塞がれていた(カメラは生きていて、同時刻に
    //  再接続自体は成功しているのにシャッターへ進めない)。
    //  推測で握りつぶすのではなく、ライブビューが実際に流れているかをカメラに聞いて判断する。
    if (this->liveViewAlive()) { return ERR_HGC_OK; }
    return ERR_HGC_HTTP_POST;
}

// ライブビューを止める。
//
// 【2026-07-30】従来は撮影開始時に開始したまま、撮影が終わるまで一度も止めていなかった。
//  ライブビュー中はカメラ本体のMenu操作が busy で弾かれる = こちらがカメラを占有している
//  直接の証拠。しかもサムネ測光では撮影ループ中にライブビューを一切使っていない
//  (使うのは撮影前の初期収束だけ)。占有し続ける理由が無いので離す。
errCode apiCanonCCAPI::stopLiveView(void)
{
    if (!(funcList[funcNum::LIVE_SET].verb == verb::POS)) { return ERR_HGC_NOT_SUPPORTED; }
    if (!this->liveViewAlive()) { return ERR_HGC_OK; }	// 既に止まっている
    std::string response;
    json request_json;
    request_json["liveviewsize"]  = "off";
    request_json["cameradisplay"] = "on";	// 背面表示は戻す(撮影自体には影響しない)
    if (netThread::httpPost(funcList[funcNum::LIVE_SET].url, request_json.dump(), response))
    {
        return ERR_HGC_OK;
    }
    return ERR_HGC_HTTP_POST;
}

namespace
{
	// ライブビュー付帯情報(?kind=info)の体裁チェック。中身(ヒストグラム)は解析しない。
	//  return 0=正常 / 1=マーカ不一致(付帯情報ではない) / 2=長さフィールドが本文に収まらない
	// 従来 alzMetering は先頭3バイトを長さ確認なしで読んでいたので、その境界検査もここへ寄せた。
	int checkLiveViewInfo(const std::string& body)
	{
		if (body.size() < 10) { return 1; }
		if (static_cast<uint8_t>(body[0]) != 0xff
		 || static_cast<uint8_t>(body[1]) != 0x00
		 || static_cast<uint8_t>(body[2]) != 0x01) { return 1; }
		const uint32_t len = (static_cast<uint32_t>(static_cast<uint8_t>(body[3])) << 24)
		                   | (static_cast<uint32_t>(static_cast<uint8_t>(body[4])) << 16)
		                   | (static_cast<uint32_t>(static_cast<uint8_t>(body[5])) <<  8)
		                   |  static_cast<uint32_t>(static_cast<uint8_t>(body[6]));
		return (len > static_cast<uint32_t>(body.size() - 9)) ? 2 : 0;
	}
}

// ライブビューが実際に動いているか(軽い ?kind=info で確認する)。
//  これは「接続の生存確認」で、撮影中の再接続判断(startLiveView の『既に開始済み』/
//  stopLiveView の『既に止まっている』)に使う。中身の体裁までは見ない。
bool apiCanonCCAPI::liveViewAlive(void)
{
    if (!(funcList[funcNum::LIVE_DETAIL].verb == verb::GET)) { return false; }
    std::string body;
    if (!netThread::httpGet(funcList[funcNum::LIVE_DETAIL].url + "?kind=info", body)) { return false; }
    return !body.empty();
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
    std::string isoCur, ssCur, fnCur;	// 同じ応答に入っている現在値(camExp_ の初期化に使う)
    errCode err = getJsonAbility(funcNum::ISO, isoRaw, &isoCur);
    if (err != ERR_HGC_OK) { return err; }
    err = getJsonAbility(funcNum::SS, ssRaw, &ssCur);
    if (err != ERR_HGC_OK) { return err; }
    err = getJsonAbility(funcNum::F_NUMBER, fnRaw, &fnCur);
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

    // いまカメラに乗っている露出を控える。初期収束(meterHere)がライブビュー測光の出発点に使う。
    // これが無いと「測光したいがカメラが何段の設定なのか分からない」ため上位から渡してもらう
    // ことになり、測り方の都合が captureRunner へ漏れる。ここで持てば漏れない。
    camExp_.iso = toDisp(isoCur, expo::expoKind::iso);
    camExp_.ss  = toDisp(ssCur,  expo::expoKind::ss);
    camExp_.fn  = toDisp(fnCur,  expo::expoKind::fn);

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
    if (netThread::httpPut(funcList[func].url, body, resp)) { camExp_.fn = fNumber; return ERR_HGC_OK; }

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
    if (netThread::httpPut(funcList[func].url, body, resp)) { camExp_.ss = ss; return ERR_HGC_OK; }

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
    if (netThread::httpPut(funcList[func].url, body, resp)) { camExp_.iso = iso; return ERR_HGC_OK; }

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
	// イベント取得(event/polling)を停止する。GETで開始したものの対(CCAPI Reference 4.13.1)。
	// 開きっぱなしにするとカメラ側に取得状態が残る。撮影セッションの終了時に必ず送る。
	stopEventPolling();

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

// 接続維持用の無害なGET。撮影窓まで待機中や、測光しないコマ(夜間の固定露出)で定期送出して
// 無通信でカメラのWi-Fi/CCAPIセッションが切れるのを防ぐ。到達できれば ERR_HGC_OK。
//
// 【ついでに登録通知を流す】この方式では撮影画像の登録通知(event/polling)を測光のたびに
//  引いている。ところが夜間の固定露出は測光しないので、その間 通知がカメラ側に溜まり続ける
//  (1コマ2件×数千コマ)。溜まったぶんは夜明けの最初の測光で一度に返ってくるので、応答が
//  大きくなり、エッジ端末のメモリを圧迫する。keepAlive は「無害なGETを1回投げる」ものなので、
//  その1回を event/polling にすれば、追加の通信なしに溜まりを流せる。
//  event/polling を持たないカメラでは従来どおり /ccapi カタログを取る。
errCode apiCanonCCAPI::keepAlive(void)
{
    std::string resp;
    auto it = funcList.find(funcNum::EVENT_POLL);
    if (it != funcList.end() && (it->second.verb == verb::GET))
    {   // クエリ無し=待たずに即返る。溜まっていた通知はこの応答で捨てられる。
        if (netThread::httpGet(it->second.url, resp)) { return ERR_HGC_OK; }
        return ERR_HGC_HTTP_GET;
    }
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
errCode apiCanonCCAPI::getJsonAbility(funcNum number, std::vector<std::string>& abilitys, std::string* curRaw)
{
    if (curRaw != nullptr) { curRaw->clear(); }
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
        // 同じ応答の "value" が現在値。取れなくても ability は返す(現在値は任意の付録)。
        if (curRaw != nullptr && json.contains("value") && json.at("value").is_string())
        {
            *curRaw = json.at("value").get<std::string>();
        }
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
        // 体裁の検査は checkLiveViewInfo へ寄せた(測光可の判定 liveViewMeterReady と同じ物差しに
        // するため。従来は先頭3バイトを長さ確認なしで読んでいたので境界検査も入った)。
        const int lvChk = checkLiveViewInfo(liveViewInfo);
        if (lvChk == 1)
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

        if (lvChk == 2)
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
//
//  【方式】直前に撮れた最新の撮影画像のサムネイルから、場面のリニア輝度を出す。
//   1コマ:  シャッター → 露光(ss) → event/polling で登録通知 → 最新ファイルのサムネイル取得
//           → リニア輝度 → 露出設定 → 撮影周期の残りを待つ → 次のシャッター
//   本露光そのものの積分なので、ライブビューが約1.6秒相当で頭打ちになる夜間でも真値が出る。
//
//  【この層に閉じていること】測り方の一切(何を読むか・カメラをどう使うか・何回粘るか)。
//   上位(captureRunner)へ渡すのは meterResult の sceneRef / usable と診断だけである。
//   別メーカーのカメラは、まったく違う測り方でこの2関数を実装すればよい。
//
//  【ライブビューは初期収束だけ】meterHere は撮影窓の手前で1枚目の露出を決めるためのもので、
//   その時点ではまだ1コマも撮っていない=サムネイルの元になる画像が無い。他に測る手段が
//   無いのでここだけライブビューを使う。撮影ループに入ったら掴みもしない。
// ============================================================================
namespace
{
	// --- 撮影画像フィードバック測光の予算 ---
	// 露光終了 → カメラの記録完了 → 登録通知 → サムネイル取得 までを含む総予算。
	// 記録は実測2.0〜2.6秒だが、カメラが混んでいると伸びる。取得が503等で弾かれても
	// 諦めずこの予算内でリトライする(1回で諦めると測光失敗を接続断と誤判定した経緯がある)。
	constexpr int    kThumbBudgetMs        = 10000;	// 測光全体(待ち+取得リトライ)の上限[ms]
	constexpr int    kThumbFetchRetryMs    = 200;	// サムネイル取得のリトライ間隔[ms]
	constexpr int    kPollGapMs            = 200;	// event/polling の再試行間隔[ms](連打防止)
	// サムネイルのレターボックス黒帯を捨てる比率(上下それぞれ)。160x120サムネは3:2画像に
	// 黒帯が付くため、捨てないと中央値が約0.2段下がる(実測)。
	constexpr double kThumbCropRatio       = 0.06;

	// --- 初期収束(meterHere)のライブビュー測光の調整定数 ---
	constexpr int    kMeterRetryMs        = 100;	// ヒスト取得リトライ間隔[ms]
	constexpr int    kMeterMaxMs          = 5000;	// ヒスト取得リトライ上限[ms]
	constexpr int    kLvFreshMarginMs     = 2000;	// 古いLVフレーム判定の許容[ms](生成周期+揺らぎ)
	// 測光ssの上限[秒]。これより長いとLVが積分できず張り付く(実測: 0.6sはpin=0、2sで時々pin=1)。
	constexpr double kInitMeterMaxSsSec   = 0.5;
	// ヒストグラム中央値がこの範囲外なら明暗に張り付き(その値は使わず測光露出をずらす)。
	constexpr double kPegBright           = 0.99;
	constexpr double kPegDark             = 0.01;
	// 張り付きを抜けるための測光露出のずらし。飽和側は「どれだけ超えているか」の情報が無い
	// (真昼にISO1600/0.5sだと十数段超え)ので大股で降りる。-2段刻みでは真昼に時間内で
	// 抜けられなかった(2026-07-26 09:00 実測)。
	constexpr double kPegBrightStep       = -4.0;
	constexpr double kPegDarkStep         = +2.0;
	constexpr int    kHereMaxShift        = 4;		// 1回の meterHere で張り付きを抜ける試行の上限
	// --- ライブビュー主体方式 ---
	// ライブビューが底に張り付いたまま抜けられないとき、サムネイルを何コマに1回取るか。
	//  取得回数に上限がある機種のための方式なので、必要最小限にする。夜明けの明るさは
	//  15秒周期で 0.03段/コマ程度しか動かないので、4コマ(1分)に1回でも 0.13段刻みで足りる。
	constexpr int    kLvFallbackEveryN    = 4;
	// サムネイルへ落ちたときの測光予算[ms]。1コマ前のファイルを直接取るので待ちは要らない。
	constexpr int    kLvFallbackBudgetMs  = 6000;
	// 撮影ファイルの登録通知をこれだけ待つ[ms]。ライブビュー主体では通知は
	//  「どのファイルが1コマ前か」を知るためだけに使うので、取れなければ諦めてよい。
	constexpr int    kLvNotifyBudgetMs    = 3000;
	constexpr int    kHereSettleMs        = 700;	// 測光露出を変えてからLVが追従するまでの待ち[ms]
	constexpr int    kHereApplyRetryMs    = 300;	// 測光露出の適用に失敗したときの間隔[ms]

	// 露出の3軸がそろっているか。
	bool validExp(const hgc::exposure& e)
	{
		return !e.iso.empty() && !e.ss.empty() && !e.fn.empty();
	}
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
	hereExp_       = hgc::exposure{};	// 初期収束の測光露出は取り直す
	lvFreshPrevMs_ = 0;					// 再接続でLVセッションが作り直されるため鮮度基準も捨てる
	lvFreshPrevAt_ = nullptr;
	// ライブビュー主体方式の状態。再接続でファイル名の並びの追跡が切れるので捨てる
	// (古いパスを1コマ前だと思って測ると、まったく別の明るさを掴む)。
	lvShotPrev_.clear(); lvShotLast_.clear();
	lvFallbackSkip_ = 0; lvHeldSceneRef_ = 0.0;
	// 【空読みはここでしない(2026-08-13)】ここで流しても、この後の準備(撮影モード変更・
	//  設定取得・初期収束の露出適用)で新しいイベントが積まれてしまい、1コマ目の登録通知が
	//  その中に埋もれて拾えなかった。空読みは1枚目の直前(meterArm)へ移した。
}

// 1枚目のシャッターを切る直前。ここまでの準備で積まれた通知/設定変更イベントを捨て、
// 次に来る通知が1コマ目のものになるようにする。
void apiCanonCCAPI::meterArm(void)
{
	this->flushEventPolling();
}

// 測光露出をテーブル上で delta 段ずらす(結果の ss は capSec を超えない)。
// iso/fn は動かさず ss だけで作る。ISO を動かすと測光のノイズ特性まで変わるため。
void apiCanonCCAPI::shiftMeterSs(hgc::exposure& me, double delta, double capSec) const
{
	if (tables_.ss.empty()) { return; }
	hgc::exposure t = me;
	const double wantStops = expo::brightnessStops(me, tables_) + delta;
	std::string pick; double best = 1e9;
	for (const auto& e : tables_.ss)
	{
		if (capSec > 0.0 && e.real > capSec) { continue; }
		t.ss = e.value;
		const double d = std::fabs(expo::brightnessStops(t, tables_) - wantStops);
		if (d < best) { best = d; pick = e.value; }
	}
	if (!pick.empty()) { me.ss = pick; }
}

// ライブビューのヒストグラムを1枚ぶん読む(取れるまで kMeterMaxMs まで粘る)。
//  長秒露光の直後はLVが使えるようになるまで実測3.3秒かかる機種があるので粘る。
//  古いフレーム(時刻の進みが実経過に足りない)は捨てて取り直す。
//  露出には一切触れない。呼び出し側(meterHere)が測光露出を決める。
errCode apiCanonCCAPI::readLvHistogram(meterResult& out, const std::function<bool()>& keepGoing)
{
	void* t0 = tool::startElapse();
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
			// チェックサム+明側診断(p99/pMax)+統計量の比較用(制御には使わない)。
			uint32_t s = 0; double total = 0.0;
			for (int i = 0; i < cmdt::hist_bin; ++i) { s = s * 31u + hist.y[i]; total += hist.y[i]; }
			out.histSum = s;
			if (total > 0.0)
			{
				const double thr = total * 0.99; double cum = 0.0; int p99i = cmdt::hist_bin - 1, pmax = 0;
				for (int i = 0; i < cmdt::hist_bin; ++i) { cum += hist.y[i]; if (cum >= thr) { p99i = i; break; } }
				for (int i = cmdt::hist_bin - 1; i >= 0; --i) { if (hist.y[i] > 0) { pmax = i; break; } }
				const double last  = static_cast<double>(cmdt::hist_bin - 1);
				out.p99  = static_cast<double>(p99i) / last;
				out.pMax = static_cast<double>(pmax) / last;
				const double thr75 = total * 0.75, thr90 = total * 0.90;
				int  p75i = cmdt::hist_bin - 1, p90i = cmdt::hist_bin - 1;
				bool got75 = false, got90 = false;
				double c2 = 0.0, sumLin = 0.0;
				for (int i = 0; i < cmdt::hist_bin; ++i)
				{
					const double n = hist.y[i];
					sumLin += expo::srgbToLinear(static_cast<double>(i) / last) * n;
					c2 += n;
					if (!got75 && c2 >= thr75) { p75i = i; got75 = true; }
					if (!got90 && c2 >= thr90) { p90i = i; got90 = true; }
				}
				out.meanLin  = sumLin / total;
				out.p75      = static_cast<double>(p75i) / last;
				out.p90      = static_cast<double>(p90i) / last;
				out.satRatio = static_cast<double>(hist.y[cmdt::hist_bin - 1]) / total;
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

// 【シャッター前の測光】撮影窓の手前で1枚目の露出を決める初期収束用。
//
// まだ1コマも撮っていないのでサムネイルが無い。ここだけライブビューで測る。
// 測るのに都合のよい露出(=測光露出)はこの関数が自分で決める:
//  ・出発点は前回の続き(hereExp_)、無ければカメラに乗っている値(camExp_)。
//  ・ss は kInitMeterMaxSsSec を超えない。これより長いとLVが積分できず張り付いて測れない。
//  ・白飛び/黒潰れならその値は使わず、測光露出をずらして測り直す。
// 返すのは meterScene と同じ「露出非依存の場面の明るさ」sceneRef なので、呼び出し側は
// どちらで測ったのかを知らなくてよい。カメラへ乗せた露出は appliedExp で申告する。
//
// failStage: 20=出発点の露出が無い / 21=測光露出を適用できない / 22=LVヒストが取れない
//            23=張り付きを抜けられなかった
// 初期収束(撮影前)の測光。撮り始めていないので出発点は自分の学習値しか無い。
errCode apiCanonCCAPI::meterHere(meterResult& out, const std::function<bool()>& keepGoing)
{
	return this->meterLvAt(hgc::exposure{}, out, keepGoing);
}

// ライブビュー測光の本体。seed が有効ならそこから測り始める。
//
// 【seed を渡せるようにした理由(2026-08-14)】ライブビューの明るさを場面の明るさへ直すには
//  「そのとき何段の露出で見ているか」で割り戻す必要があるので、測光は露出を確定させてから行う。
//  初期収束では撮影露出がまだ無く、自分の学習値(hereExp_)を乗せるしかない。
//  ところが撮影中も同じ経路を使うと、**撮影露出とは別系統の測光露出を毎コマ乗せ直す**ことになり、
//  「撮る→ssが変わる→また戻る」が毎コマ起きる。撮影露出そのもので測れるなら乗せ替えは要らない。
//  そこで撮影中は撮影露出を seed として渡し、ここで何も送らずに測る(送るのは張り付いたときだけ)。
errCode apiCanonCCAPI::meterLvAt(const hgc::exposure& seed, meterResult& out,
                                 const std::function<bool()>& keepGoing)
{
	out = meterResult{};
	out.settleMs = 0;

	// 撮影ループ中はライブビューを掴んでいない(離してある)ので、要るときだけ張る。
	if (!this->liveViewAlive()) { this->startShooting(); }

	// 測光露出の出発点。撮影露出(seed)が使えるならそれ、無ければ自分の学習値。
	hgc::exposure me = validExp(seed) ? seed : (validExp(hereExp_) ? hereExp_ : camExp_);
	if (!validExp(me)) { out.failStage = 20; return ERR_HGC_RDY_METARING; }
	if (expo::parseValue(me.ss, expo::expoKind::ss) > kInitMeterMaxSsSec)
	{
		this->shiftMeterSs(me, 0.0, kInitMeterMaxSsSec);
	}

	for (int shift = 0; shift < kHereMaxShift; ++shift)
	{
		if (keepGoing && !keepGoing()) { break; }

		// 測光露出をカメラへ乗せる(変わっていない軸は送らない)。
		//  1つも送らなかったときは「いまカメラに乗っている露出のまま測る」ので、
		//  ライブビューが追従するのを待つ理由も無い(撮影中の通常時はこの経路になる)。
		const bool needFn  = (me.fn  != camExp_.fn);
		const bool needSs  = (me.ss  != camExp_.ss);
		const bool needIso = (me.iso != camExp_.iso);
		const bool applied = needFn || needSs || needIso;
		errCode ae = ERR_HGC_OK, r;
		if (needFn)  { r = this->setFNumber(me.fn); if (r != ERR_HGC_OK) { ae = r; } }
		if (needSs)  { r = this->setSS(me.ss);      if (r != ERR_HGC_OK) { ae = r; } }
		if (needIso) { r = this->setIso(me.iso);    if (r != ERR_HGC_OK) { ae = r; } }
		out.appliedExp = camExp_;	// 実際にカメラへ乗った値(成功した軸だけ更新されている)
		if (ae != ERR_HGC_OK)
		{
			// 乗っていない露出で測ると、割り戻す分母が嘘になり場面の明るさを取り違える。
			// 測らずにやり直す(連打はしない)。
			out.applyFailed = true;
			out.failStage   = 21;
			meterSleep(kHereApplyRetryMs, keepGoing);
			continue;
		}
		if (applied)
		{
			meterSleep(kHereSettleMs, keepGoing);	// LVが新しい露出へ追従するのを待つ
			out.settleMs += kHereSettleMs;
		}

		meterResult h;
		h.tries = out.tries;	// 試行回数は通算で数える
		if (this->readLvHistogram(h, keepGoing) != ERR_HGC_OK || !(h.linear > 0.0))
		{
			out.tries    = h.tries;
			out.rdyMs    = h.rdyMs;
			out.staleSkip += h.staleSkip;
			out.failStage = 22;
			continue;
		}
		// 測れた。張り付きで捨てる回もログには残したいので、見えていた値をそのまま out へ移す
		// (通算で数えているもの=試行回数・捨てた回数・待ち・適用値 は引き継ぐ)。
		h.staleSkip += out.staleSkip;
		h.settleMs   = out.settleMs;
		h.appliedExp = out.appliedExp;
		out = h;

		// 白飛び: 大股で降りる。ss が最短側に達して降りきれないときは ISO を下げて抜ける。
		if (h.x >= kPegBright)
		{
			const double before = expo::brightnessStops(me, tables_);
			this->shiftMeterSs(me, kPegBrightStep, kInitMeterMaxSsSec);
			if (before - expo::brightnessStops(me, tables_) < 1.0)
			{
				hgc::exposure t = me;
				const double wantB = before + kPegBrightStep;
				std::string pick; double best = 1e9;
				for (const auto& e : tables_.iso)
				{
					t.iso = e.value;
					const double d = std::fabs(expo::brightnessStops(t, tables_) - wantB);
					if (d < best) { best = d; pick = e.value; }
				}
				if (!pick.empty()) { me.iso = pick; }
			}
			out.failStage = 23;
			continue;
		}
		// 黒潰れ: まだ伸ばせるなら忠実上限まで伸ばして測り直す。
		// 伸ばしきっても黒い(=本当に真っ暗な夜)ならその値で進める。投影は明側限界へ
		// クランプされ、夜間露出へ落ち着く。
		if (h.x <= kPegDark
		 && expo::parseValue(me.ss, expo::expoKind::ss) < kInitMeterMaxSsSec * 0.99)
		{
			this->shiftMeterSs(me, kPegDarkStep, kInitMeterMaxSsSec);
			out.failStage = 23;
			continue;
		}

		// 採用。次回の出発点として覚える(呼ぶたびに最初から探し直さない)。
		hereExp_       = me;
		out.meterExp   = me;
		out.sceneRef   = h.linear / std::pow(2.0, expo::brightnessStops(me, tables_));
		out.ok         = true;
		out.usable     = true;
		out.failStage  = 0;
		return ERR_HGC_OK;
	}

	// 張り付きを抜けられなかった/適用できなかった。ずらした結果は hereExp_ へ残し、
	// 次の呼び出しが続きから探せるようにする(呼び出し側は自分の予算内で測り直す)。
	hereExp_   = me;
	out.ok     = false;
	out.usable = false;
	return ERR_HGC_RDY_METARING;
}

// 【撮影中の測光】直前に撮れた最新画像のサムネイルから場面の明るさを得る。
errCode apiCanonCCAPI::meterScene(const hgc::exposure& shotExp, meterResult& out,
                                  const std::function<bool()>& keepGoing)
{
	if (meterLv_) { return this->meterSceneLvFirst(shotExp, out, keepGoing); }
	return this->meterSceneShot(shotExp, out, keepGoing);
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

// event/polling の URL。クエリは付けない(=待たずに即返る)。理由はヘッダの説明を参照。
std::string apiCanonCCAPI::pollUrl(void) const
{
	auto it = funcList.find(funcNum::EVENT_POLL);
	return (it == funcList.end()) ? std::string() : it->second.url;
}

// イベント取得を停止する(GET で開始したものの対。セッション終了時に必ず送る)。
void apiCanonCCAPI::stopEventPolling(void)
{
	auto it = funcList.find(funcNum::EVENT_POLL);
	if (it == funcList.end() || !(it->second.verb == verb::DEL)) { return; }
	std::string resp;
	netThread::httpDelete(it->second.url, resp);	// 旧方式の残骸掃除(念のため)
}

// 溜まっている登録通知を1回で流す(セッション開始時)。
//  前の撮影で撮った画像の通知が残っていると、1枚目の測光がその古い画像を掴む。
//  クエリ無しの GET は「待たずに即返る」ので、1往復で溜まったぶんを捨てられる。
void apiCanonCCAPI::flushEventPolling(void)
{
	auto it = funcList.find(funcNum::EVENT_POLL);
	if (it == funcList.end() || !(it->second.verb == verb::GET)) { return; }
	std::string body;
	netThread::httpGet(it->second.url, body);	// 結果は捨てる(流すのが目的)
}

// 登録通知の中から、サムネイルを取る1枚を選ぶ。
//
// 【なぜ選ぶ必要があるか】JPG+RAW記録だと1コマの撮影で2つ通知される(CR3 と JPG)。
// 【なぜ JPG を優先するか(2026-08-13 実測)】同じ絵でも取得元で持ちが違う。露光1/60・
//  最新ファイル・300コマ上限の同一条件で、R10 は CR3 なら48回、JPG なら179回で止まった
//  (R100 はどちらも300コマ完走)。CR3 は縮小画像を出すのに RAW の解析が要るのに対し、
//  JPG は埋め込みサムネイルをそのまま返せる、と考えると筋は通る。
//  どちらも無ければ最後の1つ(=最新)を使う。
std::string apiCanonCCAPI::pickThumbPath(const std::vector<std::string>& names)
{
	auto endsWithAny = [](const std::string& s, const char* const* exts, int n) -> bool
	{
		for (int i = 0; i < n; ++i)
		{
			const size_t el = std::strlen(exts[i]);
			if (s.size() < el) { continue; }
			bool same = true;
			for (size_t k = 0; k < el; ++k)
			{
				char c = s[s.size() - el + k];
				if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
				if (c != exts[i][k]) { same = false; break; }
			}
			if (same) { return true; }
		}
		return false;
	};
	static const char* const kJpg[] = { ".JPG", ".JPEG" };
	static const char* const kRaw[] = { ".CR3", ".CRAW", ".CR2" };
	for (size_t i = names.size(); i > 0; --i)
	{ if (endsWithAny(names[i - 1], kJpg, 2)) { return names[i - 1]; } }
	for (size_t i = names.size(); i > 0; --i)
	{ if (endsWithAny(names[i - 1], kRaw, 3)) { return names[i - 1]; } }
	return names.empty() ? std::string() : names.back();
}

// 新しい画像が記録されるのを待ち、そのパス(ホスト無しの相対パス)を返す。空=時間内に来なかった。
//
// event/polling は「カメラからの登録通知を待つ」方式で、**ディレクトリを一切読まない**のが
// 利点である。総数ポーリング方式は1コマあたり10〜13回ディレクトリを読みに行くため、RAW
// (約25MB)の書き込み中のカードを繰り返し叩くことになる。2026-08-12〜13 の実測では、この
// 流れ(露光 → 即通知 → 取得)で R100 が 300コマを完走した。
//
// 仕様(CCAPI Reference 4.13.1):
//  ・GET は「イベント取得の開始」で、対の DELETE で停止する状態付きAPI。停止し忘れると以後
//    503 {"message":"Already started"} で全滅する(旧FWのR100で残存を実機確認)。
//  ・待ち方は ver110〜 ?timeout=short / ver100 ?continue=on。無指定は「待たずに即返る」ため
//    指定しないと連打になる(旧実装は1コマ44〜59回叩いていた)。判定はセッションで一度だけ。
//
// 複数たまっていた場合(通信が詰まった等)は最後の1件=最新を使う。JPG と RAW の選択は
// pickThumbPath に任せる。
std::string apiCanonCCAPI::waitAddedByEvent(int budgetMs, const std::function<bool()>& keepGoing,
                                           int& triesOut, waitDiag& diag)
{
	triesOut = 0;
	diag = waitDiag{};
	if (!(funcList[funcNum::EVENT_POLL].verb == verb::GET)) { diag.step = 1; return std::string(); }
	void* t0 = tool::startElapse();
	bool triedRecover = false;	// このコマで「DELETEして再判定」を試したか(1回だけ)
	// 通知の取得そのものは通ったか。1回でも通っていれば「カメラは応答しているのに画像が
	// 現れない」と言い切れる(上位はこれを見てオフライン判定へ回す)。途中の一過性の失敗で
	// この判断を落とさないよう、最後の1回ではなく「1回でも通ったか」で持つ。
	bool anyPollOk = false;

	while (static_cast<int>(tool::getElapse(t0)) < budgetMs)
	{
		if (keepGoing && !keepGoing()) { break; }
		++triesOut;

		std::string body;
		bool ok = netThread::httpGet(pollUrl(), body);
		if (!ok && !triedRecover)
		{	// イベント取得が開始済みで残っている疑い(503 Already started)。一度だけ止めて張り直す。
			triedRecover = true;
			stopEventPolling();
			meterSleep(kPollGapMs, keepGoing);
			continue;
		}

		if (ok) { anyPollOk = true; }
		else    { netThread::lastHttpFailure(diag.http, diag.body); diag.step = 6; }
		if (ok && !body.empty())
		{
			// {"addedcontents":["/ccapi/.../IMG_xxxx.CR3", ...], ...} を軽量に抽出(DOM化しない)。
			const size_t key = body.find("\"addedcontents\"");
			if (key != std::string::npos)
			{
				std::vector<std::string> names;
				size_t p = body.find('[', key);
				const size_t e = (p == std::string::npos) ? std::string::npos : body.find(']', p);
				while (p != std::string::npos && e != std::string::npos)
				{
					const size_t q1 = body.find('"', p + 1);
					if (q1 == std::string::npos || q1 > e) { break; }
					const size_t q2 = body.find('"', q1 + 1);
					if (q2 == std::string::npos || q2 > e) { break; }
					names.push_back(body.substr(q1 + 1, q2 - q1 - 1));
					p = q2;
				}
				std::string last = pickThumbPath(names);
				if (!last.empty())
				{
					// CCAPIのJSONはスラッシュを \/ とエスケープして返す。生抽出なので戻す
					// (戻さないと不正URLになりサムネ取得が404で全滅する。7/27実機で発生)。
					std::string un; un.reserve(last.size());
					for (size_t i = 0; i < last.size(); ++i)
					{
						if (last[i] == 0x5C && i + 1 < last.size() && last[i + 1] == '/') { continue; }
						un.push_back(last[i]);
					}
					diag = waitDiag{};	// 成功
					return un;
				}
			}
			// イベントはあったが addedcontents 無し(設定変更等)。
		}
		// 見つからなかった/失敗した → 必ず間隔を空ける(連打しない)。
		meterSleep(kPollGapMs, keepGoing);
	}
	// 通知は引けていたのに画像が現れなかった = カメラが撮影を完了していない疑い(step=7)。
	// 一度も引けていない = 通信の問題(step=6 のまま。上位はシャッター失敗の方で気づく)。
	if (anyPollOk) { diag.step = 7; }
	else if (diag.step == 0) { diag.step = 6; }
	return std::string();
}

// 測光の中核(露出非依存の部分): 新規画像の登録を待ち、サムネイルを取得・復号して
// 輝度ヒストグラム統計(中央値/p99/pMax/チェックサム)まで作る。
//  meterExp/sceneRef は呼び出し側(meterSceneShot)が撮影露出で確定する。
errCode apiCanonCCAPI::thumbMeterCore(meterResult& out, int budgetMs, const std::function<bool()>& keepGoing,
                                      const std::string& pathOverride)
{
	out = meterResult{};
	void* t0 = tool::startElapse();

	// ① 新しい画像の登録通知を待つ(カメラが露光後の記録を終えるまで)。
	//    pathOverride があるときは「どのファイルを測るか」が既に決まっているので待たない
	//    (ライブビュー主体方式が1コマ前のファイルを指定してくる経路)。
	int tries = 0;
	waitDiag diag;
	const std::string path = pathOverride.empty()
	                       ? waitAddedByEvent(budgetMs, keepGoing, tries, diag)
	                       : pathOverride;
	out.waitStep = diag.step;
	out.waitHttp = diag.http;
	out.waitBody = diag.body;
	out.tries  = tries;
	out.waitMs = static_cast<int>(tool::getElapse(t0));
	if (path.empty())
	{
		out.failStage = 1;
		out.rdyMs     = out.waitMs;
		// 通知そのものは取れていた(=カメラは応答している)のに、予算内で新しい画像が
		// 現れなかった。シャッターは受け付けたのに撮影が完了していない疑いを上位へ渡す。
		// 通信自体が失敗している場合(waitStep=1/6)は、撮れていないのか届いていないのかを
		// ここでは区別できないので申告しない(上位はシャッター失敗の方で気づく)。
		out.shotMissing = (diag.step == 7);
		return ERR_HGC_RDY_METARING;
	}

	const std::string base = apiHostBase();
	if (base.empty()) { out.failStage = 2; out.rdyMs = out.waitMs; return ERR_HGC_RDY_METARING; }

	// ② サムネイル取得 → ③ 復号。この2つは1組で予算内リトライする。
	//
	// 【なぜ復号失敗もリトライするか(2026-08-13 実機で判明)】記録直後のカメラは 503 で
	//  弾くことがあるのでリトライしていたが、**HTTP 200 なのに中身が復号できない**ケースが
	//  R10 で 63コマ中4回(約6%)出た。まだ書き終わっていない JPEG を掴んでいると考えられる。
	//  従来はこれを1回で諦めて測光失敗(露出据え置き)にしていた。取り直せば済むので取り直す。
	//  取得回数は R10 の固着予算に効くが、6%のためにコマを捨てる方が損である。
	void* tf = tool::startElapse();
	const std::string url = base + path + "?kind=thumbnail";
	std::string jpg;
	uint16_t hist[256];
	int  w = 0, h = 0;
	bool got = false, dec = false;
	int  decodeMs = 0;
	while (true)
	{
		++out.fetchTries;
		jpg.clear();
		got = (netThread::httpGet(url, jpg) && !jpg.empty());
		if (got)
		{
			void* td = tool::startElapse();
			dec = jpglm::lumaHistogram(reinterpret_cast<const uint8_t*>(jpg.data()), jpg.size(),
			                           hist, w, h, kThumbCropRatio);
			decodeMs += static_cast<int>(tool::getElapse(td));
			if (dec) { break; }
		}
		if (keepGoing && !keepGoing()) { break; }
		if (static_cast<int>(tool::getElapse(t0)) >= budgetMs) { break; }	// 総予算切れ
		meterSleep(kThumbFetchRetryMs, keepGoing);
	}
	out.fetchMs  = static_cast<int>(tool::getElapse(tf)) - decodeMs;	// 取得だけの時間
	out.decodeMs = decodeMs;
	out.rdyMs    = static_cast<int>(tool::getElapse(t0));
	if (!got) { out.failStage = 3; return ERR_HGC_RDY_METARING; }
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

// 撮影画像フィードバック測光。直前に撮れた最新画像のサムネイルから輝度を得る。
//  ・本露光の積分そのものなので、LVが頭打ちになる夜間でも真値が得られる(7/26実験: LVより約3.75段深い)。
//  ・露出には一切触れない(測光ss切替なし=appliedExp空・settleなし)。
//  ・呼び出しタイミングは meterTimingHint の申告どおり(露光終了直後)。captureRunner が従う。
//  ・shotExp = このサムネイルを撮った露出(呼び出し側の直前コマ)。sceneRef の割り戻しに使う。
// 【ライブビュー主体の測光】撮影済みサムネイルの取得回数に上限がある機種のための方式。
//
// 【なぜ要るか】サムネイル測光は本露光そのものを見るので最も正確だが、EOS R10 は撮影と対にした
//  サムネイル取得が電源投入あたり 200 回程度で応答しなくなる(2026-08-12 実測。1コマ前を取る
//  形でも 236〜243 回)。15秒周期なら1時間で 240 コマなので、毎コマ取ると一晩持たない。
//  一方ライブビューは何回読んでも減らないが、積分の上限(ここでは測光ss 0.5秒)があるため
//  暗くなると中央値が黒に張り付き、そこから先の明るさが見えなくなる。
//
// 【方針】ふだんはライブビューで測り、**ライブビューが底に張り付いて抜けられないときだけ**
//  サムネイルへ落ちる。落ちている間も毎コマは取らず kLvFallbackEveryN コマに1回だけ取り、
//  間のコマは直近の値を返す(明るさは 15 秒で 0.03 段程度しか動かないので足りる)。
//  実測(2026-08-14、撮影画像との突き合わせ)では、夕方の薄明は夜間露出へクランプされるので
//  サムネイルが要る区間はほぼ無く、夜明けだけが対象で 200 コマ程度。4コマに1回なら
//  50 回前後に収まり、R10 の予算内で一晩通せる計算になる。
//
// 【1コマ前を取る理由】生成直後のファイルに触ると R10 は 4/4 で停止した(2026-08-13)。
//  そこで登録通知は毎コマ拾ってファイル名だけ覚えておき、測るのは常に1つ前のファイルにする。
errCode apiCanonCCAPI::meterSceneLvFirst(const hgc::exposure& shotExp, meterResult& out,
                                         const std::function<bool()>& keepGoing)
{
	// ① 登録通知を拾ってファイル名の並びを進める(測光そのものはまだしない)。
	//    サムネイルは取らないので、ここは回数の予算を消費しない。
	{
		int tries = 0;
		waitDiag diag;
		const std::string p = waitAddedByEvent(kLvNotifyBudgetMs, keepGoing, tries, diag);
		if (!p.empty() && p != lvShotLast_)
		{
			lvShotPrev_ = lvShotLast_;	// 1つ前へ送る
			lvShotLast_ = p;
		}
	}

	// ② ライブビューで測る。ここが本線。
	//    **撮影露出のまま測る**のが基本。ライブビューは積分の上限(kInitMeterMaxSsSec)を
	//    超える ss を再現できないので、それより長い撮影露出のときだけ測光専用の露出へ
	//    乗せ替える(乗せ替えると1コマごとに ss が動いて見えるうえ、追従待ちも要る)。
	hgc::exposure seed;
	if (validExp(shotExp) &&
	    expo::parseValue(shotExp.ss, expo::expoKind::ss) <= kInitMeterMaxSsSec)
	{
		seed = shotExp;
	}
	meterResult lv;
	const errCode le = this->meterLvAt(seed, lv, keepGoing);
	// ライブビューが「見えていない」判定: 測れてはいるが中央値が底に張り付いていて、
	//  しかも測光ss を伸ばしきっている(=これ以上積分できない)。この状態の値は
	//  場面の明るさではなく「LVの下限」なので、そのまま使うと露出が追随しなくなる。
	const double meterSs = expo::parseValue(lv.meterExp.ss, expo::expoKind::ss);
	const bool   lvBlind = (le != ERR_HGC_OK) || !lv.usable
	                    || (lv.x <= kPegDark && meterSs >= kInitMeterMaxSsSec * 0.99);
	if (!lvBlind)
	{
		out = lv;
		lvFallbackSkip_ = 0;	// 明るさが戻ったので、次に暗くなったら即サムネイルを取る
		return ERR_HGC_OK;
	}

	// ③ ライブビューでは足りない。サムネイルへ落ちる(間引きあり)。
	//    間引き中のコマは直近の場面基準をそのまま返す。上位はこれを目標に1コマぶんずつ
	//    近づくので、値を据え置いても露出は滑らかに動く。
	if (lvFallbackSkip_ > 0 && lvHeldSceneRef_ > 0.0)
	{
		--lvFallbackSkip_;
		out           = lv;			// 診断値(ヒスト等)はライブビューのものを残す
		out.ok        = true;
		out.usable    = true;
		out.failStage = 0;
		out.sceneRef  = lvHeldSceneRef_;
		out.meterExp  = shotExp;
		return ERR_HGC_OK;
	}

	// 1コマ前が分かっていなければ取りようがない(セッションの最初の1コマ)。
	//  ライブビューの値をそのまま使う(暗い側へ張り付いた値だが、次のコマで取り直せる)。
	if (lvShotPrev_.empty())
	{
		out = lv;
		if (!lv.usable) { return (le == ERR_HGC_OK) ? ERR_HGC_RDY_METARING : le; }
		return ERR_HGC_OK;
	}

	meterResult th;
	const errCode te = thumbMeterCore(th, kLvFallbackBudgetMs, keepGoing, lvShotPrev_);
	if (te != ERR_HGC_OK || !th.ok)
	{	// 取れなかった。ライブビューの値で凌ぐ(次のコマでまた取りに行く)。
		out = lv;
		return (lv.usable) ? ERR_HGC_OK : ((le == ERR_HGC_OK) ? ERR_HGC_RDY_METARING : le);
	}
	// 1コマ前の画像なので、割り戻す露出も「そのコマの露出」でなければならない。
	//  撮影周期の間に露出を動かしているとズレるが、暗所ではクランプが効いていて
	//  ほとんど動かないため、直前コマの露出(shotExp)で足りる。ズレるようなら
	//  コマごとの露出を覚えて渡す形に直す(実験で見る)。
	th.meterExp     = shotExp;
	th.sceneRef     = th.linear / std::pow(2.0, expo::brightnessStops(shotExp, tables_));
	lvHeldSceneRef_ = th.sceneRef;
	lvFallbackSkip_ = kLvFallbackEveryN - 1;
	++lvFallbackShots_;
	out = th;
	return ERR_HGC_OK;
}

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
