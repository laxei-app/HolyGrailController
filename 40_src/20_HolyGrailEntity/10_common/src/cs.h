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
		std::shared_ptr<ccmBase> ccm;	// 撮影制御方法(実体)
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
		std::vector<ccmWindow> ccmList;	// 撮影制御方法(実体をコピー)
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
