// Android(NDK) のファイル保存実装(データ構造仕様書43 §8.1)。
// 保存先(アプリの外部ファイル領域)は JNI 経由で setBaseDir に渡される。
// POSIX のファイル I/O で追記・flush する。

#include "osFile.h"
#include <sys/stat.h>
#include <cstdio>
#include <mutex>
#include <string>

namespace
{
	std::mutex  g_mtx;
	std::string g_base;	// JNI から渡されるアプリ外部ファイル領域(.../files)

	// 末尾までのディレクトリを順次作成する(mkdir -p 相当)。
	void mkdirs(const std::string& dir)
	{
		for (size_t i = 1; i <= dir.size(); ++i)
		{
			if (i == dir.size() || dir[i] == '/')
			{
				std::string sub = dir.substr(0, i);
				if (!sub.empty()) { mkdir(sub.c_str(), 0770); }
			}
		}
	}
}

namespace osfile
{
	void setBaseDir(const std::string& dir)
	{
		std::lock_guard<std::mutex> lk(g_mtx);
		g_base = dir;
	}

	std::string dir(const std::string& name)
	{
		std::string base;
		{
			std::lock_guard<std::mutex> lk(g_mtx);
			base = g_base;
		}
		if (base.empty()) { return ""; }
		std::string d = base + "/" + name;
		mkdirs(d);
		return d;
	}

	std::string logDir(void)
	{
		return dir("log");
	}

	bool writeAll(const std::string& path, const char* data, size_t len)
	{
		std::string tmp = path + ".tmp";
		FILE* f = std::fopen(tmp.c_str(), "wb");
		if (f == nullptr) { return false; }
		size_t n = std::fwrite(data, 1, len, f);
		std::fflush(f);
		std::fclose(f);
		if (n != len) { std::remove(tmp.c_str()); return false; }
		std::remove(path.c_str());
		return std::rename(tmp.c_str(), path.c_str()) == 0;
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
}
