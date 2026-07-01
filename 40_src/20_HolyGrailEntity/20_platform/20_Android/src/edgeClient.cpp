// Android(スマホ)側の ETP クライアント(データ構造仕様書43 §6)。
// エッジ端末(M5Stack)を UDP で検索し、TCP で時刻同期・計画転送・開始/停止・進捗取得する。
// パケット組立は共通の etp、計画JSONは entity(hge_getPlanJson)から取得して再利用する。

#include <jni.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "etp.h"
#include "holyGrailEntity.h"
#include "commonAndroid.h"

namespace
{
	constexpr int PORT_DISCOVERY = 50505;

	void setRcvTimeout(int fd, int ms)
	{
		timeval tv{};
		tv.tv_sec  = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}

	// 全インターフェースのサブネットブロードキャストアドレスを集める。
	std::vector<uint32_t> broadcastAddrs(void)
	{
		std::vector<uint32_t> outs;
		outs.push_back(0xFFFFFFFFu);	// 255.255.255.255(限定ブロードキャスト)
		ifaddrs* ifaddr = nullptr;
		if (getifaddrs(&ifaddr) == 0)
		{
			for (ifaddrs* ifa = ifaddr; ifa; ifa = ifa->ifa_next)
			{
				if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) { continue; }
				if (ifa->ifa_flags & IFF_LOOPBACK) { continue; }
				if (!ifa->ifa_netmask) { continue; }
				uint32_t ip   = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr;
				uint32_t mask = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask)->sin_addr.s_addr;
				outs.push_back((ip & mask) | ~mask);	// 有向ブロードキャスト
			}
			freeifaddrs(ifaddr);
		}
		return outs;
	}

	// data 文字列フィールドから簡易に値を取り出す(ip の重複排除用。"\"ip\":\"x\"")。
	std::string jsonStr(const std::string& j, const std::string& key)
	{
		std::string pat = "\"" + key + "\":\"";
		size_t p = j.find(pat);
		if (p == std::string::npos) { return ""; }
		p += pat.size();
		size_t e = j.find('"', p);
		if (e == std::string::npos) { return ""; }
		return j.substr(p, e - p);
	}

	// TCP で 1 フレーム送って ack/nak を読む。return: method(ACK/NAK) or 0。応答 data は outData。
	int tcpRequest(int fd, uint16_t cmd, uint16_t method, const std::string& data, std::string& outData)
	{
		std::vector<uint8_t> out = etp::encode(cmd, method, data);
		if (send(fd, out.data(), out.size(), 0) < 0) { return 0; }

		std::vector<uint8_t> rx;
		uint8_t buf[1024];
		for (int i = 0; i < 64; ++i)	// 最大数回読む
		{
			etp::packet pk;
			int c = etp::decode(rx.data(), rx.size(), pk);
			if (c > 0) { outData = pk.data; return pk.method; }
			ssize_t n = recv(fd, buf, sizeof(buf), 0);
			if (n <= 0) { break; }
			rx.insert(rx.end(), buf, buf + n);
		}
		return 0;
	}

	int tcpConnect(const std::string& host, int port, int timeoutMs)
	{
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) { return -1; }
		setRcvTimeout(fd, timeoutMs);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<uint16_t>(port));
		inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
		if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
		{
			close(fd);
			return -1;
		}
		return fd;
	}
}

