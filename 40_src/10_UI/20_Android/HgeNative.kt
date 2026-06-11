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
    external fun nativeSetListener(listener: HgeListener?)
    external fun nativeSearchDevices(): Int
    external fun nativeConnectManual(host: String): Int

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
