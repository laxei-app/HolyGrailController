#include "debugOut.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <type_traits>

// debug 用出力
std::string dbg::escSeq(col col)
{
    // 色の指定
    const char* colSample[] =
    {
        "7m",           // 白
        "3m",           // 黄色
        "2m",           // 緑
        "6m",           // シアン
        "5m",           // マゼンタ
        "1m",           // 赤
    };
    const char reset[] = "\x1b[0m";            // リセット

    char color[8] = "";
    std::memcpy(color, reset, sizeof(reset));
    size_t ix = (uint32_t)col & (uint32_t)col::MSK;
    if (ix < std::extent<decltype(colSample)>::value)
    {   // インデックス範囲なら色指定をやる
        const char* base = colSample[ix];
        const char* bold = "";
        const char* revs = "3";                // 標準
        if ((uint32_t)col & (uint32_t)col::BLD) { bold = "1;"; };  // 太字
        if ((uint32_t)col & (uint32_t)col::REV) { revs = "4"; };   // 反転
        snprintf(color, sizeof(color), "\x1b""[%s%s%s", bold, revs, base);
    }
    return color;
}

// メモリダンプ
void dbg::dump(uint32_t addr, const void* mem, uint32_t size)
{
    const uint8_t* memIx = static_cast<const uint8_t*>(mem);
    for(uint32_t ix = 0; ix < size;)
    {
        dbg::trace("0x%08x :", addr);
        for(uint32_t ofs = 0; (ofs < 16) || (ofs+ix < size); ofs++, ix++, addr++)
        {
            if(ofs%4 == 0 ){dbg::trace(" ");}
            dbg::trace("%02x", *memIx++);
        }
        dbg::trace("\n");
    }
}
