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

	bool readAll(const std::string& path, std::string& out)
	{
		out.clear();
		FILE* f = std::fopen(path.c_str(), "rb");
		if (f == nullptr) { return false; }
		char buf[1024];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { out.append(buf, n); }
		std::fclose(f);
		return true;
	}

	bool readRange(const std::string& path, size_t offset, size_t maxLen, std::string& out)
	{
		out.clear();
		FILE* f = std::fopen(path.c_str(), "rb");
		if (f == nullptr) { return false; }
		if (offset > 0) { std::fseek(f, static_cast<long>(offset), SEEK_SET); }
		out.resize(maxLen);
		size_t n = std::fread(&out[0], 1, maxLen, f);
		out.resize(n);
		std::fclose(f);
		return true;
	}

	// 検証用。容量不明(false)として容量基準のログ削除を働かせない(2026-08-08)。
	bool spaceInfo(unsigned long long& /*totalBytes*/, unsigned long long& /*usedBytes*/)
	{
		return false;
	}

	// 保存先へ実際に書けるか。この環境ではアプリ専用領域なので常に真。
	bool writable(void) { return true; }
}