extern "C" {

// エッジ端末を検索する。timeoutMs だけ応答を集め、edgeInfo の JSON 配列を返す。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeSearch(JNIEnv* env, jobject, jint timeoutMs)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) { return env->NewStringUTF("[]"); }
	int yes = 1;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	setRcvTimeout(fd, timeoutMs);

	std::vector<uint8_t> q = etp::encode(etp::C_SEARCH, etp::M_GET, "");
	for (uint32_t b : broadcastAddrs())
	{
		sockaddr_in dst{};
		dst.sin_family = AF_INET;
		dst.sin_port = htons(PORT_DISCOVERY);
		dst.sin_addr.s_addr = b;
		sendto(fd, q.data(), q.size(), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
	}

	std::string arr = "[";
	std::set<std::string> seen;	// ip で重複排除
	uint8_t buf[1024];
	for (int i = 0; i < 32; ++i)
	{
		ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
		if (n <= 0) { break; }
		etp::packet pk;
		int c = etp::decode(buf, static_cast<size_t>(n), pk);
		if (c <= 0 || pk.cmd != etp::C_SEARCH || pk.method != etp::M_ACK) { continue; }
		std::string ip = jsonStr(pk.data, "ip");
		if (ip.empty() || seen.count(ip)) { continue; }
		seen.insert(ip);
		if (arr.size() > 1) { arr += ","; }
		arr += pk.data;
	}
	arr += "]";
	close(fd);
	return env->NewStringUTF(arr.c_str());
}

// エッジ端末へ time→capturePlan→action を送って撮影開始させる。return: 0=成功。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeStart(JNIEnv* env, jobject, jstring host_, jint port,
                                                   jstring datetime_, jint offMin, jbyteArray nameBmp, jstring planId_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* dt   = env->GetStringUTFChars(datetime_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string dtS   = dt ? dt : "";
	std::string pidS  = pid ? pid : "";
	env->ReleaseStringUTFChars(host_, host);
	env->ReleaseStringUTFChars(datetime_, dt);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }

	int fd = tcpConnect(hostS, port, 5000);
	if (fd < 0) { return -1; }

	jint result = 0;
	std::string rd;
	// 1) 時刻同期
	std::string timeJson = "{\"datetime\":\"" + dtS + "\",\"utcOffsetMin\":" + std::to_string(offMin) + "}";
	if (tcpRequest(fd, etp::C_TIME, etp::M_PUT, timeJson, rd) != etp::M_ACK) { result = -2; }

	// 2) 撮影計画(現在の計画JSONを entity から取得)
	if (result == 0)
	{
		int32_t len = 0;
		hge_getPlanJson(nullptr, &len);
		std::vector<char> pbuf(len > 0 ? static_cast<size_t>(len) : 1);
		if (hge_getPlanJson(pbuf.data(), &len) == 0)
		{
			// data = "id\t{plan json}"。エッジは id ごとに計画を蓄積する。
			std::string body = pidS + "\t" + std::string(pbuf.data());
			if (tcpRequest(fd, etp::C_CAPTURE_PLAN, etp::M_PUT, body, rd) != etp::M_ACK)
			{ result = -3; }
		}
		else { result = -3; }
	}

	// 2.5) 計画名ビットマップ(あれば送る。失敗は致命的でない)
	if (result == 0 && nameBmp != nullptr)
	{
		jsize nlen = env->GetArrayLength(nameBmp);
		if (nlen > 0)
		{
			jbyte* nb = env->GetByteArrayElements(nameBmp, nullptr);
			// data = "id\t<bitmap bytes>"。エッジは計画 id ごとに名前ビットマップを保持する。
			std::string bmpData = pidS + "\t" + std::string(reinterpret_cast<const char*>(nb), static_cast<size_t>(nlen));
			env->ReleaseByteArrayElements(nameBmp, nb, JNI_ABORT);
			tcpRequest(fd, etp::C_NAME_BMP, etp::M_PUT, bmpData, rd);
		}
	}

	// 3) 撮影開始(計画 id を渡してその計画を開始させる)
	if (result == 0)
	{
		if (tcpRequest(fd, etp::C_ACTION, etp::M_POST, pidS, rd) != etp::M_ACK) { result = -4; }
	}

	close(fd);
	return result;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeStop(JNIEnv* env, jobject, jstring host_, jint port, jstring planId_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string pidS  = pid ? pid : "";
	env->ReleaseStringUTFChars(host_, host);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }

	int fd = tcpConnect(hostS, port, 5000);
	if (fd < 0) { return -1; }
	std::string rd;
	int m = tcpRequest(fd, etp::C_STOP, etp::M_POST, pidS, rd);
	close(fd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// エッジ端末へ「継続(カメラ未検出時の即再探索)」を送る。planId 空=全取得フェーズ。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeResearch(JNIEnv* env, jobject, jstring host_, jint port, jstring planId_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string pidS  = pid ? pid : "";
	env->ReleaseStringUTFChars(host_, host);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }

	int fd = tcpConnect(hostS, port, 5000);
	if (fd < 0) { return -1; }
	std::string rd;
	int m = tcpRequest(fd, etp::C_RESEARCH, etp::M_POST, pidS, rd);
	close(fd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// エッジ端末の進捗を取得する。progress の JSON を返す(失敗時 "")。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeProgress(JNIEnv* env, jobject, jstring host_, jint port)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	std::string hostS = host ? host : "";
	env->ReleaseStringUTFChars(host_, host);

	int fd = tcpConnect(hostS, port, 5000);
	if (fd < 0) { return env->NewStringUTF(""); }
	std::string rd;
	int m = tcpRequest(fd, etp::C_PROGRESS, etp::M_GET, "", rd);
	close(fd);
	return env->NewStringUTF((m == etp::M_ACK) ? rd.c_str() : "");
}

} // extern "C"
