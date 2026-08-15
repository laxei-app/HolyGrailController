// 実カメラ(EOS R50 V)が返したチャレンジそのもので httpAuth を検証する。
//  空白なしのカンマ区切り、algorithm はクォート無し、domain に "://" を含む。
#include "httpAuth.h"
#include "md5.h"
#include <cstdio>
#include <string>
static int fails = 0;
static void chk(bool ok, const char* w){ if(!ok){++fails;std::printf("FAIL %s\n",w);} else std::printf("ok   %s\n",w); }
static std::string f(const std::string& h, const std::string& k)
{
	const size_t p = h.find(k + "=");
	if (p == std::string::npos) { return ""; }
	size_t q = p + k.size() + 1;
	if (q < h.size() && h[q] == '"') { const size_t e = h.find('"', q + 1); return h.substr(q + 1, e - q - 1); }
	const size_t e = h.find_first_of(", ", q);
	return h.substr(q, (e == std::string::npos ? h.size() : e) - q);
}
int main(int argc, char** argv)
{
	if (argc < 3) { std::printf("usage: treal <user> <pass>\n"); return 2; }
	// 実機が返した WWW-Authenticate をそのまま貼る
	const std::string chal =
	  "Digest realm=\"CameraControlApi\",nonce=\"16805f0b2a8151fbadc71b8fdf3890d888701603\","
	  "domain=\"http://192.168.1.16:8080/ccapi\",opaque=\"0123456789abcdef0123456789\","
	  "algorithm=MD5,qop=\"auth\"";
	const std::string host = "192.168.1.16:8080";
	const std::string uri  = "/ccapi/ver100/deviceinformation";

	httpAuth::resetAll();
	httpAuth::setCandidates({ {argv[1], argv[2]} });
	chk(httpAuth::learn(host, chal), "実機のチャレンジを解釈できる");
	const std::string h = httpAuth::authorization(host, "GET", uri);
	std::printf("Authorization: %s\n", h.c_str());

	chk(f(h,"realm")  == "CameraControlApi", "realm");
	chk(f(h,"nonce")  == "16805f0b2a8151fbadc71b8fdf3890d888701603", "nonce(空白なし区切りを読める)");
	chk(f(h,"opaque") == "0123456789abcdef0123456789", "opaque");
	chk(f(h,"algorithm") == "MD5", "algorithm(クォート無しを読める)");
	chk(f(h,"qop")    == "auth", "qop");
	chk(!f(h,"nc").empty(), "nc がある(値は時刻由来なので固定値では見ない)");
	chk(f(h,"uri")    == uri, "uri");
	chk(h.find("192.168.1.16:8080/ccapi") == std::string::npos, "domain を realm/nonce と取り違えない");

	const std::string ha1 = md5::hex(std::string(argv[1]) + ":CameraControlApi:" + argv[2]);
	const std::string ha2 = md5::hex("GET:" + uri);
	const std::string want = md5::hex(ha1 + ":16805f0b2a8151fbadc71b8fdf3890d888701603:" + f(h,"nc") + ":" +
	                                  f(h,"cnonce") + ":auth:" + ha2);
	chk(f(h,"response") == want, "response(独立計算と一致)");
	std::printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	return fails;
}
