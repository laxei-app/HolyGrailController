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
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <android/log.h>

#include "etp.h"
#include "holyGrailEntity.h"
#include "commonAndroid.h"

// 診断ログ(スマホ→エッジ開始の各段の可視化)。adb logcat -s HGEdgeCli で確認。
#define ELOG(...) __android_log_print(ANDROID_LOG_INFO, "HGEdgeCli", __VA_ARGS__)

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
		// MSG_NOSIGNAL: 相手が閉じた接続(使い回しで stale の場合)への send で SIGPIPE により
		// プロセスが落ちるのを防ぐ。失敗は戻り値<0 で検知して呼び側が張り直す。
		if (send(fd, out.data(), out.size(), MSG_NOSIGNAL) < 0) { return 0; }

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

	// --- エッジ制御用の永続TCP接続(案B) ---
	// 毎回 open/close せず 1 本を張りっぱなしにして使い回す。connect/close の churn を無くし、
	// エッジ側の単一クライアントTCPサーバとも自然に噛み合わせてスタックを防ぐ。
	// start/stop/progress が別スレッドから呼ばれるため g_connMtx で操作全体を直列化する。
	std::mutex   g_connMtx;
	int          g_connFd   = -1;
	std::string  g_connHost;
	int          g_connPort = 0;

	void closeConn(void)	// 要 g_connMtx
	{
		if (g_connFd >= 0) { close(g_connFd); g_connFd = -1; }
	}

	// 接続を用意する(必要なら張り直す)。宛先が変わったら再接続。return: fd or -1。
	// fresh には「今この呼び出しで新規に張ったか(=使い回しでない)」を返す。要 g_connMtx。
	int ensureConn(const std::string& host, int port, bool* fresh)
	{
		if (fresh) { *fresh = false; }
		if (g_connFd >= 0 && (host != g_connHost || port != g_connPort)) { closeConn(); }
		if (g_connFd < 0)
		{
			g_connFd   = tcpConnect(host, port, 5000);
			g_connHost = host; g_connPort = port;
			if (fresh) { *fresh = true; }
			ELOG("edgeConn %s ip=%s:%d fd=%d", (g_connFd >= 0 ? "open" : "open FAIL"), host.c_str(), port, g_connFd);
		}
		return g_connFd;
	}

	// 使い回し接続に残った未読バイト(想定外の残骸)を捨てる。要 g_connMtx。
	void drainSocket(int fd)
	{
		uint8_t b[512];
		while (recv(fd, b, sizeof(b), MSG_DONTWAIT) > 0) { /* discard */ }
	}

	// 永続接続で「操作の最初の1コマンド」を送る。使い回し接続が死んでいたら 1 度だけ張り直して再送。
	// return: method(ACK/NAK) or 0(失敗)。要 g_connMtx。
	int firstReq(const std::string& host, int port, uint16_t cmd, uint16_t method,
	             const std::string& data, std::string& out)
	{
		bool fresh = false;
		int fd = ensureConn(host, port, &fresh);
		if (fd >= 0)
		{
			if (!fresh) { drainSocket(fd); }			// 使い回しは残骸を掃除してから
			int m = tcpRequest(fd, cmd, method, data, out);
			if (m) { return m; }
			if (fresh) { closeConn(); return 0; }		// 新規接続でも失敗ならエッジ側の問題
			closeConn();								// 使い回しが stale → 張り直して 1 回だけ再送
			fd = ensureConn(host, port, &fresh);
		}
		if (fd < 0) { return 0; }
		int m = tcpRequest(fd, cmd, method, data, out);
		if (!m) { closeConn(); }
		return m;
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
	uint8_t buf[2048];	// edgeInfo は sessions(実行中セッション一覧・最大8件)を含むため余裕を持つ
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
                                                   jstring datetime_, jint offMin, jbyteArray nameBmp, jstring planId_,
                                                   jstring planJson_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* dt   = env->GetStringUTFChars(datetime_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	// 項目5: 送る計画JSONは呼び出し側が planId から取得して渡す(選択中の計画=グローバル状態に依存しない)。
	//  これにより「選択→JSON取得」を呼び出し側の短時間ロックで済ませ、遅いネットワーク送信を planExec から外せる。
	const char* pj   = planJson_ ? env->GetStringUTFChars(planJson_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string dtS   = dt ? dt : "";
	std::string pidS  = pid ? pid : "";
	std::string pjS   = pj ? pj : "";
	env->ReleaseStringUTFChars(host_, host);
	env->ReleaseStringUTFChars(datetime_, dt);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }
	if (pj)  { env->ReleaseStringUTFChars(planJson_, pj); }

	std::lock_guard<std::mutex> lk(g_connMtx);	// 永続接続を操作全体で占有(コマンド間に他操作を割り込ませない)

	jint result = 0;
	std::string rd;
	// 1) 時刻同期(操作の先頭。使い回し接続が死んでいれば firstReq が張り直して再送)
	std::string timeJson = "{\"datetime\":\"" + dtS + "\",\"utcOffsetMin\":" + std::to_string(offMin) + "}";
	int mTime = firstReq(hostS, port, etp::C_TIME, etp::M_PUT, timeJson, rd);
	ELOG("edgeStart C_TIME method=%d fd=%d", mTime, g_connFd);
	if (mTime != etp::M_ACK) { closeConn(); return -2; }
	int fd = g_connFd;	// 以降は確立済みの同一接続で送る
	// C_CAPTURE_PLAN(約5.5KB)のインポートはエッジ側でSD書込み+スケジュール構築に~5秒かかり、
	// さらに他セッションのカメラ確立と重なると遅れる。既定の受信5秒では2台目の順次開始が
	// ちょうどタイムアウトして開始不発になるため、この操作の間だけ受信タイムアウトを延ばす。
	setRcvTimeout(fd, 15000);

	// 2) 撮影計画。呼び出し側が渡した planJson を使う(選択中の計画に依存しない=項目5)。
	//    互換: 空で渡された場合のみ従来どおり選択中の計画を取得する。
	if (result == 0)
	{
		std::string planStr = pjS;
		if (planStr.empty())
		{
			int32_t len = 0;
			hge_getPlanJson(nullptr, &len);
			std::vector<char> pbuf(len > 0 ? static_cast<size_t>(len) : 1);
			if (hge_getPlanJson(pbuf.data(), &len) == 0) { planStr = pbuf.data(); }
		}
		ELOG("edgeStart planJson len=%d (fromArg=%d)", (int)planStr.size(), (int)!pjS.empty());
		if (!planStr.empty())
		{
			// data = "id\t{plan json}"。エッジは id ごとに計画を蓄積する。
			std::string body = pidS + "\t" + planStr;
			int mPlan = tcpRequest(fd, etp::C_CAPTURE_PLAN, etp::M_PUT, body, rd);
			ELOG("edgeStart C_CAPTURE_PLAN bodyLen=%d method=%d", (int)body.size(), mPlan);
			if (mPlan != etp::M_ACK)
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
			// data = "id\t<bitmap bytes>\x01"。エッジは計画 id ごとに名前ビットマップを保持する。
			std::string bmpData = pidS + "\t" + std::string(reinterpret_cast<const char*>(nb), static_cast<size_t>(nlen));
			env->ReleaseByteArrayElements(nameBmp, nb, JNI_ABORT);
			// 番兵: ETP decode の末尾空白/NUL除去からビットマップ末尾の黒画素(0x00)を守る(エッジ側で1バイト除去)。
			bmpData.push_back('\x01');
			tcpRequest(fd, etp::C_NAME_BMP, etp::M_PUT, bmpData, rd);
		}
	}

	// 3) 撮影開始(計画 id を渡してその計画を開始させる)
	if (result == 0)
	{
		int mAct = tcpRequest(fd, etp::C_ACTION, etp::M_POST, pidS, rd);
		ELOG("edgeStart C_ACTION planId=%s method=%d", pidS.c_str(), mAct);
		if (mAct != etp::M_ACK) { result = -4; }
	}

	if (result != 0) { closeConn(); }	// 失敗時は接続を破棄し、次回はクリーンに張り直す(残骸混入を防ぐ)
	else { setRcvTimeout(fd, 5000); }	// 受信タイムアウトを既定(5秒)へ戻す(この接続は使い回される)
	ELOG("edgeStart result=%d", (int)result);
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

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_STOP, etp::M_POST, pidS, rd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// 項目6: エッジ端末から計画を削除する(C_DELETE_PLAN)。スマホで停止した計画をエッジからも消す用。
//  エッジは撮影中なら停止してから削除する(項目9)。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeDeletePlan(JNIEnv* env, jobject, jstring host_, jint port, jstring planId_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string pidS  = pid ? pid : "";
	env->ReleaseStringUTFChars(host_, host);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_DELETE_PLAN, etp::M_POST, pidS, rd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// エッジ端末へ時刻同期(C_TIME)だけを能動的に送る。撮影開始と無関係に定期同期する用
// (RTC無し機=StickS3が電波悪い所でもスマホが近くにあれば時計を保てるように)。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeSyncTime(JNIEnv* env, jobject, jstring host_, jint port, jstring datetime_, jint offMin)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* dt   = datetime_ ? env->GetStringUTFChars(datetime_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string dtS   = dt ? dt : "";
	env->ReleaseStringUTFChars(host_, host);
	if (dt) { env->ReleaseStringUTFChars(datetime_, dt); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	std::string timeJson = "{\"datetime\":\"" + dtS + "\",\"utcOffsetMin\":" + std::to_string(offMin) + "}";
	int m = firstReq(hostS, port, etp::C_TIME, etp::M_PUT, timeJson, rd);
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

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_RESEARCH, etp::M_POST, pidS, rd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// エッジ端末へ発見中のオンラインカメラ情報(cameraInfo)を送る。json=[{serial,model,ip,online}]。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeCameraInfo(JNIEnv* env, jobject, jstring host_, jint port, jstring json_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* js   = json_ ? env->GetStringUTFChars(json_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string jsonS = js ? js : "";
	env->ReleaseStringUTFChars(host_, host);
	if (js) { env->ReleaseStringUTFChars(json_, js); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_CAMERA_INFO, etp::M_PUT, jsonS, rd);
	return (m == etp::M_ACK) ? 0 : -2;
}

// エッジ端末の進捗を取得する。progress の JSON を返す(失敗時 "")。
// planId 指定時は「その計画」の状態/進捗を返す(1エッジ複数カメラで誤検出を防ぐ)。空文字は集約(復元/生存確認)。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeProgress(JNIEnv* env, jobject, jstring host_, jint port, jstring planId_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* pid  = planId_ ? env->GetStringUTFChars(planId_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string pidS  = pid ? pid : "";
	env->ReleaseStringUTFChars(host_, host);
	if (pid) { env->ReleaseStringUTFChars(planId_, pid); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_PROGRESS, etp::M_GET, pidS, rd);
	return env->NewStringUTF((m == etp::M_ACK) ? rd.c_str() : "");
}

// エッジのログファイル名一覧(JSON配列 ["hg_....log",...])を取得する。失敗時 "[]"。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeLogList(JNIEnv* env, jobject, jstring host_, jint port)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	std::string hostS = host ? host : "";
	env->ReleaseStringUTFChars(host_, host);

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_LOG_LIST, etp::M_GET, "", rd);
	return env->NewStringUTF((m == etp::M_ACK) ? rd.c_str() : "[]");
}

