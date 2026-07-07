// Android(NDK) 用 net 実装。POSIX ソケットで CCAPI の平文HTTPとSSDPを行う。
// HTTPS は使用しない(EOS-R10 CCAPI はローカルLANの平文HTTP)。

#include "common.h"
#include "net.h"
#include "commonAndroid.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace net
{
	namespace
	{
		std::atomic<bool> g_break{ false };

		// 受信タイムアウトを設定する
		void setTimeout(int fd, int sec)
		{
			timeval tv{};
			tv.tv_sec = sec;
			tv.tv_usec = 0;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		}

		// URL を host/port/path に分解する。http:// 前提。
		bool parseUrl(const std::string& url, std::string& host, int& port, std::string& path)
		{
			std::string u = url;
			const std::string pre = "http://";
			size_t p = u.find(pre);
			if (p == 0) { u = u.substr(pre.size()); }
			port = 80;
			size_t slash = u.find('/');
			std::string hostport = (slash == std::string::npos) ? u : u.substr(0, slash);
			path = (slash == std::string::npos) ? "/" : u.substr(slash);
			size_t colon = hostport.find(':');
			if (colon == std::string::npos)
			{
				host = hostport;
			}
			else
			{
				host = hostport.substr(0, colon);
				port = std::atoi(hostport.substr(colon + 1).c_str());
			}
			return !host.empty();
		}

		// host:port へ TCP 接続する。失敗で -1。
		int tcpConnect(const std::string& host, int port)
		{
			addrinfo hints{};
			hints.ai_family = AF_INET;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			std::string portStr = std::to_string(port);
			if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr)
			{
				return -1;
			}
			int fd = -1;
			for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next)
			{
				fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
				if (fd < 0) { continue; }
				setTimeout(fd, 3);
				if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) { break; }
				close(fd);
				fd = -1;
			}
			freeaddrinfo(res);
			return fd;
		}

		bool sendAll(int fd, const char* data, size_t len)
		{
			size_t sent = 0;
			while (sent < len)
			{
				ssize_t n = send(fd, data + sent, len - sent, 0);
				if (n <= 0) { return false; }
				sent += static_cast<size_t>(n);
			}
			return true;
		}

		// レスポンスを受信する。Content-Length / chunked を解析し本文完了時点で読み終える。
		// カメラが Connection: close を無視して keep-alive を保つ場合でも、
		// 受信タイムアウト(数秒)待ちにならず素早く返す。
		bool recvResponse(int fd, std::string& raw)
		{
			raw.clear();
			char buf[4096];
			size_t    headerEnd = std::string::npos;
			long long contentLen = -1;	// -1: 不明
			bool      chunked = false;
			for (;;)
			{
				if (g_break.load()) { g_break.store(false); return false; }

				// ヘッダ受信後、完了したか判定する
				if (headerEnd == std::string::npos)
				{
					headerEnd = raw.find("\r\n\r\n");
					if (headerEnd != std::string::npos)
					{
						std::string h = raw.substr(0, headerEnd);
						for (auto& c : h) { c = static_cast<char>(::tolower(c)); }
						if (h.find("transfer-encoding: chunked") != std::string::npos) { chunked = true; }
						size_t cl = h.find("content-length:");
						if (cl != std::string::npos) { contentLen = std::atoll(h.c_str() + cl + 15); }
					}
				}
				if (headerEnd != std::string::npos)
				{
					size_t bodyStart = headerEnd + 4;
					if (chunked)
					{	// 終端チャンク "0\r\n\r\n" を受信したら完了
						if (raw.find("\r\n0\r\n\r\n", bodyStart) != std::string::npos ||
						    raw.compare(bodyStart, 5, "0\r\n\r\n") == 0) { break; }
					}
					else if (contentLen >= 0)
					{	// Content-Length 分受信したら完了
						if (static_cast<long long>(raw.size() - bodyStart) >= contentLen) { break; }
					}
					// 長さ不明(Content-Lengthもchunkedも無い)時は EOF まで読む
				}

				ssize_t n = recv(fd, buf, sizeof(buf), 0);
				if (n > 0) { raw.append(buf, static_cast<size_t>(n)); }
				else       { break; }	// EOF / エラー / タイムアウト
			}
			return true;
		}

		// chunked 転送をデコードする。
		std::string dechunk(const std::string& body)
		{
			std::string out;
			size_t pos = 0;
			while (pos < body.size())
			{
				size_t eol = body.find("\r\n", pos);
				if (eol == std::string::npos) { break; }
				size_t sz = static_cast<size_t>(std::strtoul(body.substr(pos, eol - pos).c_str(), nullptr, 16));
				pos = eol + 2;
				if (sz == 0) { break; }
				if (pos + sz > body.size()) { sz = body.size() - pos; }
				out.append(body, pos, sz);
				pos += sz + 2;	// データ + 末尾CRLF
			}
			return out;
		}

		// HTTP リクエストを送って応答本体とステータスコードを得る。
		//  return : ステータスコード(失敗は 0)
		int httpRequest(const std::string& method, const std::string& url,
		                const std::string& body, bool hasBody, std::string& response)
		{
			response.clear();
			std::string host, path;
			int port = 80;
			if (!parseUrl(url, host, port, path)) { return 0; }

			int fd = tcpConnect(host, port);
			if (fd < 0)
			{
				__android_log_print(ANDROID_LOG_WARN, HGE_LOG_TAG,
				                    "net: connect failed host=%s port=%d errno=%d(%s)",
				                    host.c_str(), port, errno, std::strerror(errno));
				return 0;
			}

			std::string req = method + " " + path + " HTTP/1.1\r\n";
			req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
			req += "Connection: close\r\n";
			if (hasBody)
			{
				req += "Content-Type: application/json\r\n";
				req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
			}
			req += "\r\n";
			if (hasBody) { req += body; }

			if (!sendAll(fd, req.c_str(), req.size())) { close(fd); return 0; }

			std::string raw;
			recvResponse(fd, raw);
			close(fd);

			// ヘッダと本体を分離
			size_t he = raw.find("\r\n\r\n");
			std::string header = (he == std::string::npos) ? raw : raw.substr(0, he);
			std::string content = (he == std::string::npos) ? std::string() : raw.substr(he + 4);

			// ステータスコード
			int code = 0;
			size_t sp = header.find(' ');
			if (sp != std::string::npos) { code = std::atoi(header.substr(sp + 1, 3).c_str()); }

			// chunked 判定(ヘッダを小文字化して検査)
			std::string lower = header;
			for (auto& c : lower) { c = static_cast<char>(::tolower(c)); }
			if (lower.find("transfer-encoding: chunked") != std::string::npos)
			{
				content = dechunk(content);
			}
			response = content;
			return code;
		}
	} // anonymous namespace

	bool init()
	{
		g_break = false;
		return true;
	}

	bool deInit()
	{
		return true;
	}

	// 利用可能な全NICのIPv4アドレスを取得(ループバック除く)
	std::vector<std::string> getLocalIpList()
	{
		std::vector<std::string> ips;
		ifaddrs* ifaddr = nullptr;
		if (getifaddrs(&ifaddr) != 0) { return ips; }
		for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
		{
			if (ifa->ifa_addr == nullptr) { continue; }
			if (ifa->ifa_addr->sa_family != AF_INET) { continue; }
			if ((ifa->ifa_flags & IFF_UP) == 0) { continue; }
			if (ifa->ifa_flags & IFF_LOOPBACK) { continue; }
			auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
			char buf[INET_ADDRSTRLEN];
			if (inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf)))
			{
				ips.push_back(std::string(buf));
			}
		}
		freeifaddrs(ifaddr);
		return ips;
	}

	// APモードのクライアントIP列挙はエッジ端末専用機能。Android は非対応(空を返す)。
	std::vector<std::string> apClientIps()
	{
		return std::vector<std::string>();
	}

	// 限定サブネットのバッチ探索はエッジ役(§3.3 tier3)専用。Android(スマホ役)は発見をSSDPに委ねるため非対応。
	std::vector<std::string> scanSubnetPort(int /*port*/, int /*timeoutMs*/, int /*maxHosts*/)
	{
		return std::vector<std::string>();
	}

	// SSDP 探索開始(指定NICからM-SEARCHを送信)
	void* ssdpStart(const std::string& query, const std::string& localIp)
	{
		int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock < 0) { return nullptr; }
		setTimeout(sock, 1);

		int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		sockaddr_in localAddr{};
		localAddr.sin_family = AF_INET;
		localAddr.sin_port = htons(0);
		if (!localIp.empty()) { inet_pton(AF_INET, localIp.c_str(), &localAddr.sin_addr); }
		else                  { localAddr.sin_addr.s_addr = htonl(INADDR_ANY); }
		if (bind(sock, reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) < 0)
		{
			close(sock);
			return nullptr;
		}

		// マルチキャストの送信インターフェースを明示する。ソケットの bind(送信元IP)では
		// マルチキャストの出口IFは決まらず、既定経路(携帯回線・別Wi-Fi等)へ出てしまうため、
		// localIp の NIC を IP_MULTICAST_IF に設定して必ずカメラと同じLANへ M-SEARCH を出す。
		if (!localIp.empty())
		{
			in_addr ifAddr{};
			inet_pton(AF_INET, localIp.c_str(), &ifAddr);
			setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &ifAddr, sizeof(ifAddr));
		}
		unsigned char ttl = 4;	// 同一サブネット想定だが念のため数ホップ許可
		setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

		sockaddr_in dest{};
		dest.sin_family = AF_INET;
		dest.sin_port = htons(1900);
		inet_pton(AF_INET, "239.255.255.250", &dest.sin_addr);
		sendto(sock, query.c_str(), query.length(), 0,
		       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

		return new int(sock);
	}

	bool ssdpRead(void* handle, std::string& answer)
	{
		if (handle == nullptr) { return false; }
		int sock = *static_cast<int*>(handle);
		answer.resize(1024 * 4);
		ssize_t r = recvfrom(sock, answer.data(), answer.size() - 1, 0, nullptr, nullptr);
		if (r > 0)
		{
			answer.resize(static_cast<size_t>(r));
			return true;
		}
		return false;
	}

	void ssdpClose(void* handle)
	{
		if (handle != nullptr)
		{
			int* sockPtr = static_cast<int*>(handle);
			close(*sockPtr);
			delete sockPtr;
		}
	}

	// SSDP受動待ち受け: 0.0.0.0:1900 に bind + 239.255.255.250 へ参加(IP_ADD_MEMBERSHIP)。
	// ※MulticastLock は Java 側(MainActivity)で保持する。無いとここで受信できても破棄される。
	void* ssdpListenStart(void)
	{
		int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (sock < 0) { return nullptr; }
		setTimeout(sock, 1);	// recv 1秒タイムアウト(専用スレッドから周期読み。停止に追随)

		int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
		setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));	// 能動探索の1900と共存
