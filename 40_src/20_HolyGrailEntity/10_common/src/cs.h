#ifndef _CS_H_
#define _CS_H_
// データ構造仕様書(43) 第4章 撮影計画 (Capture Schedule = cs)。
// エッジ端末へ転送して単独撮影できるよう必要情報をすべて埋め込み自己完結させる。

#include "hgcCommon.h"
#include "ccm.h"
#include <memory>
#include <vector>

namespace hgc
{
	// 4.1 スケジュールに入れるイベント種別
	enum class csEvent : uint8_t
	{
		invalid = 0,		// 無効
		start,				// 開始
		sunset,				// 日の入り
		civilDusk,			// 市民薄明の終わり
		nauticalDusk,		// 航海薄明の終わり
		astronomicalDusk,	// 天文薄明の終わり
		astronomicalDawn,	// 天文薄明の始まり
		nauticalDawn,		// 航海薄明の始まり
		civilDawn,			// 市民薄明の始まり
		sunrise,			// 日の出
		moonrise,			// 月の出
		moonset,			// 月の入り
		end					// 終了
	};

	// 4.2 イベント要素 (events は eventItem の配列)
	struct eventItem
	{
		csEvent  event = csEvent::invalid;	// 時系列な csEvent
		dateTime when;						// イベント発生時刻
	};

	// 4.3 撮影制御方法の適用区間 (ccmList の要素)
	struct ccmWindow
	{
		dateTime start;					// 適用開始時刻
		dateTime end;					// 適用終了時刻
		ccmType  type = ccmType::invalid;	// どの撮影制御方法か(保存はこれだけ。実体は cs::ccm 側)
		// 計画が所有する実体(cs::ccm)への参照。**複製しない**ので、計画の ccm を編集すれば
		// 同じ型の窓すべてに反映される。移行(preNight/postNight)は実体を持たないのでその場で作る。
		std::shared_ptr<ccmBase> ccm;
	};

	// 撮影制御方法の境目(隣接する窓の境界)の時刻を手動で上書きする指定。
	// (before→after) の型ペアの occ 番目(0始まり)の境目を when の時刻へ動かす。
	// スケジュール再生成(時刻/方向/機材変更)後も型ペア+出現順で同定して再適用する。
	struct boundaryOverride
	{
		ccmType  before = ccmType::invalid;	// 境目の前の窓の種別
		ccmType  after  = ccmType::invalid;	// 境目の後の窓の種別
		uint16_t occ    = 0;				// 同じ型ペアの出現順(0始まり)
		dateTime when;						// 上書きする境目の時刻(後方互換/表示用)
		// 境目の太陽高度(移動範囲でクランプ済み)。適用時はこの高度から現在の窓内で時刻を再計算するため、
		// 撮影日時を変えても正しい高度で再適用できる(旧データは -100=未設定で when にフォールバック)。
		double   altDeg = -100.0;
		bool     rising = false;			// 朝(上昇)=true / 夕(下降)=false
	};

	// 4.5 撮影計画
	struct cs
	{
		std::string name;				// 名前
		dateTime    start;				// 撮影開始の日時
		dateTime    end;				// 撮影終了の日時
		hgc::place  place;				// 位置情報
		hgc::camera camera;				// 使用するカメラ(実体)
		hgc::lens   lens;				// 使用するレンズ(実体)
		double      interval  = 0.0;	// 撮影周期[秒]
		double      azimuth   = 0.0;	// 開始時の撮影方位[°] 0.0～359.9
		double      elevation = 0.0;	// 開始時の仰角[°] -90.0～90.0
		bool        landscape = true;	// 横向きで撮る(ランドスケープ)
		std::vector<eventItem> events;	// 撮影計画のイベント
		// この計画が所有する撮影制御方法一式。**ここだけが権威**(仕様3.7)。
		// 初期値から取り込むのは新規作成時と初期値リスト選択時だけ。
		ccmOwned ccm;
		// 適用区間。ccm を材料に buildSchedule が組み立てる派生物(保存は時刻+型のみ)。
		std::vector<ccmWindow> ccmList;
		std::vector<boundaryOverride> boundaries;		// 境目の時刻上書き
		// 夜間撮影の固定露出と移行目標ev。スケジュールに夜間ウィンドウが無くても(終了時刻が夜間より前でも)
		// 夜間前/後移行のクランプ(暗所限界)・基準(home)・ss上限として常に使えるよう、
		// buildSchedule が夜間プリセットから設定し csJson で永続/転送する(仕様3.7/3.9)。
		exposure nightFixedExposure;		// 夜間撮影の固定露出(=暗所限界/基準/ss上限の基準)
		double   nightPreNightEv  = 0.0;	// 夜間前移行の目標ev(夜間プリセット由来)
		double   nightPostNightEv = 0.0;	// 夜間後移行の目標ev(夜間プリセット由来)
	};

	// 4.6 撮影計画プリセット (実体は持たず名称で関連付ける)
	struct planPreset
	{
		std::string name;					// プリセット名
		std::string cameraName;				// 使用するカメラの名称
		std::string lensName;				// 使用するレンズの名称
		double      interval  = 0.0;		// 撮影周期[秒]
		double      azimuth   = 0.0;		// 開始時の撮影方位[°]
		double      elevation = 0.0;		// 開始時の仰角[°]
		bool        landscape = true;		// 横向きで撮る(ランドスケープ)
		std::vector<std::string> ccmNames;	// 使用する撮影制御方法の名称一覧
		bool        autoInsert = false;		// 新規作成時に最初に選択する
	};
}

#endif // _CS_H_