// エッジのログファイル name の offset バイト目から1チャンク(最大4KB)を取得する。返り値=生バイト。
// 空配列=EOF(またはエラー)。応答末尾の番兵(0x01)は除去して返す。
JNIEXPORT jbyteArray JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeLogRead(JNIEnv* env, jobject, jstring host_, jint port, jstring name_, jint offset)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* name = name_ ? env->GetStringUTFChars(name_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string nameS = name ? name : "";
	env->ReleaseStringUTFChars(host_, host);
	if (name) { env->ReleaseStringUTFChars(name_, name); }

	std::string rd;
	{
		std::lock_guard<std::mutex> lk(g_connMtx);
		std::string data = nameS + "\t" + std::to_string((long)offset);
		int m = firstReq(hostS, port, etp::C_LOG_READ, etp::M_GET, data, rd);
		if (m != etp::M_ACK) { rd.clear(); }
	}
	if (!rd.empty() && rd.back() == '\x01') { rd.pop_back(); }	// 番兵を除去
	jbyteArray arr = env->NewByteArray(static_cast<jsize>(rd.size()));
	if (arr && !rd.empty()) { env->SetByteArrayRegion(arr, 0, static_cast<jsize>(rd.size()), reinterpret_cast<const jbyte*>(rd.data())); }
	return arr;
}

