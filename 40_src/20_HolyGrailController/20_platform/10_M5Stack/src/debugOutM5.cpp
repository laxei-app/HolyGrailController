#include "debugOut.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <type_traits>
#include "commonM5.h"

// debug 用出力
void dbg::init(void)
{
    Serial.begin(115200);
}

void dbg::ln(enum col col, const char * fmt, ...)
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

    // 色の指定
    auto color = dbg::escSeq(col);

    // 出力
    Serial.printf(color.c_str());           // 色出力
    Serial.printf(buf.data());
    Serial.printf("\n");            // 改行付加
    Serial.printf("\e[0m");         // 色属性のリセット
}

void dbg::trace(const char * fmt, ...)
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

    // 出力
    Serial.printf(buf.data());
}

// 標準入出力のラッピング
// 画面のクリア
void cons::clr(void)
{
    M5.Display.clear();
    M5.Display.setCursor(0, 0);
}

// printf() のラッピング
void cons::printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);     // 可変引数の解析開始（formatの直後の引数から）
    M5.Display.vprintf(fmt, args);     // printf の va_list 版である vprintf を呼び出す
    va_end(args);           // 後片付け
}

// キーの値一時保存領域
int cons::keyVal;

// コンソール入力
int cons::getch(void)
{
    while(1)
    {
        if(!kbhit() ){ continue; }
        return keyVal;
    }
}

// コンソール入力あり？
int cons::kbhit(void)
{
    M5.update(); // ボタン状態の更新（Core2のタッチもこれで判定）
    if(M5.BtnA.wasPressed())
    { 
        keyVal = 'a';
        return 1;
    }
    if(M5.BtnB.wasPressed())
    { 
        keyVal = 'b';
        return 1;
    }
    if(M5.BtnC.wasPressed())
    { 
        keyVal = 'c';
        return 1;
    }
    return 0;
}

// 現在座標を取得
void cons::getRowCol(int& row, int& col)
{
    col = M5.Display.getCursorX()/M5.Display.fontWidth();
    row = M5.Display.getCursorY()/M5.Display.fontHeight();
}

// 座標を設定
void cons::setRowCol(int row, int col)
{
    auto w = M5.Display.fontWidth();
    auto h = M5.Display.fontHeight();

    M5.Display.setCursor(col*w, row*h);
}

// 色、反転、指定
void cons::attr(col coller)
{
    int m5Coller[] =
    {
        TFT_WHITE,
        TFT_YELLOW,
        TFT_GREEN,
        TFT_CYAN,
        TFT_MAGENTA,
        TFT_RED,
    };
    int targetCol = m5Coller[static_cast<int>(coller) & static_cast<int>(col::MSK)];
    int backCol = TFT_BLACK;
    if(static_cast<int>(coller) & static_cast<int>(col::REV))
    {
        backCol = targetCol;
        targetCol = TFT_BLACK;
    }
    M5.Display.setTextColor(targetCol, backCol);
}
