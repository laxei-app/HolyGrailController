package app.laxei.holygrail

// holyGrailEntity(extern "C") への JNI ブリッジ窓口。
// ネイティブ実装は 20_platform/20_Android/src/jniBridge.cpp。

interface HgeListener {
    // Entity からの通知。ワーカースレッドから呼ばれるため UI 更新は post すること。
    fun onHgeEvent(event: Int, json: String)
}

object HgeNative {
    init {
        System.loadLibrary("HolyGrailEntity")
    }

    // 通知イベント種別 (モジュール構造仕様書 47 §2.3)
    const val EV_STATE = 1
    const val EV_PROGRESS = 2
    const val EV_CAPTURED = 3
    const val EV_DEVICE = 4
    const val EV_SCHEDULE = 5
    const val EV_ERROR = 6
    const val EV_PRESENCE = 7    // スマホ常駐プレゼンスマップの変化 [{serial,model,ip,online}]

    // 撮影状態 (47 §2.3)
    const val ST_IDLE = 0
    const val ST_SEARCHING = 1
    const val ST_READY = 2
    const val ST_CAPTURING = 3
    const val ST_STOPPING = 4
    const val ST_ERROR = 5
    const val ST_DISCONNECTED = 6   // 撮影中にカメラ接続が切れた(NOCAMERAの旧同義)。✖点灯
    const val ST_WAITING = 7        // 撮影要求済・撮影窓前・カメラOKで待機中。カメラアイコン点灯(点滅しない)
    const val ST_NOCAMERA = 8       // 武装/撮影中いずれでもカメラ未検出。✖カメラアイコン点灯
    const val ERR_NAME_DUP = 31     // 名称が既に使用されている(errCode の ERR_HGC_NAME_DUP。item5)

    external fun nativeSetLogDir(dir: String)
    external fun nativeInit(): Int
    external fun nativeTerm(): Int
    external fun nativeVersion(): String
    external fun nativeCaptureStart(): Int
    external fun nativeCaptureStop(): Int
    external fun nativeGetState(): Int
    external fun nativeResumeCapture(): Int   // 再起動時の撮影再開(item2)。再開した計画数を返す
    // 並行撮影(計画id指定。空文字=編集対象)。通知に planId が付く。
    external fun nativeCaptureStartPlan(planId: String): Int
    external fun nativeCaptureStopPlan(planId: String): Int
    external fun nativeGetStatePlan(planId: String): Int
    external fun nativePokeAcquire(planId: String): Int   // 継続: スマホ直接撮影のNOCAMERA計画に即再探索を促す
    external fun nativeScheduleJson(): String
    external fun nativeGetPlanJson(): String
    external fun nativeSetPlanTimes(start: String, end: String, offMin: Int): Int
    external fun nativeSetPlanDirection(azimuth: Double, elevation: Double): Int  // 撮影方向/仰角を設定し再生成
    external fun nativeSetPlanInterval(seconds: Double): Int   // 撮影周期。最小(最長ss+2)未満は失敗
    external fun nativeRenamePlan(id: String, name: String): Int  // 計画名をid指定で変更(リスト直接リネーム)
    external fun nativeSetPlanLandscape(landscape: Int): Int   // 横向き(ランドスケープ)。再生成
    external fun nativeSetPlanGearConst(json: String): Int     // センサー/レンズ定数の上書き(sensorW/H/pixelW/focalLength/fn)
    // スケジュール手動編集(7.3.2)
    external fun nativeSetBandMode(sunriseMode: Int, sunsetMode: Int): Int  // 0=自動,1=挿入,2=排除
    external fun nativeSetBoundary(beforeType: Int, afterType: Int, occ: Int, whenIso: String): Int
    external fun nativeSetBoundaryByAlt(beforeType: Int, afterType: Int, occ: Int, altDeg: Double, rising: Int): Int
    external fun nativeClearScheduleEdits(): Int
    external fun nativeSavePlan(): Int
    // --- 複数撮影計画(§7.4) ---
    external fun nativeListPlans(): String           // [{"id","name","start","end","capturable","state"},...]
    external fun nativeNewPlan(presetName: String): Int   // 新規作成→編集対象に切替(EV_SCHEDULE通知)
    external fun nativeCopyPlan(id: String): Int          // 複製→編集対象に切替
    external fun nativeDeletePlan(id: String): Int
    external fun nativeSelectPlan(id: String): Int        // 編集対象に切替(EV_SCHEDULE通知)
    external fun nativeCurrentPlanId(): String            // 現在(編集対象)の計画 id
    external fun nativeGetCcmDefaults(): String      // 参照専用の初期値(コード上の出荷時設定)
    external fun nativeGetPlanCcm(): String          // 計画固有ccm(初期値とは別)
    external fun nativeSetPlanCcm(json: String): Int
    external fun nativeGetExpoValues(): String
    external fun nativeSunAltitudeTimes(altitudeDeg: Int): String   // {"start":"MM/dd HH:mm","end":...}
    external fun nativeSetListener(listener: HgeListener?)
    external fun nativeSearchDevices(): Int
    external fun nativeConnectManual(host: String): Int

