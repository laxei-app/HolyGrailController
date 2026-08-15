// 実カメラ相手に httpAuth の 401 対応を通す(資格情報は argv で受ける。ファイルに残さない)。
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "httpAuth.h"
#include <cstdio>
#include <string>
#pragma comment(lib, "ws2_32.lib")

static std::string request(const std::string& host, int port, const std::string& method,
                           const std::string& path, const std::string& auth)
{
	addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	addrinfo* res = nullptr;
	if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) { return ""; }
	SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) { closesocket(s); freeaddrinfo(res); return ""; }
	freeaddrinfo(res);
	std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
	if (!auth.empty()) { req += "Authorization: " + auth + "\r\n"; }
	req += "Connection: close\r\n\r\n";
	send(s, req.c_str(), (int)req.size(), 0);
	std::string out; char buf[4096]; int n;
	while ((n = recv(s, buf, sizeof(buf), 0)) > 0) { out.append(buf, n); }
	closesocket(s);
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
static std::string statusOf(const std::string& r) { return r.substr(0, r.find("\r\n")); }
static std::string bodyOf(const std::string& r)
{
	size_t p = r.find("\r\n\r\n");
	return (p == std::string::npos) ? "" : r.substr(p + 4);
}

int main(int argc, char** argv)
{
	if (argc < 5) { std::printf("usage: tlive <ip> <port> <user> <pass>\n"); return 2; }
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
	const std::string ip = argv[1];
	const int port = atoi(argv[2]);
	const std::string hostKey = ip + ":" + std::to_string(port);

	// 実運用と同じ: 所持カメラの資格情報を候補として登録するだけ。事前に「認証が要る」とは知らない。
	httpAuth::setCandidates({ {argv[3], argv[4]} });	// 誤った候補は入れない(認証失敗でカメラが締め出されるため)

	const char* paths[] = { "/ccapi/ver100/deviceinformation", "/ccapi/ver100/devicestatus/battery",
	                        "/ccapi/ver100/shooting/settings" };
	for (const char* path : paths)
	{
		std::string auth = httpAuth::authorization(hostKey, "GET", path);
		const bool preset = !auth.empty();
		std::string r = request(ip, port, "GET", path, auth);
		std::string st = statusOf(r);
		if (st.find("401") != std::string::npos)
		{
			const std::string wa = headerOf(r, "WWW-Authenticate");
			std::printf("%-42s %s  (auth=%s)\n", path, st.c_str(), preset ? "付けた" : "無し");
			if (!httpAuth::learn(hostKey, wa)) { std::printf("   learn 失敗(候補が無い/尽きた)\n"); continue; }
			auth = httpAuth::authorization(hostKey, "GET", path);
			std::printf("   -> Authorization: %.150s...\n", auth.c_str());
			r = request(ip, port, "GET", path, auth);
			st = statusOf(r);
		}
		std::string b = bodyOf(r);
		if (b.size() > 150) { b = b.substr(0, 150) + "..."; }
		std::printf("%-42s %s  %s%s\n", path, st.c_str(), preset ? "[最初から認証付き] " : "", b.c_str());
	}
	WSACleanup();
	return 0;
}
