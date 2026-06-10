#include "commonM5.h"
#include "tool.h"

// 経過時間の計測を開始する
// return handle。これをgetElapse に渡して経過時間を取得する
void* tool::startElapse(void)
{
    return (void *)millis();
}

// 経過時間を取得する
// handle は startElapse() で取得したハンドル
// return: startElapse() からの経過時間
uint32_t tool::getElapse(void * handle)
{
    return (uint32_t)millis() - (uint32_t)handle;
}

// 指定時間待つ。
// delay() は ESP32 Arduino では vTaskDelay を呼び FreeRTOS に yield する。
// (ビジーwaitにすると撮影ループの長い待機で IDLE タスクが飢餓し Task WDT が発火する)
void tool::sleep(uint32_t ms)
{
    delay(ms);
}
#include <esp_heap_caps.h>

void tool::memoryInfo(void) 
{
    size_t freeHeap = esp_get_free_heap_size();
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    size_t free8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t minFree = esp_get_minimum_free_heap_size();

    DBGLN(col::YEL, "Heap Fragmentation:");
    DBGLN(col::CYN,
        "use=%u(%u%%) total=%u free8=%u largest=%u max used=%u(%u%%)\n",
        totalHeap - freeHeap,
        ((totalHeap - freeHeap) * 100) / totalHeap,
        totalHeap, free8bit, largestBlock, 
        totalHeap -minFree,
        ((totalHeap - minFree) * 100) / totalHeap
    );
}