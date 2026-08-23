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
#include <mutex>
#include <vector>
#include <algorithm>

namespace ossc
{
    struct ThreadControl {
        SemaphoreHandle_t             doneSem;   // タスク終了を通知するセマフォ
        void*                         userParm;
        std::function<errCode(void*)> userFunc;  // 値コピーで保持(呼び出し元のローカルが破棄されても安全)
        TaskHandle_t                  taskHandle;
        int                           slot = -1; // 静的スタックプールの番号(-1=ヒープから確保)
        uint32_t                      stackBytes = 0;  // 確保量(高水位ログで使う)
    };

    // --- 撮影セッション用スタックの静的確保(2026-08-23) ---
    //
    // 【なぜ静的にするか】開始→中止を繰り返すと、内部DRAMの**最大連続ブロック**だけが
    //  削られていき、3回目の開始で xTaskCreate が失敗する(2026-08-23 実機で確定)。
    //    中止後の largest: 27636 -> 18420 -> 14324
    //    失敗時: xTaskCreate failed stack=14336 free=80540 largest=14324  ← **12バイト足りない**
    //  空き総量(free)は毎回 92KB へ戻っており漏れてはいない。断片化だけが進む。
    //  一度 largest が要求を下回ると固定され、何度リトライしても永久に通らない
    //  (再起動しか復旧手段が無い)。スタックを縮めても延命にしかならないので、
    //  確保先をヒープから外す。.bss ならリンク時に場所が決まるので断片化の影響を受けない。
    //
    // 【使う範囲】撮影セッションの起動スレッド(既定 14336)だけ。netThread ワーカー(6144)や
    //  SSDP待ち受け(4096)は小さく、断片化で失敗していないので従来どおりヒープから取る。
    // 【枚数】同時に撮れるカメラ台数(holyGrailEntity.cpp の MAX_CONCURRENT に合わせる)。埋まっていたら従来どおり
    //  ヒープへフォールバックする(一斉開始などで一時的に3本要る場面を潰さない)。
    constexpr uint32_t kPoolStackBytes = 12288;
    constexpr int      kPoolSlots      = 3;
    static StackType_t  s_poolStack[kPoolSlots][kPoolStackBytes / sizeof(StackType_t)];
    static StaticTask_t s_poolTcb[kPoolSlots];
    static bool         s_poolUsed[kPoolSlots] = { false };
    static std::mutex   s_poolMtx;

    // --- 生きているスレッドの台帳(2026-08-23) ---
    // 常駐スレッド(在否監視等)は終了しないので taskWrapper の終了ログが出ない。
    // 内部RAMを削るには「今生きているスレッドがどれだけ使っているか」が要るので、
    // ハンドルを控えて外から高水位を読めるようにする。
    static std::vector<ThreadControl*> s_live;
    static std::mutex                  s_liveMtx;

    static int poolAcquire(void)
    {
        std::lock_guard<std::mutex> lk(s_poolMtx);
        for (int i = 0; i < kPoolSlots; ++i) { if (!s_poolUsed[i]) { s_poolUsed[i] = true; return i; } }
        return -1;
    }
    static void liveAdd(ThreadControl* c)
    {
        std::lock_guard<std::mutex> lk(s_liveMtx);
        s_live.push_back(c);
    }
    static void liveRemove(ThreadControl* c)
    {
        std::lock_guard<std::mutex> lk(s_liveMtx);
        s_live.erase(std::remove(s_live.begin(), s_live.end(), c), s_live.end());
    }

    static void poolRelease(int slot)
    {
        if (slot < 0) { return; }
        std::lock_guard<std::mutex> lk(s_poolMtx);
        s_poolUsed[slot] = false;
    }

    // 内部ラッパー
    static void taskWrapper(void* arg) {
        ThreadControl* ctrl = (ThreadControl*)arg;

        if (ctrl->userFunc) {
            ctrl->userFunc(ctrl->userParm);
        }

        // スタックの高水位(一度でも残りがここまで減った、という最小値)を残す(2026-08-23)。
        //  14336 を詰められるか、枚数を増やせるかの判断材料にする。
        //  ESP-IDF の uxTaskGetStackHighWaterMark は**バイト**を返す(本家 FreeRTOS はワード)。
        {
            const unsigned left = (unsigned)uxTaskGetStackHighWaterMark(NULL);
            char d[96];
            std::snprintf(d, sizeof(d), "task end size=%u leftMin=%u used=%u",
                          (unsigned)ctrl->stackBytes, left,
                          (ctrl->stackBytes > left) ? (unsigned)(ctrl->stackBytes - left) : 0u);
            dataManager::logEvent("STACK", d);
        }

        // 終了を通知する。ctrl の解放は threadEnd 側で行う(ここでは触らない)。
        xSemaphoreGive(ctrl->doneSem);
        vTaskDelete(NULL); // 自滅
    }

