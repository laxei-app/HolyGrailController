// M5Stack(CoreS3) のファイル保存実装(データ構造仕様書43 §8.1)。
// microSD を優先し、無ければ内蔵フラッシュ(LittleFS)にフォールバックする。
// どちらも Arduino の fs::FS インターフェースで扱う。

#include "osFile.h"
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h>
#include <vector>
#include "debugOut.h"

namespace
{
	fs::FS*     g_fs = nullptr;	// 採用したファイルシステム(&SD or &LittleFS)
	bool        g_inited = false;

	// CoreS3 の microSD は SPI 共有(SCK=36, MISO=35, MOSI=37, CS=4)。
	constexpr int SD_SCK = 36, SD_MISO = 35, SD_MOSI = 37, SD_CS = 4;

	void ensureInit(void)
	{
		if (g_inited) { return; }
		g_inited = true;

		// 1) microSD を試す。CoreS3 は microSD と LCD が SPI を共有し、配線・カード相性で
		//    高クロックだと SD.begin が失敗することがあるため、クロックを段階的に下げて再試行する。
		SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
		static const uint32_t kSdHz[] = { 25000000, 20000000, 16000000, 10000000, 4000000, 1000000 };
		for (uint32_t hz : kSdHz)
		{
			if (SD.begin(SD_CS, SPI, hz))
			{
				g_fs = &SD;
				Serial.printf("[osfile] using SD (%u Hz, cardType=%u, %lluMB)\n",
				              (unsigned)hz, (unsigned)SD.cardType(),
				              (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
				return;
			}
			Serial.printf("[osfile] SD.begin failed at %u Hz\n", (unsigned)hz);
			SD.end();
		}

		// 2) 失敗したら内蔵フラッシュ(LittleFS)
		if (LittleFS.begin(true))	// true: 未フォーマットなら自動フォーマット
		{
			g_fs = &LittleFS;
			Serial.println("[osfile] SD not found, using LittleFS");
		}
		else
		{
			g_fs = nullptr;
			Serial.println("[osfile] no filesystem available");
		}
	}
}

namespace osfile
{
	void setBaseDir(const std::string& /*dir*/)
	{
		// M5Stack は保存先固定(SD/LittleFS)のため無視する。
	}

	std::string dir(const std::string& name)
	{
		ensureInit();
		if (g_fs == nullptr) { return ""; }
		std::string p = "/" + name;
		if (!g_fs->exists(p.c_str())) { g_fs->mkdir(p.c_str()); }
		return p;
	}

	std::string logDir(void)
	{
		return dir("log");
	}

	bool writeAll(const std::string& path, const char* data, size_t len)
	{
		ensureInit();
		if (g_fs == nullptr) { return false; }
		std::string tmp = path + ".tmp";
		File f = g_fs->open(tmp.c_str(), FILE_WRITE);	// FILE_WRITE は新規/切詰
		if (!f) { return false; }
		size_t n = f.write(reinterpret_cast<const uint8_t*>(data), len);
		f.flush();
		f.close();
		if (n != len) { g_fs->remove(tmp.c_str()); return false; }
		g_fs->remove(path.c_str());	// rename先が存在すると失敗するFSがあるため消す
		return g_fs->rename(tmp.c_str(), path.c_str());
	}

	bool append(const std::string& path, const char* data, size_t len)
	{
		ensureInit();
		if (g_fs == nullptr) { return false; }
		File f = g_fs->open(path.c_str(), FILE_APPEND);
		if (!f) { return false; }
		size_t n = f.write(reinterpret_cast<const uint8_t*>(data), len);
		f.flush();
		f.close();
		return n == len;
	}

	bool readAll(const std::string& path, std::string& out)
	{
		ensureInit();
		out.clear();
		if (g_fs == nullptr) { return false; }
		File f = g_fs->open(path.c_str(), FILE_READ);
		if (!f) { return false; }
		while (f.available()) { out.push_back(static_cast<char>(f.read())); }
		f.close();
		return true;
	}

	const char* backendName(void)
	{
		ensureInit();
		if (g_fs == &SD)       { return "SD"; }
		if (g_fs == &LittleFS) { return "LittleFS"; }
		return "none";
	}

	std::vector<std::string> listFiles(const std::string& subdir,
	                                   const std::string& prefix,
	                                   const std::string& suffix)
	{
		ensureInit();
		std::vector<std::string> out;
		if (g_fs == nullptr) { return out; }
		std::string dpath = "/" + subdir;
		File d = g_fs->open(dpath.c_str());
		if (!d) { return out; }
		if (!d.isDirectory()) { d.close(); return out; }
		for (File f = d.openNextFile(); f; f = d.openNextFile())
		{
			std::string nm = f.name();
			size_t slash = nm.find_last_of('/');
			std::string base = (slash == std::string::npos) ? nm : nm.substr(slash + 1);
			bool okPre = prefix.empty() || base.rfind(prefix, 0) == 0;
			bool okSuf = suffix.empty() ||
			             (base.size() >= suffix.size() && base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0);
			if (okPre && okSuf) { out.push_back(base); }
			f.close();
		}
		d.close();
		return out;
	}

	bool removeFile(const std::string& subdir, const std::string& name)
	{
		ensureInit();
		if (g_fs == nullptr) { return false; }
		std::string p = "/" + subdir + "/" + name;
		return g_fs->remove(p.c_str());
	}

	std::vector<std::string> logFileNames(void)
	{
		ensureInit();
		std::vector<std::string> out;
		if (g_fs == nullptr) { return out; }
		File d = g_fs->open("/log");
		if (!d) { return out; }
		if (!d.isDirectory()) { d.close(); return out; }
		for (File f = d.openNextFile(); f; f = d.openNextFile())
		{
			std::string nm = f.name();
			size_t slash = nm.find_last_of('/');
			std::string base = (slash == std::string::npos) ? nm : nm.substr(slash + 1);
			if (base.rfind("hg_", 0) == 0) { out.push_back(base); }
			f.close();
		}
		d.close();
		return out;
	}

	bool removeLog(const std::string& name)
	{
		ensureInit();
		if (g_fs == nullptr) { return false; }
		std::string p = "/log/" + name;
		return g_fs->remove(p.c_str());
	}

	int removeInternalLogs(void)
	{
		// 内蔵フラッシュ(LittleFS)を明示的に開き /log のファイルを全削除する。
		// SD 採用中でも内蔵側ログを消したいので g_fs ではなく LittleFS を直接使う。
		if (!LittleFS.begin(true)) { return -1; }
		File d = LittleFS.open("/log");
		if (!d) { return 0; }
		if (!d.isDirectory()) { d.close(); return 0; }

		std::vector<std::string> paths;
		for (File f = d.openNextFile(); f; f = d.openNextFile())
		{
			std::string nm = f.name();
			// core により name() がフルパス/ベース名のどちらでも動くよう整える。
			std::string full = (!nm.empty() && nm[0] == '/') ? nm : (std::string("/log/") + nm);
			paths.push_back(full);
			f.close();
		}
		d.close();

		int n = 0;
		for (const auto& p : paths) { if (LittleFS.remove(p.c_str())) { ++n; } }
		return n;
	}
}
