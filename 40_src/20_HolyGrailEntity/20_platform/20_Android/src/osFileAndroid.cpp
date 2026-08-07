// Android(NDK) のファイル保存実装(データ構造仕様書43 §8.1)。
// 保存先(アプリの外部ファイル領域)は JNI 経由で setBaseDir に渡される。
// POSIX のファイル I/O で追記・flush する。

#include "osFile.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

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

	std::vector<std::string> listFiles(const std::string& subdir,
	                                   const std::string& prefix,
	                                   const std::string& suffix)
	{
		std::vector<std::string> out;
		std::string d = dir(subdir);
		if (d.empty()) { return out; }
		DIR* dp = opendir(d.c_str());
		if (dp == nullptr) { return out; }
		struct dirent* e;
		while ((e = readdir(dp)) != nullptr)
		{
			std::string n = e->d_name;
			if (n == "." || n == "..") { continue; }
			bool okPre = prefix.empty() || n.rfind(prefix, 0) == 0;
			bool okSuf = suffix.empty() ||
			             (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0);
			if (okPre && okSuf) { out.push_back(n); }
		}
		closedir(dp);
		return out;
	}

	bool removeFile(const std::string& subdir, const std::string& name)
	{
		std::string d = dir(subdir);
		if (d.empty()) { return false; }
		return std::remove((d + "/" + name).c_str()) == 0;
	}

	std::vector<std::string> logFileNames(void)
	{
		std::vector<std::string> out;
		std::string d = logDir();
		if (d.empty()) { return out; }
		DIR* dp = opendir(d.c_str());
		if (dp == nullptr) { return out; }
		struct dirent* e;
		while ((e = readdir(dp)) != nullptr)
		{
			std::string n = e->d_name;
			if (n.rfind("hg_", 0) == 0) { out.push_back(n); }
		}
		closedir(dp);
		return out;
	}

	bool removeLog(const std::string& name)
	{
		std::string d = logDir();
		if (d.empty()) { return false; }
		return std::remove((d + "/" + name).c_str()) == 0;
	}

	// スマホの保存先はアプリ外部ファイル領域で、エッジの内蔵フラッシュのような逼迫は
	// 起きない。容量不明(false)として、容量基準のログ削除を働かせない(2026-08-08)。
	bool spaceInfo(unsigned long long& /*totalBytes*/, unsigned long long& /*usedBytes*/)
	{
		return false;
	}
}
