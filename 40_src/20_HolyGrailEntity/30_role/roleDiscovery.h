#pragma once
// 役割別の発見オーケストレーション(30_role)への窓口(ポート)。
//  ・エッジ役(20_edge): 既知IPテーブル + IP直結 + (model,serial)本人確認 を実装(§3.3)。
//  ・スマホ役(10_phone): スタブ(発見はスマホ自身のSSDPで行うため常にSSDPへ委ねる)。
// 各成果物は自分の役割の実装だけをリンクする(M5Stack=20_edge / Android=10_phone)。
// 依存の向き: role → common(抽象IF/プリミティブ)。common は role の“実体”を include しない。
#include <string>
#include <functional>
#include "hgcCommon.h"   // hgc::camera

class device;

namespace hge { namespace role {

// IP直結を試みる。既知カメラ(スマホプッシュ由来)から計画カメラのIPを引き、connectManual +
// (model, serial) 検証まで済ませて out に1台入れられたら true を返す(呼び手はSSDPを省略してよい)。
//  ・wantSerial 非空 → serial一致のIPへ直結し (serial かつ model) を確認。
//  ・wantSerial 空 かつ hasModel → 機種一致がオンライン1台の曖昧さ無しケースのみ直結(model確認)。
//  ・serialBusy: その serial が他の動作中セッションで使用中か(二重撮影防止。model一意枝で使用)。
bool tryIpDirect(const std::string& wantSerial, const hgc::camera& cam, bool hasModel,
                 const std::function<bool(const std::string&)>& serialBusy,
                 device& out);

// スマホから受けた発見中オンラインカメラ一覧(JSON配列 [{serial,model,ip,online}])で既知テーブルを更新。
// 戻り値は errCode 相当(0=OK)。スマホ役では no-op(0)。
int setKnownCameras(const char* json, int len);

// 接続・本人確認に成功したカメラ(serial, model, ip)を既知テーブルへ記録し、エッジ役は不揮発
// (/asset/knownCams.json)へ保存する。無人再起動後の「前回IP直結」(§3.3 tier1)を可能にする。
// IPはヒントであり、次回接続時に (model, serial) で必ず本人確認する(誤接続はしない)。スマホ役は no-op。
void noteConnected(const std::string& serial, const std::string& model, const std::string& ip);

// 起動時に不揮発の既知カメラを読み込む(エッジ役)。スマホ役は no-op。
void loadPersisted();

// --- スマホ役: 常駐プレゼンスマップ(§3.2 / §5.4) ---
// スマホが「オンラインのカメラ一覧」を常時保持する。起動時 M-SEARCH + 受動NOTIFY + 定期の
// 軽量疎通確認で最新化し、マップが変化(オンライン化/オフライン化/IP変更)するたび onChange を呼ぶ。
// 呼び手(スマホUI)は onChange を受けて、撮影中/待機中のエッジへ最新IPをプッシュする。
// マップは presence(見えているか)のみで CCAPIセッションは張らない(占有しない)。エッジ役は no-op。
void presenceStart(std::function<void()> onChange);
void presenceStop();
// 現在のオンラインカメラ一覧 JSON [{serial,model,ip,online}]。エッジ役は "[]"。
std::string presenceJson();

}}  // namespace hge::role