#endif
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port   = htons(1900);
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
		{
			close(sock);
			return nullptr;
		}
		ip_mreq mreq{};
		inet_pton(AF_INET, "239.255.255.250", &mreq.imr_multiaddr);
		mreq.imr_interface.s_addr = htonl(INADDR_ANY);
		setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

		return new int(sock);
	}

	bool ssdpListenRead(void* handle, std::string& answer) { return ssdpRead(handle, answer); }
	void ssdpListenClose(void* handle) { ssdpClose(handle); }

	void httpBreak(void)
	{
		g_break = true;
	}

	bool httpGet(const std::string& url, std::string& response)
	{
		httpRequest("GET", url, std::string(), false, response);
		return true;	// Windows版に倣い、取得有無に関わらず true
	}

	bool httpPost(const std::string& url, const std::string& body, std::string& response)
	{
		int code = httpRequest("POST", url, body, true, response);
		return (code >= 200 && code <= 204);
	}

	bool httpPut(const std::string& url, const std::string& body, std::string& response)
	{
		int code = httpRequest("PUT", url, body, true, response);
		return (code >= 200 && code <= 204);
	}

	bool httpDelete(const std::string& url, std::string& response)
	{
		int code = httpRequest("DELETE", url, std::string(), false, response);
		return (code == 200 || code == 202 || code == 204);
	}
}
