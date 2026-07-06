#include "roleDiscovery.h"

// スマホ役の発見: スマホ自身が M-SEARCH(必要に応じ NOTIFY)で信頼できる発見を行うため、
// エッジのような既知IPテーブルは持たない。IP直結ヒントは常に空を返し、上位は通常のSSDP発見に進む。
// このファイルはスマホ成果物(Android / 将来 iPhone)だけがリンクする(エッジは 20_edge の実装)。
// 常駐プレゼンスマップ(§5.4)を実装する場合もこのファイル(10_phone)側に置く。

namespace hge { namespace role {

bool tryIpDirect(const std::string& /*wantSerial*/, const hgc::camera& /*cam*/, bool /*hasModel*/,
                 const std::function<bool(const std::string&)>& /*serialBusy*/,
                 device& /*out*/)
{
	return false;	// スマホ役はIP直結ヒントを持たない → 常にSSDP発見へ委ねる
}

int setKnownCameras(const char* /*json*/, int /*len*/)
{
	return 0;	// ERR_HGC_OK 相当。スマホは push 受信側ではないため no-op
}

void noteConnected(const std::string& /*serial*/, const std::string& /*model*/, const std::string& /*ip*/)
{
	// スマホ役は既知IPテーブル/不揮発キャッシュを持たない(発見は都度スマホのSSDPで行う)ため no-op。
}

void loadPersisted()
{
	// スマホ役は不揮発の既知カメラを持たない。no-op。
}

}}	// namespace hge::role
