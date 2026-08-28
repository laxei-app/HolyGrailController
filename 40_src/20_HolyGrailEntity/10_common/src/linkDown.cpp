#include "linkDown.h"
#include <map>
#include <mutex>

// 説明はヘッダを参照。
namespace
{
	// 覚えるのは IP と世代番号だけ。撮影中に見る相手は数台なので、素直な map で足りる。
	//  Wi-Fi のイベントタスクと撮影スレッドの両方から触るので錠を掛ける。
	std::mutex                          g_mtx;
	std::map<std::string, unsigned>     g_gen;
}

namespace linkDown
{
	void note(const std::string& ip)
	{
		if (ip.empty()) { return; }
		std::lock_guard<std::mutex> lk(g_mtx);
		++g_gen[ip];
	}

	unsigned generation(const std::string& ip)
	{
		if (ip.empty()) { return 0; }
		std::lock_guard<std::mutex> lk(g_mtx);
		auto it = g_gen.find(ip);
		return (it == g_gen.end()) ? 0u : it->second;
	}
}
