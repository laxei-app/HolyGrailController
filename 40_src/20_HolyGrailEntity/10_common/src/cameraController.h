#ifndef _CAMERA_CONTROLLER_H_
#define _CAMERA_CONTROLLER_H_

#include "common.h"
#include "device.h"
#include "detectBase.h"
#include "apiBase.h"		// 測光IF(meterResult)の中継に使う
#include <functional>
#include <memory>

// カメラ受信の薄いアグリゲータ。カメラ種別ごとの受信バックエンド(detectBase 派生)を束ね、
// 上位層(holyGrailEntity)へ種別非依存の検出/接続/操作を提供する。
// 検出/手動接続の種別依存知識は各バックエンドに閉じる(Canon=detectCanonCCapi。将来 Sony 等を追加)。
class cameraController
{
public:
public:
	// カメラの検出(全バックエンドを走査して device へ追加)
	// want を渡すと合致した1台だけを作って打ち切る(合わない台の apiBase を作らない)。
	//  確立時の内部RAMを削るため。詳細は detectBase::detect のコメント。
	static size_t detectTarget(std::vector<class device>& device, const detectBase::deviceMatch& want = nullptr);
	// 身元だけを確かめる検出(機種名/シリアル/愛称/IP)。**CCAPI を叩かない**ので apiBase は入らない。
	//  在否監視のように「そこに居るか」だけ知りたい側が使う。認証が要るカメラを、撮影主体でない側が
	//  叩くと認証がぶつかってカメラを締め出すため(EOS R50 V 実測 2026-08-16)。
	static size_t identifyTargets(std::vector<class device>& device);

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
	static errCode stopLiveView(const class device& device);			// ライブビュー停止
	static bool    liveViewNeededWhileCapturing(const class device& device);
	static errCode rdyShutter(const class device& device, const cmdt::shotSet& shotSet);
	// 露出を1項目ずつ設定(タイマ方式で変更のあった項目だけ適用するため)。上位が種別非依存に呼ぶ。
	static errCode setFNumber(const class device& device, const std::string& fNumber);
	static errCode setSS(const class device& device, const std::string& ss);
	static errCode setIso(const class device& device, const std::string& iso);
	static errCode actShutter(const class device& device);
	static errCode getSettings(const class device& device, cmdt::shotRange& settings);
	// カメラ自身の状態(記録メディア/電池/温度)を読む。
	static errCode readDeviceStatus(const class device& device, apiBase::deviceStatus& out);
	// 直近に撮れた画像からセンサー実寸[mm]と横画素数を読む(マスターに無い機種の穴埋め)。
	static errCode readSensorSpec(const class device& device, double& sensorWmm, double& sensorHmm, uint32_t& pixelW);
	static errCode rdyMetering(const class device& device);
	static errCode alzMetering(const class device& device, cmdt::HISTOGRAM& hist);
	// 測光(場面のリニア輝度の取得)。測り方の実装詳細はカメラ依存(apiBase実装側)。
	static errCode meterScene(const class device& device, const hgc::exposure& shotExp,
	                          apiBase::meterResult& out, const std::function<bool()>& keepGoing);
	static errCode meterHere(const class device& device, apiBase::meterResult& out,
	                         const std::function<bool()>& keepGoing);
	// 測光の間引きの状態を捨てる(初期収束は1枚1枚が高価なので、撮ったら必ず測る)。
	static void    resetMeterCadence(const class device& device);
	// 測光を「いつ呼ぶか」の申告を得る(カメラ未取得時は既定値=シャッター5秒前)。
	static apiBase::meterTiming meterTimingHint(const class device& device);
	// 「いま測光を始められるか」の軽い問い合わせ(busy計測)。1=可 / 0=まだ / -1=計測しない。
	// 測光方式の指定(所持カメラの meterLv をそのまま渡す)。セッション確立のたびに呼ぶ。
	static void    setMeterLv(const class device& device, bool useLv);
	static void    meterReset(const class device& device);
	static void    meterArm(const class device& device);	// 1枚目のシャッター直前の構え直し
	// 直近 alzMetering が解析したライブビューフレームのカメラ側取得時刻[ms]。0=不明。
	// 撮影開始時にカメラをM(ダイアル無視)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	static errCode setupShootingModeManual(const class device& device);
	static errCode restoreShootingMode(const class device& device);
	static errCode keepAlive(const class device& device);	// 接続維持用の無害なGET

protected:
	// 受信バックアンド群(初回アクセス時に生成。現状 Canon のみ。Sony/Nikon/内蔵は将来ここへ追加)。
	static std::vector<std::unique_ptr<detectBase>>& backends();
};

#endif // _CAMERA_CONTROLLER_H_
