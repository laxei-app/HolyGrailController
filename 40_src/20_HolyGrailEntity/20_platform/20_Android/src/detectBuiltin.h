#ifndef _DETECT_BUILTIN_H_
#define _DETECT_BUILTIN_H_
// スマホ内蔵カメラの検出(2026-09-05)。
//
// 【他のバックエンドとの違い】「検出」がネットワーク探索ではなく**端末内の列挙**になる。
//  そのため SSDP も待ち受けも IP 直指定も無い。認証も締め出しも無いので、
//  identify(身元だけ確かめる)を detect と分ける意味も無い。
//
// 【対象は物理カメラだけ】CameraManager.getCameraIdList が返す id を1台ずつ扱う。
//  広角・超広角・望遠がそれぞれ別のカメラとして所持カメラに並ぶ。
#include "detectBase.h"

class detectBuiltin : public detectBase
{
public:
	size_t detect(std::vector<class device>& out, const deviceMatch& want = nullptr) override;
	size_t identify(std::vector<class device>& out) override;

	// IP の概念が無いので、手動接続も IP 直指定の身元確認も持たない。
	bool makeManualDevice(const std::string& host, class device& out) override
	{ (void)host; (void)out; return false; }
	bool identifyAt(const std::string& host, class device& out) override
	{ (void)host; (void)out; return false; }

protected:
	class apiBase* makeApi(void) override;

private:
	// 列挙して device を組み立てる。withApi=false なら apiBase を作らない(身元だけ)。
	size_t build(std::vector<class device>& out, const deviceMatch& want, bool withApi);
};

#endif // _DETECT_BUILTIN_H_
