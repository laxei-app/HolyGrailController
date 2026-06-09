#include "debugOut.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <Windows.h>
#include <conio.h>

// 初期化。やることは無い。
void dbg::init(void) {}

// debug 用出力
void dbg::ln(col col, const char * fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    // 1. 必要なバッファサイズを計算 (終端ヌル文字含まず)
    int size = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (size <= 0) return;

    // 2. バッファを確保して書き込み
    std::vector<char> buf(size + 1);
    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);

    // 3. 出力
    OutputDebugStringA(buf.data());
    OutputDebugStringA("\n"); // 改行付加
}
// debug 用出力
void dbg::trace(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    // 1. 必要なバッファサイズを計算 (終端ヌル文字含まず)
    int size = vsnprintf(nullptr, 0, fmt, args);
    va_end(args);

    if (size <= 0) return;

    // 2. バッファを確保して書き込み
    std::vector<char> buf(size + 1);
    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    va_end(args);

    // 3. 出力
    OutputDebugStringA(buf.data());
}

// 標準入出力のラッピング
int cons::keyVal;       // windows では使わない

// 画面のクリア
void cons::clr(void)
{
    printf("\033[2J\033[H");				// クリア

}

// printf() のラッピング
void cons::printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);     // 可変引数の解析開始（formatの直後の引数から）
    vprintf(fmt, args);     // printf の va_list 版である vprintf を呼び出す
    va_end(args);           // 後片付け


}

// コンソール入力
int cons::getch(void)
{
    return _getch();
}

// コンソール入力あり？
int cons::kbhit(void)
{
    return _kbhit();
}

// 現在座標を取得
void cons::getRowCol(int& row, int& col)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        col = csbi.dwCursorPosition.X;
        row = csbi.dwCursorPosition.Y;
    }
}

// 座標を設定
void cons::setRowCol(int row, int col)
{
    COORD coord;
    coord.X = col;
    coord.Y = row;
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), coord);
}

// 色、反転、太字の指定
void cons::attr(col coller)
{
    cons::printf(dbg::escSeq(coller).c_str());
}

