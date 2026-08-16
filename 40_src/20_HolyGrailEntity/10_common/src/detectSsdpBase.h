#ifndef _DETECT_SSDP_BASE_H_
#define _DETECT_SSDP_BASE_H_

#include "detectBase.h"
#include "deviceDiscovery.h"
#include <atomic>

// SSDP(UPnP)で検出する受信バックエンドの中間基底。
// 能動 M-SEARCH(deviceDiscovery::search)→重複統合→apiBase 生成/初期化までを共通化する。
// 派生は「対応するサービス定義(interfaces)」と「apiBase 生成(makeApi)」だけを与える。
// SSDP 受動待ち受け(watchStart/Stop)も本基底に実装する(#5 の答え: 受信層に置く)。専用ソケット
// (net::ssdpListen*)+専用スレッドで NOTIFY を待ち、対象サービスの出現で onAppear を呼ぶ。
class detectSsdpBase : public detectBase
{
public:
	~detectSsdpBase() override;
	// M-SEARCH で探索し、統合・apiBase 初期化まで済ませたデバイスを out に追加する。
	size_t detect(std::vector<class device>& out) override;
	// 同じ探索だが、身元確認(デバイス記述)までで止める。CCAPI は叩かない。
	size_t identify(std::vector<class device>& out) override;
	// SSDP受動待ち受けの開始/停止。既に稼働中なら watchStart は何もしない(共有単一リスナ)。
	void watchStart(std::function<void()> onAppear) override;
	void watchStop() override;

protected:
	// このバックエンドが対応するサービス定義(キーワード→apiClass)。
	virtual const std::vector<deviceDiscovery::definitionIntereface>& interfaces() const = 0;

private:
	// 探索の共通部。identifyOnly=true なら apiBase を作らず、身元(記述子)だけで out へ入れる。
	size_t discover(std::vector<class device>& out, bool identifyOnly);
	errCode watchLoop();
	std::atomic<bool>     watchRunning_{ false };
	void*                 watchThread_ = nullptr;	// ossc スレッドハンドル
	void*                 watchSock_   = nullptr;	// net::ssdpListen* ハンドル
	std::function<void()> onAppear_;
};

#endif // _DETECT_SSDP_BASE_H_