    // スレッドを起動する。
    // return : スレッドを破棄するためのハンドル(ThreadControl*)
    void* threadNet(THREAD_FUNC& func, void* parm, uint32_t stackBytes, bool useStaticPool)
    {
        ThreadControl* ctrl = new ThreadControl();
        ctrl->userFunc = func;     // std::function を値コピー
        ctrl->userParm = parm;
        ctrl->doneSem  = xSemaphoreCreateBinary();
        ctrl->taskHandle = NULL;
        ctrl->stackBytes = stackBytes;

        // タスクスタック(16KB)は内部RAM必須。2カメラ同時の再開/開始バーストでは、他セッションの
        // タスク生成やWiFi/BLE初期化と重なって内部の「連続領域」が一時的に不足し xTaskCreate が失敗する
        // ことがある(ヒープ総量は十分でも largest ブロックが足りない)。失敗を即諦めず、少し待って
        // 数回リトライする。先行タスクの初期化完了/一時確保の解放でほぼ通る(2本目の撮影runner不発を防ぐ)。
        BaseType_t created = pdFAIL;
        // ① 静的スタックの空きがあればそちらで作る。ここは断片化に左右されない。
        ctrl->slot = useStaticPool ? poolAcquire() : -1;
        if (ctrl->slot >= 0)
        {
            ctrl->taskHandle = xTaskCreateStaticPinnedToCore(
                taskWrapper, "ossNet", kPoolStackBytes / sizeof(StackType_t), ctrl, 3,
                s_poolStack[ctrl->slot], &s_poolTcb[ctrl->slot], 0);
            if (ctrl->taskHandle != nullptr) { liveAdd(ctrl); return (void*)ctrl; }
            poolRelease(ctrl->slot); ctrl->slot = -1;   // 引数不正等(通常起きない)。ヒープへ逃げる
        }
        // ② プールが埋まっている/対象外のサイズ → 従来どおりヒープから確保する。
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
        liveAdd(ctrl);
        return (void*)ctrl; // ハンドルとして ThreadControl* を返す
    }

    // 生きているスレッドの確保量と高水位をログへ出す。スタックを削る判断材料。
    void logLiveThreads(void)
    {
        std::lock_guard<std::mutex> lk(s_liveMtx);
        for (ThreadControl* c : s_live)
        {
            if (c == nullptr || c->taskHandle == nullptr) { continue; }
            const unsigned left = (unsigned)uxTaskGetStackHighWaterMark(c->taskHandle);
            char d[96];
            std::snprintf(d, sizeof(d), "live size=%u leftMin=%u used=%u pool=%d",
                          (unsigned)c->stackBytes, left,
                          (c->stackBytes > left) ? (unsigned)(c->stackBytes - left) : 0u, c->slot);
            dataManager::logEvent("STACK", d);
        }
    }

    // 終了を待ち合わせてスレッド資源を破棄する。
    // どのタスクから呼んでも安全。
    void threadEnd(void* handle)
    {
        if (!handle) { return; }
        ThreadControl* ctrl = (ThreadControl*)handle;

        // タスクが終了を通知するまでブロックして待機する
        xSemaphoreTake(ctrl->doneSem, portMAX_DELAY);

        // 静的スタックは「本当に消えてから」返す。
        //  taskWrapper は xSemaphoreGive の**後**で vTaskDelete(NULL) するので、この時点では
        //  まだ自分のスタックの上で動いている。ヒープ確保なら FreeRTOS が後始末するが、
        //  静的だと即再利用したときに「まだ使われているスタック」を上書きする。
        //  削除の仕上げはアイドルタスクがやるので、譲りながら eDeleted を待つ。
        //  (ヒープ確保のハンドルは TCB ごと解放されるため、同じことをしてはいけない)
        if (ctrl->slot >= 0)
        {
            for (int i = 0; i < 200 && eTaskGetState(ctrl->taskHandle) != eDeleted; ++i)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            poolRelease(ctrl->slot);
        }

        liveRemove(ctrl);
        vSemaphoreDelete(ctrl->doneSem);
        delete ctrl;
    }
}
