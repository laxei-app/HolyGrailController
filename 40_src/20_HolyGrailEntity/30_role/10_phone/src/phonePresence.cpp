#include "common.h"
#include "roleDiscovery.h"
#include "presenceMonitor.h"

// スマホ役の常駐プレゼンス(§3.2 / §5.4)。実体は共通の presenceMonitor(スマホ/エッジ共用)。
//  スマホは常時スキャン(canScan=常に true)。エッジ役は「撮影中でない時だけ」を渡す(edgeKnownCameras.cpp)。
namespace hge { namespace role {

void presenceStart(std::function<void()> onChange)
{
	presenceMon::start(std::move(onChange), std::function<bool()>());	// canScan 空=常に可
}

void presenceStop() { presenceMon::stop(); }

std::string presenceJson() { return presenceMon::json(); }

int cameraPresence(const hgc::camera& cam) { return presenceMon::presence(cam); }

}}	// namespace hge::role
