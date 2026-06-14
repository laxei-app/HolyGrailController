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
    void* threadNet(THREAD_FUNC& func, void* parm)
    {
        ThreadControl* ctrl = new ThreadControl();
        ctrl->userFunc = func;     // std::function を値コピー
        ctrl->userParm = parm;
        ctrl->doneSem  = xSemaphoreCreateBinary();
        ctrl->taskHandle = NULL;

        xTaskCreatePinnedToCore(
            taskWrapper,
            "ossNet",
            16384,              // json/HTTPパース・撮影ループ用(4096では不足)。
                                // 最初の補正(§4.4)の反復収束は loop→initialConverge→alzMetering(json)と
                                // 1段深くなるため 8192 ではスタック超過(canary)した。余裕を持って 16384。
            ctrl,
            3,                  // 優先度
            &ctrl->taskHandle,
            0
        );

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
