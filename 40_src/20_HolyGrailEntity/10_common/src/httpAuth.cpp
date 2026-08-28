#include "httpAuth.h"
#include "md5.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>

// 流れと方針はヘッダの説明を参照。
namespace
{
	struct hostState
	{
		std::string realm, nonce, opaque, qop, algorithm;
		uint32_t    nc      = 0;		// この nonce で何回目か(RFC は1から)
		size_t      credIdx = 0;		// いま使っている資格情報の候補
		bool        known   = false;	// 401 を受けて中身を覚えたか
		bool        proven  = false;	// この資格情報で一度でも通ったか(誤判定の防止)
		bool        exhausted = false;	// 候補を全部試して全滅した(資格情報が直るまで打ち止め)
		bool        bumped    = false;	// nc を大きく飛ばして試したか(この nonce につき1回)
	};

	std::mutex                                          g_mtx;	// ワーカー2本から触られる
	std::map<std::string, std::unique_ptr<std::mutex>>  g_locks;	// host ごとの直列化用
	std::vector<std::pair<std::string, std::string>>    g_creds;
	std::map<std::string, hostState>                    g_hosts;

	// WWW-Authenticate から name= の値を取り出す。値は "..." でも裸でもよい(qop=auth 等)。
	std::string param(const std::string& src, const std::string& name)
	{
		size_t p = 0;
		while ((p = src.find(name, p)) != std::string::npos)
		{
			// 直前が英数字なら別の語の一部(例: "nonce" を "cnonce" の中に見つけた)
			if (p > 0)
			{
				const char b = src[p - 1];
				if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9')) { p += name.size(); continue; }
			}
			size_t q = p + name.size();
			while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) { ++q; }
			if (q >= src.size() || src[q] != '=') { p += name.size(); continue; }
			++q;
			while (q < src.size() && (src[q] == ' ' || src[q] == '\t')) { ++q; }
			if (q < src.size() && src[q] == '"')
			{
				const size_t e = src.find('"', q + 1);
				if (e == std::string::npos) { return std::string(); }
				return src.substr(q + 1, e - q - 1);
			}
			size_t e = q;
			while (e < src.size() && src[e] != ',' && src[e] != ' ' && src[e] != '\r' && src[e] != '\n') { ++e; }
			return src.substr(q, e - q);
		}
		return std::string();
	}

	// RFC 2617 の stale。true なら「nonce が古いだけで資格情報は正しい」。
	bool isStale(const std::string& src)
	{
		std::string v = param(src, "stale");
		for (auto& c : v) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
		return v == "true";
	}

	const long long kEpoch2020 = 1577836800LL;	// 2020-01-01T00:00:00Z

	// 時計が入っているか。未設定のうちは nc を時刻から作れない。
	bool clockReady(void) { return static_cast<long long>(std::time(nullptr)) > kEpoch2020; }

	// nc の初期値を時刻から作る。理由はヘッダの説明を参照。
	//  2020-01-01 からの秒 × 8。×8 は「毎秒8リクエストまでなら、次に起動したときの種のほうが
	//  前回の最後の nc より必ず大きい」ための余裕(実際の撮影は毎秒1リクエストにも満たない)。
	//  32bit(nc は16進8桁)に収まり、2037年ごろまで持つ。
	uint32_t nowSeed(void)
	{
		const long long now = static_cast<long long>(std::time(nullptr));
		if (!clockReady()) { return 0; }				// 時計が未設定(呼ぶ側で弾く)
		const long long v = (now - kEpoch2020) * 8;
		if (v > 0x7fffffffLL) { return 0x7fffffffu; }		// 念のため上限で止める
		return static_cast<uint32_t>(v);
	}

	// クライアント側の使い捨て文字列。暗号学的な乱数である必要はない(再利用しなければよい)。
	std::string makeCnonce(void)
	{
		static uint32_t seq = 0;
		char b[24];
		std::snprintf(b, sizeof(b), "%08x%08x",
		              static_cast<unsigned>(std::time(nullptr)), static_cast<unsigned>(++seq));
		return std::string(b);
	}
}

