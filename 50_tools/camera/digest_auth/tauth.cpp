#include "httpAuth.h"
#include "md5.h"
#include <cstdio>
#include <string>

static int fails = 0;
static void chk(bool ok, const char* what)
{
	if (!ok) { ++fails; std::printf("FAIL %s\n", what); } else { std::printf("ok   %s\n", what); }
}
static std::string field(const std::string& h, const std::string& k)
{
	const size_t p = h.find(k + "=");
	if (p == std::string::npos) { return ""; }
	size_t q = p + k.size() + 1;
	if (q < h.size() && h[q] == '"') { const size_t e = h.find('"', q + 1); return h.substr(q + 1, e - q - 1); }
	const size_t e = h.find_first_of(", ", q);
	return h.substr(q, (e == std::string::npos ? h.size() : e) - q);
}

int main()
{
	// CCAPI Reference 3.3.3 と同じ形のチャレンジ
	const std::string chal = "Digest realm=\"CameraControlApi\", nonce=\"abc123nonce\", "
	                         "domain=\"/ccapi\", opaque=\"op4567\", algorithm=MD5, qop=\"auth\"";

	// ① 候補が無ければ学べない(打つ手が無いので false)
	httpAuth::resetAll();
	httpAuth::setCandidates({});
	chk(!httpAuth::hasCandidates(), "no candidates");
	chk(!httpAuth::learn("192.168.1.7:8080", chal), "learn without candidates fails");
	chk(httpAuth::authorization("192.168.1.7:8080", "GET", "/ccapi").empty(), "no header without candidates");

	// ② addCandidate: 空は無視、重複は増えない
	httpAuth::addCandidate("", "");
	chk(!httpAuth::hasCandidates(), "empty candidate ignored");
	httpAuth::addCandidate("user1", "pw1");
	httpAuth::addCandidate("user1", "pw1");
	chk(httpAuth::hasCandidates(), "candidate added");

	// ③ 401 を学んで Authorization を作る
	const std::string host = "192.168.1.7:8080";
	chk(httpAuth::learn(host, chal), "learn challenge");
	const std::string h1 = httpAuth::authorization(host, "GET", "/ccapi/ver100/shooting/settings");
	chk(!h1.empty(), "header built");
	chk(field(h1, "username") == "user1", "username");
	chk(field(h1, "realm") == "CameraControlApi", "realm");
	chk(field(h1, "nonce") == "abc123nonce", "nonce");
	chk(field(h1, "opaque") == "op4567", "opaque");
	chk(field(h1, "algorithm") == "MD5", "algorithm");
	chk(field(h1, "qop") == "auth", "qop");
	chk(!field(h1, "nc").empty(), "nc is present");
	chk(field(h1, "uri") == "/ccapi/ver100/shooting/settings", "uri");

	// response を独立に計算して一致するか
	{
		const std::string ha1 = md5::hex("user1:CameraControlApi:pw1");
		const std::string ha2 = md5::hex("GET:/ccapi/ver100/shooting/settings");
		const std::string want = md5::hex(ha1 + ":abc123nonce:" + field(h1, "nc") + ":" + field(h1, "cnonce") + ":auth:" + ha2);
		chk(field(h1, "response") == want, "response digest");
	}

	// ④ nc が増える / cnonce が毎回変わる
	const std::string h2 = httpAuth::authorization(host, "GET", "/ccapi");
	chk(std::stoul(field(h2, "nc"), nullptr, 16) == std::stoul(field(h1, "nc"), nullptr, 16) + 1, "nc increments");
	chk(field(h2, "cnonce") != field(h1, "cnonce"), "cnonce varies");

	// ⑤ 同じ nonce で再び 401 → 候補が1つしかないので使い切って false
	chk(httpAuth::learn(host, chal), "same nonce: first suspect nc (bump)");
	httpAuth::authorization(host, "GET", "/x");
	chk(!httpAuth::learn(host, chal), "then give up with only one credential");

	// ⑥ 候補が2つあれば2つ目へ進む
	httpAuth::resetAll();
	httpAuth::setCandidates({{"bad", "bad"}, {"good", "good"}});
	chk(httpAuth::learn(host, chal), "learn (2 candidates)");
	chk(field(httpAuth::authorization(host, "GET", "/x"), "username") == "bad", "first candidate");
	chk(httpAuth::learn(host, chal), "same nonce -> bump nc first");
	httpAuth::authorization(host, "GET", "/x");
	chk(httpAuth::learn(host, chal), "then next candidate");
	chk(field(httpAuth::authorization(host, "GET", "/x"), "username") == "good", "second candidate");
	httpAuth::authorization(host, "GET", "/x");
	chk(httpAuth::learn(host, chal), "bump for the second candidate too");
	httpAuth::authorization(host, "GET", "/x");
	chk(!httpAuth::learn(host, chal), "exhausted");

	// ⑦ nonce が変わったら nc は 1 に戻る(RFC 2617 3.2.2)
	httpAuth::resetAll();
	httpAuth::setCandidates({{"u", "p"}});
	httpAuth::learn(host, chal);
	httpAuth::authorization(host, "GET", "/a");
	httpAuth::learn(host, "Digest realm=\"CameraControlApi\", nonce=\"NEWnonce\", qop=\"auth\"");
	chk(!field(httpAuth::authorization(host, "GET", "/a"), "nc").empty(), "nc is re-seeded on new nonce");

	// ⑧ ホストごとに別管理
	chk(httpAuth::authorization("192.168.1.12:8080", "GET", "/a").empty(), "other host untouched");

	// ⑨ ダイジェストでない(Basic)は学ばない
	httpAuth::resetAll();
	chk(!httpAuth::learn(host, "Basic realm=\"x\""), "basic not learned");

	// ⑩ qop 無し(RFC 2069)
	httpAuth::resetAll();
	httpAuth::setCandidates({{"u", "p"}});
	httpAuth::learn(host, "Digest realm=\"R\", nonce=\"N\"");
	{
		const std::string h = httpAuth::authorization(host, "GET", "/a");
		chk(h.find("qop=") == std::string::npos, "no qop emitted");
		chk(h.find("nc=") == std::string::npos, "no nc emitted");
		const std::string want = md5::hex(md5::hex("u:R:p") + ":N:" + md5::hex("GET:/a"));
		chk(field(h, "response") == want, "RFC2069 response");
	}

	// ⑪ setCandidates は学習内容を捨てる(資格情報が変わったら記憶は無効)
	httpAuth::setCandidates({{"z", "z"}});
	chk(httpAuth::authorization(host, "GET", "/a").empty(), "setCandidates clears learned");

	std::printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	return fails;
}
