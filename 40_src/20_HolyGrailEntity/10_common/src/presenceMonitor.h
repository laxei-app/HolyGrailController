#ifndef _PRESENCE_MONITOR_H_
#define _PRESENCE_MONITOR_H_
// カメラ在否モニタ(共通)。スマホ役・エッジ役の両方が使う「同じ仕組み」。
//  ・起動時と定期(既定60s)に M-SEARCH(cameraController::detectTarget)でオンラインカメラを洗い出す。
//  ・間の各周期は既知オンライン個体への軽量HTTP疎通でオフライン化を素早く拾う。
//  ・受動NOTIFY(cameraController::watchStart)で新カメラ出現を検知したら即フル探索へ前倒し。
//  ・presence(見えているか)のみ保持し CCAPIセッションは張らない(占有しない)。
//  役割による違いは canScan 述語だけ:
//    スマホ = 常に true(常時スキャン)。
//    エッジ = 撮影中でない時だけ true(単一netThreadでシャッターI/Oと競合しないため。撮影中の在否はランナーが管理)。
#include "common.h"
#include "hgcCommon.h"	// hgc::camera
#include <functional>
#include <string>

namespace presenceMon
{
	// モニタ開始。onChange=マップ変化時に呼ぶ(スマホはこれで最新IPをエッジへpush)。
	// canScan=フル探索/疎通をしてよいか(false の周期はネットワークを触らずマップを保持)。空なら常に可。
	// useNotify=受動NOTIFY(SSDP watchStart)で新カメラ出現を即拾うか。
	//   スマホ=true(常時)。エッジ=false(SSDP共有リスナは entity 側=runner poke 用が使うため二重登録を避ける。
	//   在否は定期M-SEARCHで拾えば#1には十分)。
	void start(std::function<void()> onChange, std::function<bool()> canScan, bool useNotify = true);
	void stop(void);
	// 現在のマップを JSON 配列 [{serial,model,assignedName,ip,online}] で返す。
	std::string json(void);
	// 計画カメラの在否。1=オンライン / 0=オフライン / -1=不明(まだ一度も探索していない)。
	int presence(const hgc::camera& cam);
	// このカメラを「今すぐ」確かめる(#1: 撮影開始要求を受けた瞬間に×を出すため)。
	//  定期ループのオフライン化は TTL(150秒)待ちで遅いので、次のtick(<=1秒)で疎通を即確認し、
	//  失敗したら TTL を待たずにオフラインへ落とす。あわせてフル探索も前倒しする
	//  (IPが変わっただけの個体を取り逃がさない)。非ブロッキング=呼び出し側は待たない。
	void verifyNow(const hgc::camera& cam);
}

#endif // _PRESENCE_MONITOR_H_
