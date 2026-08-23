// osSystemCall.cpp
// M5Stack の OS 依存部分をここに集約する。
// スレッド join はバイナリセマフォで行う(どのタスクから threadEnd を呼んでも安全)。
//
// 旧実装は「作成元タスクへ xTaskNotifyGive」する方式だったが、
// ネストしたスレッド生成(loopTask→startup→captureLoop)で作成元タスクが
// 先に自タスク削除されると、削除済みハンドルへ通知してヒープを破壊していた。
// セマフォを ThreadControl に持たせ、taskWrapper では ctrl を解放せず、
// threadEnd 側で待ち合わせてから解放することでこの問題を解消する。

#include "osSystemCall.h"
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <cstdio>
#include "dataManager.h"

namespace ossc
{
    struct ThreadControl {
        SemaphoreHandle_t             doneSem;   // タスク終了を通知するセマフォ
        void*                         userParm;
        std::function<errCode(void*)> userFunc;  // 値コピーで保持(呼び出し元のローカルが破棄されても安全)
        TaskHandle_t                  taskHandle;
    };

    // 内部ラッパー
    static void taskWrapper(void* arg) {
        ThreadControl* ctrl = (ThreadControl*)arg;

        if (ctrl->userFunc) {
            ctrl->userFunc(ctrl->userParm);
        }

        // 終了を通知する。ctrl の解放は threadEnd 側で行う(ここでは触らない)。
        xSemaphoreGive(ctrl->doneSem);
        vTaskDelete(NULL); // 自滅
    }

    // スレッドを起動する。
    // return : スレッドを破棄するためのハンドル(ThreadControl*)
    void* threadNet(THREAD_FUNC& func, void* parm, uint32_t stackBytes)
    {
        ThreadControl* ctrl = new ThreadControl();
        ctrl->userFunc = func;     // std::function を値コピー
        ctrl->userParm = parm;
        ctrl->doneSem  = xSemaphoreCreateBinary();
        ctrl->taskHandle = NULL;

        // タスクスタック(16KB)は内部RAM必須。2カメラ同時の再開/開始バーストでは、他セッションの
        // タスク生成やWiFi/BLE初期化と重なって内部の「連続領域」が一時的に不足し xTaskCreate が失敗する
        // ことがある(ヒープ総量は十分でも largest ブロックが足りない)。失敗を即諦めず、少し待って
        // 数回リトライする。先行タスクの初期化完了/一時確保の解放でほぼ通る(2本目の撮影runner不発を防ぐ)。
        BaseType_t created = pdFAIL;
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            created = xTaskCreatePinnedToCore(
                taskWrapper,
                "ossNet",
                stackBytes,     // 呼び出し側指定(既定14336)。撮影runnerはjson反復収束で~8.4KB使うため14KB確保。
                                // Arduino3.xでは内部の最大連続ブロックが約16KBに細切れ化し16384では2本目が入らない。
                ctrl,
                3,              // 優先度
                &ctrl->taskHandle,
                0
            );
            if (created == pdPASS) { break; }
            vTaskDelay(pdMS_TO_TICKS(200));   // 内部連続領域が空くのを待って再試行
        }

        // ★重要: それでも失敗するなら握りつぶさない。非nullハンドルを返すとタスクは走らないのに
        //   呼び出し側は成功と誤認し(例: 2本目の撮影runnerが起動せず establish不発=撮影開始せず)、
        //   原因が見えなくなる。失敗時は明示ログ＋nullptr返し。
        if (created != pdPASS)
        {
            // シリアルだけでなくログへも残す(2026-08-23)。無人運用ではシリアルを見ていないので、
            // 後から「失敗した瞬間にどれだけ空いていたか」を追えないと原因に届かない。
            // 空き総量(free)ではなく最大連続ブロック(largest)が足りないのが典型なので両方出す。
            {
                char d[96];
                std::snprintf(d, sizeof(d), "xTaskCreate failed stack=%u free=%u largest=%u",
                              (unsigned)stackBytes,
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                dataManager::logEvent("ERR", d, true);
            }
            Serial.printf("[thread] xTaskCreate failed r=%d free=%u largest=%u\n",
                          (int)created, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            vSemaphoreDelete(ctrl->doneSem);
            delete ctrl;
            return nullptr;
        }
        return (void*)ctrl; // ハンドルとして ThreadControl* を返す
    }

    // 終了を待ち合わせてスレッド資源を破棄する。
    // どのタスクから呼んでも安全。
    void threadEnd(void* handle)
    {
        if (!handle) { return; }
        ThreadControl* ctrl = (ThreadControl*)handle;

        // タスクが終了を通知するまでブロックして待機する
        xSemaphoreTake(ctrl->doneSem, portMAX_DELAY);

        vSemaphoreDelete(ctrl->doneSem);
        delete ctrl;
    }
}
