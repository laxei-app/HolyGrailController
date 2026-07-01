#ifndef _DETECT_BASE_H_
#define _DETECT_BASE_H_

#include "common.h"
#include "device.h"
#include "apiBase.h"
#include <functional>

// カメラ受信(検出/手動接続/待ち受け)の抽象基底。
// 上位層(cameraController)はカメラ種別を知らずに、この基底を通して検出・接続する。
// 種別依存の知識(サービス識別語・apiBase 生成・手動URL規則・SSDP待ち受け)は各派生に閉じる。
//  ・Canon CCAPI     : detectCanonCCapi (SSDP系: detectSsdpBase を継承)
//  ・Sony/Nikon/内蔵 : 将来 backend を追加(本改修外・構造のみ)
class detectBase
{
public:
	virtual ~detectBase() {}

	// 能動検出。見つかったこの種別のデバイス(apiBase 設定済み)を out に追加する。戻り=追加数。
	virtual size_t detect(std::vector<class device>& out) = 0;

	// IP 直指定でこの種別のデバイスを1台構築する(SSDP不使用)。
	//  成功時 out に apiBase 設定済みデバイスを入れて true。非対応/失敗は false。
	virtual bool makeManualDevice(const std::string& host, class device& out) = 0;

	// SSDP 等の受動待ち受け(Phase3 で実装)。既定は無効(何もしない)。
	//  onAppear : 対象カメラの出現(NOTIFY 受信等)を検知したときに呼ぶ。
	virtual void watchStart(std::function<void()> onAppear) { (void)onAppear; }
	virtual void watchStop() {}

protected:
	// この種別の apiBase を生成する(未 init)。生成失敗時 nullptr。
	virtual class apiBase* makeApi() = 0;
};

#endif // _DETECT_BASE_H_
