#include "httpAuth.h"
#include "md5.h"
#include <cstdio>
#include <ctime>
#include <map>
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
	};

	std::mutex                                          g_mtx;	// ワーカー2本から触られる
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
	}

	void addCandidate(const std::string& user, const std::string& pass)
	{
		if (user.empty() && pass.empty()) { return; }
		std::lock_guard<std::mutex> lk(g_mtx);
		for (const auto& e : g_creds) { if (e.first == user && e.second == pass) { return; } }
		g_creds.emplace_back(user, pass);
		// 学習済みの内容は消さない。候補が増えただけで、通っている相手の認証は生きている。
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
		if (st.known && st.nonce == nonce)
		{	// 同じ nonce で弾かれた = この資格情報では通らない。次の候補へ。
			if (++st.credIdx >= g_creds.size()) { st.credIdx = 0; st.known = false; return false; }
		}
		st.realm     = param(wwwAuthenticate, "realm");
		st.nonce     = nonce;
		st.opaque    = param(wwwAuthenticate, "opaque");
		st.qop       = param(wwwAuthenticate, "qop");
		st.algorithm = param(wwwAuthenticate, "algorithm");
		st.nc        = 0;
		st.known     = true;
		return true;
	}

	std::string authorization(const std::string& host, const std::string& method, const std::string& uri)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		auto it = g_hosts.find(host);
		if (it == g_hosts.end() || !it->second.known || g_creds.empty()) { return std::string(); }
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
