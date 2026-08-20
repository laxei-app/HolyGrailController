#include "common.h"
#include "detectCanonCCapi.h"
#include "apiCanonCCAPI.h"

// このバックエンドが対応するサービス定義(キーワード→apiClass)。
// 旧 deviceDiscovery の静的テーブルをそのまま移設(挙動不変)。
//  ・sony(DigitalImaging)はまだ専用 interface が無いので暫定で CANON_CCAPI に割り当て。
//    将来 Sony バックエンドを追加する際にこの行は Sony 側へ移す(本改修外・構造のみ)。
const std::vector<deviceDiscovery::definitionIntereface>& detectCanonCCapi::interfaces() const
{
	static const std::vector<deviceDiscovery::definitionIntereface> ifaces =
	{
		{ {"ICPO-CameraControlAPIService","schemas-canon-com"}, device::apiClass::CANON_CCAPI },
		{ {"DigitalImaging","schemas-sony-com"},                device::apiClass::CANON_CCAPI },
	};
	return ifaces;
}

// Canon CCAPI の apiBase を生成する(未 init)。
class apiBase* detectCanonCCapi::makeApi()
{
	return new apiCanonCCAPI();
}

// IP 直指定で身元だけ確かめる(記述XMLのみ。CCAPIは叩かない)。
//
// 【なぜ要るか(2026-08-20)】在否監視は SSDP だけで見ていたので、カメラが SSDP を止めると
//  APに繋がったままでも「居ない」と判定していた(実機 EOS R100: M-SEARCH 5回に応答0)。
//  一方で撮影開始の探索は接続局IPを直接叩く経路を持ち、同じ状況でも見つけられる。
//  在否と撮影で見え方が食い違うので、在否にも同じ手掛かりを使わせる。
//
// 【URLについて】キヤノン機の UPnP 記述は http://<ip>:49152/upnp/CameraDevDesc.xml に置かれる
//  (EOS R50 V 2台で実測)。機種依存の知識なのでこのバックエンドに閉じる。外れたら false を
//  返すだけで、従来どおり SSDP の結果に委ねる。
bool detectCanonCCapi::identifyAt(const std::string& host, class device& out)
{
	out.clear();
	out.apiClass = device::apiClass::CANON_CCAPI;
	out.location = "http://" + host + ":49152/upnp/CameraDevDesc.xml";

	apiCanonCCAPI api;
	if (api.identify(out) != ERR_HGC_OK) { return false; }
	if (out.serialno.empty()) { return false; }	// 身元が割れないものは在否に使えない
	out.apiBase = nullptr;	// CCAPI は作らない(在否は触らない側)
	return true;
}

// IP 直指定で CCAPI カメラを構築する(SSDP不使用。エミュレータ/広告停止カメラ対策)。
bool detectCanonCCapi::makeManualDevice(const std::string& host, class device& out)
{
	out.clear();
	out.apiClass = device::apiClass::CANON_CCAPI;
	out.location = host;
	out.urlAccess = "http://" + host + ":8080/ccapi";	// CCAPI のアクセスURL
	out.model = "Canon CCAPI (manual)";

	auto api = std::make_shared<apiCanonCCAPI>();
	if (api->initManual(out) != ERR_HGC_OK)
	{
		return false;	// api(shared_ptr)はスコープ離脱で自動解放
	}
	out.apiBase = api;	// 派生→基底の shared_ptr へ暗黙変換(共有所有)
	return true;
}
