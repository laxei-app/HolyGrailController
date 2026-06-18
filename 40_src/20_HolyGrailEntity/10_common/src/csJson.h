#ifndef _CS_JSON_H_
#define _CS_JSON_H_
// 撮影計画(cs)と撮影制御方法(ccm)の JSON 相互変換(データ構造仕様書43 §4/§3/§6.4)。
// エッジ端末へ転送する自己完結 cs を JSON 化/復元する。key は構造体メンバ名に一致させる。

#include "cs.h"
#include "ccm.h"
#include "hgcCommon.h"
#include <memory>
#include <string>
#include <vector>

namespace csjson
{
	// 撮影計画 cs を自己完結 JSON 文字列にする(place/camera/lens/events/ccmList を含む)。
	std::string toJson(const hgc::cs& plan);

	// JSON 文字列から撮影計画 cs を復元する。return: 成功
	bool fromJson(const std::string& s, hgc::cs& plan);

	// 撮影制御方法 ccm を JSON 文字列にする(controlMethod 個別転送用)。
	std::string ccmToJson(const hgc::ccmBase& ccm);

	// JSON 文字列から撮影制御方法 ccm を復元する。失敗時 nullptr。
	std::shared_ptr<hgc::ccmBase> ccmFromJson(const std::string& s);

	// --- 所持機材(§5.5/5.6)。内部形式(/asset/ownedCameras.json・ownedLenses.json) ---
	std::string ownedCamerasToJson(const std::vector<hgc::ownedCamera>& list);
	bool        ownedCamerasFromJson(const std::string& s, std::vector<hgc::ownedCamera>& out);
	std::string ownedLensesToJson(const std::vector<hgc::lens>& list);
	bool        ownedLensesFromJson(const std::string& s, std::vector<hgc::lens>& out);

	// --- 機材マスタ(§5.8/5.9)。30_refer 由来の読取専用形式 → 内部 camera/lens へマップ ---
	bool        camerasFromMasterJson(const std::string& s, std::vector<hgc::camera>& out);
	bool        lensesFromMasterJson(const std::string& s, std::vector<hgc::lens>& out);
}

#endif // _CS_JSON_H_
