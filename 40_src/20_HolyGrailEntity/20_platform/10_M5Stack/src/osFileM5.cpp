// M5Stack(CoreS3) のファイル保存実装(データ構造仕様書43 §8.1)。
// microSD を優先し、無ければ内蔵フラッシュ(LittleFS)にフォールバックする。
// どちらも Arduino の fs::FS インターフェースで扱う。

#include "osFile.h"
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h>
#include "debugOut.h"

namespace
{
	fs::FS*     g_fs = nullptr;	// 採用したファイルシステム(&SD or &LittleFS)
	std::string g_base;			// ログのベースディレクトリ
	bool        g_inited = false;

	// CoreS3 の microSD は SPI 共有(SCK=36, MISO=35, MOSI=37, CS=4)。
	constexpr int SD_SCK = 36, SD_MISO = 35, SD_MOSI = 37, SD_CS = 4;

	void ensureInit(void)
	{
		if (g_inited) { return; }
		g_inited = true;

		// 1) microSD を試す
		SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
		if (SD.begin(SD_CS, SPI, 25000000))
		{
			g_fs = &SD;
			g_base = "/log";
			DBGLN(col::GRN, "osfile: using SD");
		}
		// 2) 失敗したら内蔵フラッシュ(LittleFS)
		else if (LittleFS.begin(true))	// true: 未フォーマットなら自動フォーマット
		{
			g_fs = &LittleFS;
			g_base = "/log";
			DBGLN(col::YEL, "osfile: SD not found, using LittleFS");
		}
		else
		{
			g_fs = nullptr;
			g_base = "";
			DBGLN(col::RED, "osfile: no filesystem available");
			return;
		}

		if (g_fs && !g_fs->exists(g_base.c_str())) { g_fs->mkdir(g_base.c_str()); }
	}
}

namespace osfile
{
	void setBaseDir(const std::string& /*dir*/)
	{
		// M5Stack は保存先固定(SD/LittleFS)のため無視する。
	}

	std::string logDir(void)
	{
		ensureInit();
		return g_base;
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
}
