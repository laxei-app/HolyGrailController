#ifndef  _DEBUG_OUT_H_
#define  _DEBUG_OUT_H_
#include <common.h>

#ifdef _DEBUG
#define DBGLN(col, fmt, ...) dbg::ln(col, fmt, ##__VA_ARGS__)
#define DUMP(adr, buf, len) dbg::dump(adr, buf, len)

#else
#define DBGLN(col, fmt, ...) ((void)0)
#define DUMP(adr, buf, len) ((void)0))
#endif

enum class col
{
    // 色の属性
    WHT = 0x00000000,       // 白
    YEL = 0x00000001,       // 黄色
    GRN = 0x00000002,       // 緑
    CYN = 0x00000003,       // シアン
    MAG = 0x00000004,       // マゼンタ
    RED = 0x00000005,       // 赤
    RST = 0x00000005,       // リセット
    MSK = 0x000000ff,       // 色ビット位置

    BLD = 0x00000100,       // 太字
    REV = 0x00000200,       // 反転

    // 太字
    BWHT = WHT | BLD,         // 白
    BYEL = YEL | BLD,         // 黄色
    BGRN = GRN | BLD,         // 緑
    BCYN = CYN | BLD,         // シアン
    BMAG = MAG | BLD,         // マゼンタ
    BRED = RED | BLD,         // 赤

    // 反転
    RWHT = WHT | REV,         // 白
    RYEL = YEL | REV,         // 黄色
    RGRN = GRN | REV,         // 緑
    RCYN = CYN | REV,         // シアン
    RMAG = MAG | REV,         // マゼンタ
    RRED = RED | BLD,         // 赤
};

// デバッグ用出力
class dbg
{
public:
    static void init(void);
	static void ln(col col, const char * fmt, ...);
	static void trace(const char* fmt, ...);
    static std::string escSeq(col col);
    static void dump(uint32_t addr, const void* mem, uint32_t size);
};

// コンソール出力
class cons
{
public:
    static void clr(void);             // クリア
    static void printf(const char* fmt, ...);
    static int getch(void);
    static int kbhit(void);
    static void getRowCol(int &row, int& col);
    static void setRowCol(int row, int col);
    static void attr(col coller);

protected:
    static int keyVal;

};
#endif // _DEBUG_OUT_H_
