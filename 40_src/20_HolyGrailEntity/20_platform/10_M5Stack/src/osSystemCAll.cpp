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
    void* threadNet(THREAD_FUNC& func, void* parm, uint32_t stackBytes)
    {
        ThreadControl* ctrl = new ThreadControl();
        ctrl->userFunc = func;     // std::function を値コピー
        ctrl->userParm = parm;
        ctrl->doneSem  = xSemaphoreCreateBinary();
        ctrl->taskHandle = NULL;

        BaseType_t created = xTaskCreatePinnedToCore(
            taskWrapper,
            "ossNet",
            stackBytes,         // 呼び出し側指定(既定16384)。撮影runnerはjson反復収束で16KB必要
                                // (最初の補正§4.4は loop→initialConverge→alzMetering(json)と1段深くなり
                                // 8192ではcanary超過)。HTTP/UDP I/Oだけの軽量スレッドは小さくして内部DRAMを空ける。
            ctrl,
            3,                  // 優先度
            &ctrl->taskHandle,
            0
        );

        // ★重要: タスク生成失敗を握りつぶさない。ヒープ枯渇で xTaskCreate が失敗すると、以前は
        //   非nullハンドルを返すのにタスクは走らず(例: 2カメラ同時で2本目の撮影runnerが起動せず
        //   establish不発=撮影開始せず)、原因が見えなかった。失敗時は明示ログ＋nullptr返し。
        if (created != pdPASS)
        {
            Serial.printf("[THREADdiag] xTaskCreate FAILED r=%d freeHeap=%u minFree=%u\n",
                          (int)created, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
            vSemaphoreDelete(ctrl->doneSem);
            delete ctrl;
            return nullptr;
        }
        Serial.printf("[THREADdiag] task created freeHeap=%u\n", (unsigned)ESP.getFreeHeap());
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
