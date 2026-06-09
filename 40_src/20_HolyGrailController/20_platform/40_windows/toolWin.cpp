#include "commonWin.h"
#include "tool.h"

// 経過時間の計測を開始する
// return handle。これをgetElapse に渡して経過時間を取得する
void* tool::startElapse(void)
{
    return (void*)GetTickCount64();
}

// 経過時間を取得する
// handle は startElapse() で取得したハンドル
// return: startElapse() からの経過時間
uint32_t tool::getElapse(void* handle)
{
    return (uint32_t)GetTickCount64() - 
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(handle));
}

// 指定時間待つ
void tool::sleep(uint32_t ms)
{
    ::Sleep(ms);
}

void tool::memoryInfo(void)
{
    HANDLE heap = GetProcessHeap();
    PROCESS_HEAP_ENTRY entry;
    SIZE_T totalUsed = 0;
    entry.lpData = NULL;

    while (HeapWalk(heap, &entry)) {
        if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
            totalUsed += entry.cbData;
        }
    }
    DBGLN(col::YEL,"Approx used heap bytes: %zu\n", totalUsed);
}