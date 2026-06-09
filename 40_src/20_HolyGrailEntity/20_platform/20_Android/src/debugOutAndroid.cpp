// Android 用デバッグ出力。logcat(__android_log)へ出力する。
// cons(コンソール)はテスト用途のため Android ではスタブ。

#include "common.h"
#include "debugOut.h"
#include "commonAndroid.h"

#include <cstdarg>
#include <cstdio>
#include <string>

// ---------------- dbg ----------------
void dbg::init(void)
{
}

void dbg::ln(col /*c*/, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	__android_log_write(ANDROID_LOG_DEBUG, HGE_LOG_TAG, buf);
}

void dbg::trace(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	__android_log_write(ANDROID_LOG_DEBUG, HGE_LOG_TAG, buf);
}

std::string dbg::escSeq(col /*c*/)
{
	return std::string();	// Android(logcat)では色エスケープは使わない
}

void dbg::dump(uint32_t addr, const void* mem, uint32_t size)
{
	const unsigned char* p = static_cast<const unsigned char*>(mem);
	std::string line;
	char tmp[16];
	for (uint32_t i = 0; i < size; ++i)
	{
		if ((i % 16) == 0)
		{
			if (!line.empty()) { __android_log_write(ANDROID_LOG_DEBUG, HGE_LOG_TAG, line.c_str()); }
			snprintf(tmp, sizeof(tmp), "%08X: ", addr + i);
			line = tmp;
		}
		snprintf(tmp, sizeof(tmp), "%02X ", p[i]);
		line += tmp;
	}
	if (!line.empty()) { __android_log_write(ANDROID_LOG_DEBUG, HGE_LOG_TAG, line.c_str()); }
}

// ---------------- cons (Android ではスタブ) ----------------
int cons::keyVal = 0;

void cons::clr(void) {}

void cons::printf(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	__android_log_write(ANDROID_LOG_INFO, HGE_LOG_TAG, buf);
}

int  cons::getch(void) { return 0; }
int  cons::kbhit(void) { return 0; }
void cons::getRowCol(int& row, int& col) { row = 0; col = 0; }
void cons::setRowCol(int /*row*/, int /*col*/) {}
void cons::attr(col /*coller*/) {}
