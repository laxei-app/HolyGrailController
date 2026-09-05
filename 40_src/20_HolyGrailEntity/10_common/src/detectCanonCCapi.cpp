#include "common.h"
#include "detectCanonCCapi.h"
#include "apiCanonCCAPI.h"
#include "netThread.h"
#include "httpAuth.h"
#include <cstdio>

namespace
{
	// httpAuth が host を覚えるときの鍵と同じ形("http://ip:port")。net.cpp の endpointOf と揃える。
	std::string endpointOf(const std::string& url)
	{
		const size_t p = url.find("://");
		if (p == std::string::npos) { return url; }
		const size_t s = url.find('/', p + 3);
		return url.substr(0, (s == std::string::npos) ? url.size() : s);
	}
}

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

// 挨拶(2026-08-28 の知見を在否監視からここへ移動 2026-09-06)。
//
// 【何が必要か(実機 EOS R50 V で確定)】
//  ・SSDP や UPnP の記述子(:49152)では**足りない**。記述子を出しているのは別のサービス
//  ・CCAPI(:8080)へ**届くだけでも足りない**。認証なしで叩いて 401 を貰っても変わらなかった
//  ・**200 が返る CCAPI アクセス**で「接続が完了しました」に変わった
//  機器情報を1回読むだけ。軽く、どの機種にもあり、こちらにも有用な内容が返る。
//
// 【まず素で投げる(2026-09-05 ユーザー指示)】認証を使わない設定のカメラでは素の GET が 200 になる。
//  net の GET は素で投げ、401 を受けたときだけ認証を付けて1度だけ投げ直す。
//
// 【やめどき】403 = カメラが締め出した。投げ直すほど悪くなるので即やめる(戻すには本体の接続設定を
//  入れ直すしかない)。401 なのに手持ちの資格情報が1つも無い = 続けると 403 になる。1回でやめる。
//  それ以外(届かない・nc の追いつき待ち)は在否監視が間隔をあけて頼み直す。
detectBase::greetResult detectCanonCCapi::greet(const class device& d)
{
	greetResult r;
	if (d.urlAccess.empty()) { r.giveUp = true; r.detail = "no access url"; return r; }
	std::string resp;
	if (netThread::httpGet(d.urlAccess + "/ver100/deviceinformation", resp)) { r.done = true; return r; }
	int st = 0; std::string body;
	netThread::lastHttpFailure(st, body);
	if      (st == 403)                                  { r.giveUp = true; }
	else if (st == 401 && !httpAuth::hasCandidates())    { r.giveUp = true; }
	char b[200];
	std::snprintf(b, sizeof(b), "http=%d %s [%s]", st, d.urlAccess.c_str(),
	              httpAuth::diagnose(endpointOf(d.urlAccess)).c_str());
	r.detail = b;
	return r;
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
