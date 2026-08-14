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

// §3.3 tier3: 限定サブネットのバッチ探索。前回IP直結(tier1)・M-SEARCH(tier2)が不発の時の最終手段
// (STA・スマホ不在)。自サブネットの :8080 を一括プローブし、応答IPへ connectManual + (model,serial)
// 本人確認して計画カメラを1台採れたら out に入れて true を返す。引数は tryIpDirect と同義。
// エッジ役(20_edge)が実装。スマホ役(10_phone)は常に false(発見はSSDPに委ねる)。
bool trySubnetSweep(const std::string& wantSerial, const hgc::camera& cam, bool hasModel,
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

// この端末の所持カメラ台帳が「設定の出所」として権威か。
//  スマホ役 = true : ユーザーが機材を登録し、測光方式などを設定する場所。
//  エッジ役 = false: 接続したカメラを自分でも記録する(識別用の控え)が、それは設定の出所ではない。
//   **エッジは受け取った撮影計画の値を信用すること**。ここを false にしないと、スマホが計画へ
//   載せて送った設定を、エッジが自分の控え(既定値のまま)で塗り潰す。
//   2026-08-14: 実際に R10 の測光方式(ライブビュー主体)が false へ戻され、サムネイルを
//   毎コマ取りに行って 178コマで Device busy に陥った。
bool ownedCamerasAuthoritative();

// --- スマホ役: 常駐プレゼンスマップ(§3.2 / §5.4) ---
// スマホが「オンラインのカメラ一覧」を常時保持する。起動時 M-SEARCH + 受動NOTIFY + 定期の
// 軽量疎通確認で最新化し、マップが変化(オンライン化/オフライン化/IP変更)するたび onChange を呼ぶ。
// 呼び手(スマホUI)は onChange を受けて、撮影中/待機中のエッジへ最新IPをプッシュする。
// マップは presence(見えているか)のみで CCAPIセッションは張らない(占有しない)。エッジ役は no-op。
void presenceStart(std::function<void()> onChange);
void presenceStop();
// 現在のオンラインカメラ一覧 JSON [{serial,model,ip,online}]。エッジ役は "[]"。
std::string presenceJson();

// 項目8: 計画カメラが現在オンラインか(占有せず=プレゼンス情報のみで判定。CCAPIセッションは張らない)。
//  戻り値 1=オンライン / 0=オフライン / -1=不明(情報が無い)。
//  スマホ役=常駐プレゼンスマップ(phonePresence)、エッジ役=既知カメラテーブル(スマホからのC_CAMERA_INFOで更新)。
//  照合は serial 優先(cam.serial 非空)、無ければ model 一致。撮影開始前の待機区間で カメラ/×アイコンを出すのに使う。
int cameraPresence(const hgc::camera& cam);
// #1: このカメラを今すぐ確かめる(撮影開始要求を受けた瞬間に×を出すため)。
//  定期監視のオフライン化はTTL待ちで遅い。非ブロッキングで即確認を促す。
void cameraPresenceVerify(const hgc::camera& cam);

}}  // namespace hge::role
