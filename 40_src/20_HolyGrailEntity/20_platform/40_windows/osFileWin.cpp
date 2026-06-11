// Windows のファイル保存実装(データ構造仕様書43 §8.1)。
// 主に開発・テスト用。setBaseDir で指定が無ければ実行ディレクトリ下の log を使う。

#include "osFile.h"
#include <filesystem>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
	std::mutex  g_mtx;
	std::string g_base;
}

namespace osfile
{
	void setBaseDir(const std::string& dir)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_base = dir;
	}

	std::string logDir(void)
	{
		std::string base;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			base = g_base;
		}
		std::filesystem::path dir = base.empty()
			? (std::filesystem::current_path() / "log")
			: (std::filesystem::path(base) / "log");
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		if (ec) { return ""; }
		return dir.string();
	}

	bool append(const std::string& path, const char* data, size_t len)
	{
		FILE* f = std::fopen(path.c_str(), "ab");
		if (f == nullptr) { return false; }
		size_t n = std::fwrite(data, 1, len, f);
		std::fflush(f);
		std::fclose(f);
		return n == len;
	}
}
