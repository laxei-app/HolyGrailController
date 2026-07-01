#ifndef _DETECT_SSDP_BASE_H_
#define _DETECT_SSDP_BASE_H_

#include "detectBase.h"
#include "deviceDiscovery.h"

// SSDP(UPnP)で検出する受信バックエンドの中間基底。
// 能動 M-SEARCH(deviceDiscovery::search)→重複統合→apiBase 生成/初期化までを共通化する。
// 派生は「対応するサービス定義(interfaces)」と「apiBase 生成(makeApi)」だけを与える。
// SSDP 待ち受け(watchStart/Stop)は Phase3 で本基底に実装する(#5 の答え: 受信層に置く)。
class detectSsdpBase : public detectBase
{
public:
	// M-SEARCH で探索し、統合・apiBase 初期化まで済ませたデバイスを out に追加する。
	size_t detect(std::vector<class device>& out) override;

protected:
	// このバックエンドが対応するサービス定義(キーワード→apiClass)。
	virtual const std::vector<deviceDiscovery::definitionIntereface>& interfaces() const = 0;
};

#endif // _DETECT_SSDP_BASE_H_