// --- 撮影レポートの回収(2026-08-05) ---
// エッジで撮った撮影のレポートはエッジ側に溜まる。スマホの常時スイープ(30秒)が検索応答の
// "reports" を見て、1件以上あるときだけこの3本を使って引き取る。
// 引き取り→保存できたことを確認→削除、の順で進めるので、途中で切れても失われない。

// レポート一覧(JSON配列)。失敗時 "[]"。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeReportList(JNIEnv* env, jobject, jstring host_, jint port)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	std::string hostS = host ? host : "";
	env->ReleaseStringUTFChars(host_, host);

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_REPORT_LIST, etp::M_GET, "", rd);
	return env->NewStringUTF((m == etp::M_ACK) ? rd.c_str() : "[]");
}

// レポート1件の中身(JSON)。失敗時 ""(空)。空なら保存も削除もしない。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeReportRead(JNIEnv* env, jobject, jstring host_, jint port, jstring name_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* name = name_ ? env->GetStringUTFChars(name_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string nameS = name ? name : "";
	env->ReleaseStringUTFChars(host_, host);
	if (name) { env->ReleaseStringUTFChars(name_, name); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_REPORT_READ, etp::M_GET, nameS, rd);
	return env->NewStringUTF((m == etp::M_ACK) ? rd.c_str() : "");
}

// 受領済みレポートの削除を指示する。0=削除された / -2=断られた(次のスイープでまた拾う)。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeEdgeReportDelete(JNIEnv* env, jobject, jstring host_, jint port, jstring name_)
{
	const char* host = env->GetStringUTFChars(host_, nullptr);
	const char* name = name_ ? env->GetStringUTFChars(name_, nullptr) : nullptr;
	std::string hostS = host ? host : "";
	std::string nameS = name ? name : "";
	env->ReleaseStringUTFChars(host_, host);
	if (name) { env->ReleaseStringUTFChars(name_, name); }

	std::lock_guard<std::mutex> lk(g_connMtx);
	std::string rd;
	int m = firstReq(hostS, port, etp::C_REPORT_DELETE, etp::M_DELETE, nameS, rd);
	return (m == etp::M_ACK) ? 0 : -2;
}

} // extern "C"
