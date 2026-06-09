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

// escSeq / dump は共通の debugOut.cpp が実装するためここでは定義しない。

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