    // --- 機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6) ---
    external fun nativeGetMasterCameras(): String   // [{"camera":{...}},...]
    external fun nativeGetMasterLenses(): String     // [{...},...]
    external fun nativeGetOwnedCameras(): String
    external fun nativeGetOwnedLenses(): String
    external fun nativeAddOwnedCamera(name: String): Int
    external fun nativeAddOwnedLens(name: String): Int
    external fun nativeRemoveOwnedCamera(name: String): Int
    external fun nativeRemoveOwnedLens(name: String): Int
    external fun nativeSetOwnedCameraAutoInsert(name: String, autoInsert: Int): Int
    external fun nativeSetPlanCamera(name: String): Int   // 所持→撮影計画へ反映し再生成
    external fun nativeSetPlanLens(name: String): Int
    external fun nativeSetOwnedCameraDetail(origName: String, json: String): Int  // 620 詳細編集
    external fun nativeSetOwnedLensDetail(origName: String, json: String): Int    // 630 詳細編集
    external fun nativeSearchDevicesList(): String         // 接続カメラ検索: [{"model","friendly","serial"},...]
    // スマホ常駐プレゼンスマップ(§3.2/§5.4)。start でオンラインカメラの常時把握を開始、変化は EV_PRESENCE。
    external fun nativePresenceStart(): Int
    external fun nativePresenceStop(): Int
    external fun nativePresenceJson(): String              // [{"serial","model","ip","online"}]
    external fun nativeAddOwnedDetected(index: Int): Int   // 検出カメラを所持へ追加
    external fun nativeGetColors(): String                 // システム共通の色 {"night":{"text","bg"},...}
    external fun nativeSetColors(json: String): Int
    external fun nativeGetSmoothing(): String              // 露出平滑化 {"hysteresis":double,"movingAverage":int}
    external fun nativeSetSmoothing(json: String): Int
    external fun nativePruneOldLogs(offMin: Int): Int     // 起動時ログ整理(当日以外5件以上で古い順に削除)
    // 撮影制御方法の初期値プリセット(型ごとに複数)
    external fun nativeGetCcmPresets(type: String): String  // [ccm,...]
    external fun nativeSetCcmPreset(type: String, origName: String, json: String): Int
    external fun nativeRemoveCcmPreset(type: String, name: String): Int
    external fun nativeGetPreferredCcm(type: String): String
    external fun nativeSetPreferredCcm(type: String, name: String): Int

    // --- エッジ端末(ETP §6) ---
    external fun nativeEdgeSearch(timeoutMs: Int): String           // edgeInfo の JSON 配列
    external fun nativeEdgeStart(host: String, port: Int, datetime: String, offMin: Int, nameBmp: ByteArray, planId: String): Int
    external fun nativeEdgeStop(host: String, port: Int, planId: String): Int
    external fun nativeEdgeResearch(host: String, port: Int, planId: String): Int // 継続: エッジへ即再探索を送る
    external fun nativeEdgeCameraInfo(host: String, port: Int, json: String): Int // 発見中オンラインカメラ[{serial,model,ip,online}]をエッジへ通知(IP直結ヒント)
    external fun nativeEdgeProgress(host: String, port: Int): String // progress の JSON

    fun stateName(s: Int): String = when (s) {
        ST_IDLE -> "IDLE"
        ST_SEARCHING -> "SEARCHING"
        ST_READY -> "READY"
        ST_CAPTURING -> "CAPTURING"
        ST_STOPPING -> "STOPPING"
        ST_ERROR -> "ERROR"
        ST_DISCONNECTED -> "DISCONNECTED"
        ST_WAITING -> "WAITING"
        ST_NOCAMERA -> "NOCAMERA"
        else -> "?($s)"
    }
}
