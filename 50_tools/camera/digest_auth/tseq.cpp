// 締め出しの引き金を1回のリセットで切り分ける。
//   第1相: 1スレッドで逐次アクセス(nc が必ず昇順で届く)
//   第2相: 2スレッドで並行アクセス(実機と同じ。nc の到着順が入れ替わる)
// 第1相を生き延びて第2相で落ちれば「並行アクセスが引き金」。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "httpAuth.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

static std::string g_ip; static int g_port; static std::string g_hostKey;
static std::atomic<bool> g_stop{false};
static std::atomic<int>  g_ok{0}, g_ng{0};
static std::mutex g_print;
static std::chrono::steady_clock::time_point g_t0;

static int elapsed() { return (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - g_t0).count(); }

static std::string request(const std::string& method, const std::string& path, const std::string& auth, std::string& hdr)
{
	addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (getaddrinfo(g_ip.c_str(), std::to_string(g_port).c_str(), &hints, &res) != 0) { return ""; }
	SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closesocket(s); freeaddrinfo(res); return ""; }
	freeaddrinfo(res);
	// 実機の接続にかかる時間を模す。EOS R50 V は 80〜5000ms とばらつく(実測)。nc を採ってから
	//  送るまでのこの隙間が、ワーカー2本のときに nc の到着順を入れ替える。**ばらつきが要点**で、
	//  一定の待ちだと順番が保たれてしまい再現しない。ローカルホストは速すぎるので自前で入れる。
	{
		static std::atomic<unsigned> seq{0};
		const unsigned n = ++seq;
		std::this_thread::sleep_for(std::chrono::milliseconds(3 + (n * 37) % 40));
	}
	std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + g_hostKey + "\r\n";
	if (!auth.empty()) { req += "Authorization: " + auth + "\r\n"; }
	req += "Connection: close\r\n\r\n";
	send(s, req.c_str(), (int)req.size(), 0);
	std::string out; char buf[4096]; int n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0) { out.append(buf, n); }
	closesocket(s);
	hdr = out.substr(0, out.find("\r\n\r\n"));
	return out;
}
static std::string headerOf(const std::string& r, const std::string& name)
{
	std::string lo = r, ln = "\r\n" + name + ":";
	for (auto& c : lo) { c = (char)tolower(c); }
	for (auto& c : ln) { c = (char)tolower(c); }
	size_t p = lo.find(ln);
	if (p == std::string::npos) { return ""; }
	p += ln.size();
	size_t e = r.find("\r\n", p);
	std::string v = r.substr(p, e - p);
	while (!v.empty() && v.front() == ' ') { v.erase(v.begin()); }
	return v;
}

// 1リクエスト(401 なら学習して投げ直す。実機と同じ振る舞い)。返り値=ステータス番号
static bool g_useGuard = true;		// net.cpp と同じ直列化を使うか(修正前後の比較用)

static int once(const char* path, const char* tag)
{
	// net.cpp の httpRequest と同じ形。錠を取ってから認証ヘッダを作り、送り終えるまで手放さない。
	std::unique_ptr<httpAuth::hostGuard> lk;
	if (g_useGuard) { lk.reset(new httpAuth::hostGuard(g_hostKey)); }

	std::string hdr;
	std::string auth = httpAuth::authorization(g_hostKey, "GET", path);
	std::string r = request("GET", path, auth, hdr);
	if (r.empty()) { return 0; }
	int code = atoi(r.c_str() + 9);
	if (code == 401)
	{
		const std::string wa = headerOf(r, "WWW-Authenticate");
		if (httpAuth::learn(g_hostKey, wa))
		{
			auth = httpAuth::authorization(g_hostKey, "GET", path);
			r = request("GET", path, auth, hdr);
			code = atoi(r.c_str() + 9);
		}
	}
	if (code != 401 && !auth.empty()) { httpAuth::noteSuccess(g_hostKey); }
	if (code == 200) { ++g_ok; }
	else
	{
		++g_ng;
		std::lock_guard<std::mutex> lk(g_print);
		std::printf("%4ds  [%s] %.*s  ok=%d ng=%d\n", elapsed(), tag,
		            (int)r.find("\r\n"), r.c_str(), (int)g_ok, (int)g_ng);
		std::fflush(stdout);
		g_stop = true;			// 最初の失敗で止める(引き金の時刻を見たい)
	}
	return code;
}

static void worker(const char* tag)
{
	const char* paths[] = { "/ccapi/ver100/shooting/settings", "/ccapi/ver100/devicestatus/battery" };
	int i = 0;
	while (!g_stop)
	{
		once(paths[i++ % 2], tag);
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
	}
}

int main(int argc, char** argv)
{
	if (argc < 5) { std::printf("usage: tseq <ip> <port> <user> <pass> [phase1sec]\n"); return 2; }
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
	g_ip = argv[1]; g_port = atoi(argv[2]); g_hostKey = g_ip + ":" + std::to_string(g_port);
	const int phase1 = (argc > 5) ? atoi(argv[5]) : 120;
	if (argc > 6 && std::string(argv[6]) == "noguard") { g_useGuard = false; }
	std::printf("(直列化: %s)\n", g_useGuard ? "あり" : "なし");
	httpAuth::setCandidates({ {argv[3], argv[4]} });
	g_t0 = std::chrono::steady_clock::now();

	std::printf("=== 第1相: 1スレッド逐次(%d秒) ===\n", phase1);
	std::fflush(stdout);
	{
		std::thread t(worker, "seq");
		while (!g_stop && elapsed() < phase1) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
		const bool died = g_stop;
		g_stop = true; t.join();
		if (died) { std::printf("=> 第1相で落ちた: 並行アクセスは無関係(時間か回数が引き金)\n"); WSACleanup(); return 1; }
		std::printf("%4ds  第1相 生存 ok=%d\n", elapsed(), (int)g_ok);
	}

	std::printf("=== 第2相: 2スレッド並行 ===\n");
	std::fflush(stdout);
	g_stop = false;
	const int okAt = g_ok;
	{
		std::thread a(worker, "par1"), b(worker, "par2");
		while (!g_stop && elapsed() < phase1 + 180) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
		g_stop = true; a.join(); b.join();
	}
	std::printf("=> 第2相の結果: 追加 ok=%d ng=%d\n", (int)g_ok - okAt, (int)g_ng);
	WSACleanup();
	return 0;
}
