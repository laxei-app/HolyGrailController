#ifndef _CAMERA_CONTROLLER_H_
#define _CAMERA_CONTROLLER_H_

#include "common.h"
#include "device.h"
#include "detectBase.h"
#include <memory>

// カメラ受信の薄いアグリゲータ。カメラ種別ごとの受信バックエンド(detectBase 派生)を束ね、
// 上位層(holyGrailEntity)へ種別非依存の検出/接続/操作を提供する。
// 検出/手動接続の種別依存知識は各バックエンドに閉じる(Canon=detectCanonCCapi。将来 Sony 等を追加)。
class cameraController
{
public:
public:
	// カメラの検出(全バックエンドを走査して device へ追加)
	static size_t detectTarget(std::vector<class device>& device);

	// IP直指定でカメラに接続する(SSDPを使わない。エミュレータ等での手動接続用)。
	//  host : カメラのIPアドレス(例 "192.168.1.4")。対応バックエンドが接続を試みる。
	//  return : 接続できたデバイス数(0 or 1)
	static size_t connectManual(std::vector<class device>& device, const std::string& host);

	// SSDP受動待ち受けの開始/停止(3b)。全バックエンドへ委譲する。onAppear はカメラの出現(NOTIFY)
	// 検知時に呼ばれる。撮影要求中にカメラ未検出のセッションがある間だけ稼働させ、出現を60秒待たず拾う。
	static void watchStart(std::function<void()> onAppear);
	static void watchStop();

	// カメラへのアクセス
	static errCode startShooting(const class device& device);
	static errCode rdyShutter(const class device& device, const cmdt::shotSet& shotSet);
	static errCode actShutter(const class device& device);
	static errCode getSettings(const class device& device, cmdt::shotRange& settings);
	static errCode rdyMetering(const class device& device);
	static errCode alzMetering(const class device& device, cmdt::HISTOGRAM& hist);
	// 撮影開始時にカメラをM(ダイアル無視)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	static errCode setupShootingModeManual(const class device& device);
	static errCode restoreShootingMode(const class device& device);
	static errCode keepAlive(const class device& device);	// 接続維持用の無害なGET

protected:
	// 受信バックアンド群(初回アクセス時に生成。現状 Canon のみ。Sony/Nikon/内蔵は将来ここへ追加)。
	static std::vector<std::unique_ptr<detectBase>>& backends();
};

#endif // _CAMERA_CONTROLLER_H_
