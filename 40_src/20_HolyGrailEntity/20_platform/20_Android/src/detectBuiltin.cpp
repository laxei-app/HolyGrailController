#include "detectBuiltin.h"
#include "apiBuiltin.h"
#include "builtinBridge.h"
#include "device.h"
#include "dataManager.h"
#include <json/nlohmann/json.hpp>

class apiBase* detectBuiltin::makeApi(void)
{
	return new apiBuiltin();
}

// 端末が持つカメラを1台ずつ device に起こす。
//
// 【urlAccess にカメラ id を入れる】他のバックエンドはここに CCAPI の URL を入れている。
//  内蔵カメラに URL は無いが、「その1台へ届くための宛先」という役目は同じなので id を入れる。
//  apiBuiltin::init はここから id を受け取る。上位はこの中身を解釈しない。
//
// 【want で1台に絞る】欲しい1台が決まっているときは、合わない台の apiBase を作らない。
//  内蔵カメラは軽いので実害は小さいが、他のバックエンドと同じ約束にしておく。
size_t detectBuiltin::build(std::vector<class device>& out, const deviceMatch& want, bool withApi)
{
	const std::string listed = builtinCam::listJson();
	nlohmann::json j = nlohmann::json::parse(listed, nullptr, false);
	if (j.is_discarded() || !j.is_array()) { return 0; }

	// 【一度だけ台数を残す】列挙は探索のたびに走るので毎回は書かない。
	//  端末によって何台見えるかが違うため、最初の1回だけログに残して後から確かめられるようにする。
	static bool s_logged = false;
	if (!s_logged)
	{
		s_logged = true;
		dataManager::logEvent("CAMERA", ("builtin cameras: " + listed).c_str());
		// 一覧に出ないカメラ(超広角など)が何台ぶら下がっているかも残す。使うかどうかの判断材料。
		dataManager::logEvent("CAMERA", ("builtin physicals: " + builtinCam::physicalsJson()).c_str());
	}

	size_t added = 0;
	for (const auto& e : j)
	{
		if (!e.is_object()) { continue; }
		const std::string id = e.value("id", std::string());
		if (id.empty()) { continue; }

		class device d;
		d.apiClass     = device::apiClass::NON;	// 内蔵カメラは既存のどれでもない
		d.urlAccess    = id;					// 宛先=カメラ id
		d.manufacturer = "builtin";
		d.model        = e.value("name", std::string("Built-in camera"));
		d.assignedName = d.model;
		d.serialno     = std::string(apiBuiltin::kSerialPrefix) + id;
		d.uuid         = d.serialno;

		// 記述だけで判定してもらう(apiBase を作る前に絞り込む約束)。
		if (want && !want(d)) { continue; }

		if (withApi)
		{
			apiBase* api = this->makeApi();
			if (api == nullptr) { continue; }
			// init が諸元を読み、model/serialno を確定させ、設定可能値のテーブルを合成する。
			if (api->init(d) != ERR_HGC_OK) { delete api; continue; }
			d.apiBase.reset(api);
		}
		out.push_back(d);
		++added;
		if (want) { break; }	// 1台に絞る指定なら見つけ次第打ち切る
	}
	return added;
}

size_t detectBuiltin::detect(std::vector<class device>& out, const deviceMatch& want)
{
	return this->build(out, want, true);
}

size_t detectBuiltin::identify(std::vector<class device>& out)
{
	// 身元だけ。内蔵カメラは触っても害が無いが、apiBase を作らないぶん軽い。
	return this->build(out, nullptr, false);
}
