// osSystemCall.cpp
// M5Stack の OS 依存部分をここに集約する

#include "osSystemCall.h"
#include <M5Unified.h>

namespace ossc
{
struct ThreadControl {
        TaskHandle_t callerHandle; // threadEndを呼び出す側のハンドル
        void* userParm;
        THREAD_FUNC* userFunc;
    };

    // 内部ラッパー
    void taskWrapper(void* arg) {
        ThreadControl* ctrl = (ThreadControl*)arg;

        if (ctrl->userFunc) {
            (*ctrl->userFunc)(ctrl->userParm);
        }

        // threadEndを呼んでいるスレッド（呼び出し元）に通知を送る
        // eNotifyAction::eIncrement (セマフォのようにカウントを増やす)
        xTaskNotifyGive(ctrl->callerHandle);

        DBGLN(col::YEL,"before delete.");
        delete ctrl; // 構造体を自ら破棄
        vTaskDelete(NULL); // 自滅    
        DBGLN(col::YEL,"Thread ended and notified caller.");
    }

    // スレッドを起動する
    void* threadNet(THREAD_FUNC& func, void* parm)
    {
        DBGLN(col::YEL,"entry threadEnd.");
        ThreadControl* ctrl = new ThreadControl();
        ctrl->userFunc = &func;
        ctrl->userParm = parm;
        // threadEndを呼び出す予定の「今のタスクハンドル」を記録しておく
        ctrl->callerHandle = xTaskGetCurrentTaskHandle();

        TaskHandle_t taskHandle = NULL;
        xTaskCreatePinnedToCore(
            taskWrapper,
            __func__,
            4096,
            ctrl,
            3,                  // 優先度
            &taskHandle,
            0 
        );

        DBGLN(col::YEL,"exit threadEnd.");
        return (void*)taskHandle; // タスクハンドルを返す
    }

    // 終了を待ち合わせる
    void threadEnd(void* handle)
    {
        if (!handle) return;

        // タスク側から通知（Notify）が来るまでブロックして待機する
        // pdTRUE: 待機終了時に通知値を0にリセットする
        // portMAX_DELAY: 通知が来るまで永遠に待つ
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        DBGLN(col::YEL,"threadEnd.");
        // 注: FreeRTOSのタスクハンドルは、タスクが消滅すると無効になるため
        // ここで delete する必要はありません（wrapper内で処理済み）。
    }
}
