package app.laxei.holygrail

// holyGrailEntity(extern "C") への JNI ブリッジ窓口。
// ネイティブ実装は 20_platform/20_Android/src/jniBridge.cpp。

interface HgeListener {
    // Entity からの通知。ワーカースレッドから呼ばれるため UI 更新は post すること。
    fun onHgeEvent(event: Int, json: String)
}

object HgeNative {
    init {
        System.loadLibrary("holygrail")
    }

    // 通知イベント種別 (モジュール構造仕様書 47 §2.3)
    const val EV_STATE = 1
    const val EV_PROGRESS = 2
    const val EV_CAPTURED = 3
    const val EV_DEVICE = 4
    const val EV_SCHEDULE = 5
    const val EV_ERROR = 6

    // 撮影状態 (47 §2.3)
    const val ST_IDLE = 0
    const val ST_SEARCHING = 1
    const val ST_READY = 2
    const val ST_CAPTURING = 3
    const val ST_STOPPING = 4
    const val ST_ERROR = 5

    external fun nativeSetLogDir(dir: String)
    external fun nativeInit(): Int
    external fun nativeTerm(): Int
    external fun nativeVersion(): String
    external fun nativeCaptureStart(): Int
    external fun nativeCaptureStop(): Int
    external fun nativeGetState(): Int
    external fun nativeScheduleJson(): String
    external fun nativeGetPlanJson(): String
    external fun nativeSetPlanTimes(start: String, end: String, offMin: Int): Int
    external fun nativeSetPlanDirection(azimuth: Double, elevation: Double): Int  // 撮影方向/仰角を設定し再生成
    external fun nativeSavePlan(): Int
    external fun nativeGetCcmDefaults(): String
    external fun nativeSetCcmDefaults(json: String): Int
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
    external fun nativeAddOwnedDetected(index: Int): Int   // 検出カメラを所持へ追加
    external fun nativeGetColors(): String                 // システム共通の色 {"night":{"text","bg"},...}
    external fun nativeSetColors(json: String): Int
    // 撮影制御方法の初期値プリセット(型ごとに複数)
    external fun nativeGetCcmPresets(type: String): String  // [ccm,...]
    external fun nativeSetCcmPreset(type: String, origName: String, json: String): Int
    external fun nativeRemoveCcmPreset(type: String, name: String): Int
    external fun nativeGetPreferredCcm(type: String): String
    external fun nativeSetPreferredCcm(type: String, name: String): Int

    // --- エッジ端末(ETP §6) ---
    external fun nativeEdgeSearch(timeoutMs: Int): String           // edgeInfo の JSON 配列
    external fun nativeEdgeStart(host: String, port: Int, datetime: String, offMin: Int): Int
    external fun nativeEdgeStop(host: String, port: Int): Int
    external fun nativeEdgeProgress(host: String, port: Int): String // progress の JSON

    fun stateName(s: Int): String = when (s) {
        ST_IDLE -> "IDLE"
        ST_SEARCHING -> "SEARCHING"
        ST_READY -> "READY"
        ST_CAPTURING -> "CAPTURING"
        ST_STOPPING -> "STOPPING"
        ST_ERROR -> "ERROR"
        else -> "?($s)"
    }
}