namespace httpAuth
{
	void setCandidates(const std::vector<std::pair<std::string, std::string>>& userPass)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_creds.clear();
		for (const auto& up : userPass)
		{
			if (up.first.empty() && up.second.empty()) { continue; }	// 未設定は候補にしない
			bool dup = false;
			for (const auto& e : g_creds) { if (e.first == up.first && e.second == up.second) { dup = true; break; } }
			if (!dup) { g_creds.push_back(up); }
		}
		g_hosts.clear();	// 資格情報が変わったら、どれが通ったかの記憶も無効
		// g_locks は消さない。使用中の可能性があるうえ、host ごとに1つあれば足りる。
	}

	void addCandidate(const std::string& user, const std::string& pass)
	{
		if (user.empty() && pass.empty()) { return; }
		std::lock_guard<std::mutex> lk(g_mtx);
		for (const auto& e : g_creds) { if (e.first == user && e.second == pass) { return; } }
		g_creds.emplace_back(user, pass);
		// 学習済みの内容は消さない。候補が増えただけで、通っている相手の認証は生きている。
		// ただし打ち止めは解除する。新しい候補で通るかもしれない。
		for (auto& e : g_hosts) { e.second.exhausted = false; }
	}

	bool hasCandidates(void)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		return !g_creds.empty();
	}

	bool learn(const std::string& host, const std::string& wwwAuthenticate)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		if (g_creds.empty()) { return false; }		// 打つ手が無い

		const std::string nonce = param(wwwAuthenticate, "nonce");
		if (nonce.empty()) { return false; }			// ダイジェストではない(Basic 等)

		hostState& st = g_hosts[host];
		if (st.exhausted) { return false; }		// 打ち止め。誤った資格情報を投げ続けない
		const bool sameNonce = (st.known && st.nonce == nonce);
		bool keepCount = false;		// nc を維持するか(巻き戻すとリプレイ扱いになる)

		if (sameNonce)
		{
			// 同じ nonce で弾かれた。理由は2つあり、取り違えると復帰できなくなる。
			//  (a) nonce が古いだけ(stale)、または nc の順番が乱れた → 資格情報は正しい
			//  (b) 本当に資格情報が違う
			// 一度でも通った資格情報なら (a)。**ここで nc を 0 に戻してはいけない**。
			// カメラは使用済みの nc をリプレイと見なし、以後どれだけ正しく作っても 401 を返す
			// (EOS R50 V 実測)。
			if (isStale(wwwAuthenticate) || st.proven)
			{
				keepCount = true;
			}
			else if (!st.bumped)
			{
				// まだ一度も通っていないが、資格情報が違うと決めつける前に nc を疑う。
				//  カメラは nonce を使い回して nc を覚えているので、こちらが起動し直した直後は
				//  「前回の続きより小さい nc」を送ってしまうことがある(時刻の種が追いつかない場合)。
				//  飛びは許されるので、大きく飛ばして1回だけ試す。これで通れば nc の問題だった。
				st.bumped = true;
				st.nc    += (1u << 20);
				keepCount = true;
			}
			else if (++st.credIdx >= g_creds.size())
			{
				// 全滅。ここで状態を捨てて最初の候補からやり直すと、誤った資格情報での認証失敗を
				//  延々と投げ続けることになる。カメラによってはそれで完全に締め出される
				//  (EOS R50 V は 403 "Not access" になり、本体の設定を入れ直すまで戻らない)。
				//  資格情報が変わる(setCandidates/addCandidate)まで打ち止めにする。
				st.credIdx = 0; st.proven = false; st.bumped = false; st.exhausted = true;
				return false;
			}
			else
			{
				st.proven = false;		// 別の資格情報に乗り換える。実績は引き継がない
				st.bumped = false;
			}
		}
		else
		{
			st.proven = false;			// nonce が変わった。改めて通るか確かめる
			st.bumped = false;
		}

		st.realm     = param(wwwAuthenticate, "realm");
		st.nonce     = nonce;
		st.opaque    = param(wwwAuthenticate, "opaque");
		st.qop       = param(wwwAuthenticate, "qop");
		st.algorithm = param(wwwAuthenticate, "algorithm");
		// 数え直してよいのは nonce か資格情報が変わったときだけ。0 ではなく時刻から始めるのは、
		//  カメラが nonce を使い回して nc を**プロセスをまたいで**覚えているため(ヘッダ参照)。
		if (!keepCount) { st.nc = nowSeed(); }
		st.known     = true;
		return true;
	}

	std::string authorization(const std::string& host, const std::string& method, const std::string& uri)
	{
		// 【時計が未設定のうちは認証付きで触らない(2026-08-28 実測に基づく)】
		//  カメラは nc(ノンスカウンタ)を **nonce をまたいで一本で** 覚えており、前回受け入れた
		//  値より大きくないと通さない(実機 EOS R50 V で確認)。端末の区別はしていない。
		//  こちらは nc の種を時刻から作ることで「後から来た方が必ず大きい」を成立させ、
		//  スマホとエッジのどちらからでも順番に使えるようにしている。
		//  ところが時計が未設定だと種が 0 になり、他の端末が一度触った後は**何度やっても
		//  追いつけない**。しかも端末側からは「認証が通らない」としか見えず追いにくい。
		//  送らなければ nc に触らないので、カメラの状態を汚さずに済む。時刻が入れば通る。
		if (!clockReady()) { return std::string(); }

		std::lock_guard<std::mutex> lk(g_mtx);
		auto it = g_hosts.find(host);
		if (it == g_hosts.end() || !it->second.known || g_creds.empty()) { return std::string(); }
		if (it->second.exhausted) { return std::string(); }	// 通らないと分かっている物は送らない
		hostState& st = it->second;
		if (st.credIdx >= g_creds.size()) { return std::string(); }
		const std::string& user = g_creds[st.credIdx].first;
		const std::string& pass = g_creds[st.credIdx].second;

		// qop に複数(auth,auth-int)が来ることがある。こちらは auth しか実装しない。
		std::string qop = st.qop;
		if (qop.find("auth-int") != std::string::npos && qop.find("auth,") == std::string::npos) { qop = "auth"; }
		if (!qop.empty()) { qop = "auth"; }

		char ncBuf[16] = {0};
		std::string cnonce;
		if (!qop.empty())
		{
			std::snprintf(ncBuf, sizeof(ncBuf), "%08x", static_cast<unsigned>(++st.nc));
			cnonce = makeCnonce();
		}
		const std::string resp = responseHash(user, pass, st.realm, st.nonce, method, uri,
		                                      qop, ncBuf, cnonce);

		std::string h = "Digest username=\"" + user + "\", realm=\"" + st.realm +
		                "\", nonce=\"" + st.nonce + "\", uri=\"" + uri + "\"";
		if (!st.algorithm.empty()) { h += ", algorithm=" + st.algorithm; }
		if (!qop.empty())
		{
			h += ", qop=" + qop;
			h += ", nc=" + std::string(ncBuf);
			h += ", cnonce=\"" + cnonce + "\"";
		}
		h += ", response=\"" + resp + "\"";
		if (!st.opaque.empty()) { h += ", opaque=\"" + st.opaque + "\""; }
		return h;
	}

	void noteSuccess(const std::string& host)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		auto it = g_hosts.find(host);
		if (it != g_hosts.end()) { it->second.proven = true; }
	}

	hostGuard::hostGuard(const std::string& host)
	{
		std::mutex* m = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			if (g_creds.empty()) { return; }	// 認証を使わない構成では何もしない(既存の動作のまま)
			auto& slot = g_locks[host];
			if (!slot) { slot.reset(new std::mutex()); }
			m = slot.get();
		}
		m->lock();				// g_mtx は手放してから待つ(取得順を固定して死錠を避ける)
		host_ = host; held_ = true;
	}

	hostGuard::~hostGuard()
	{
		if (!held_) { return; }
		std::mutex* m = nullptr;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			auto it = g_locks.find(host_);
			if (it != g_locks.end()) { m = it->second.get(); }
		}
		if (m != nullptr) { m->unlock(); }
	}

	void reset(const std::string& host)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_hosts.erase(host);
	}

	void resetAll(void)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_hosts.clear();
	}

	std::string responseHash(const std::string& user, const std::string& pass,
	                         const std::string& realm, const std::string& nonce,
	                         const std::string& method, const std::string& uri,
	                         const std::string& qop, const std::string& nc,
	                         const std::string& cnonce)
	{
		const std::string ha1 = md5::hex(user + ":" + realm + ":" + pass);
		const std::string ha2 = md5::hex(method + ":" + uri);
		if (qop.empty())
		{	// RFC 2069(qop 無し)
			return md5::hex(ha1 + ":" + nonce + ":" + ha2);
		}
		return md5::hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
	}
}
