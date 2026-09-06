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
    const val EV_PRESENCE = 7    // スマホ常駐プレゼンスマップの変化 [{serial,model,assignedName,ip,online}]

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
    const val ERR_OVERLAP_LIMIT = 32 // 撮影期間が重なる自撮影計画が上限(MAX_CONCURRENT)を超える(§7.4)
    const val ERR_QUEUE_FULL = 33    // 撮影開始要求の受付上限(100件)超過(§7.4)
    const val ERR_SYNC_SHOT_LIMIT = 34 // 同期撮影の台数がこの端末の上限を超えた(上限値は端末が返す)

    external fun nativeSetLogDir(dir: String)
    external fun nativeInit(): Int
    external fun nativeTerm(): Int
    external fun nativeVersion(): String
    external fun nativeCaptureStart(): Int
    external fun nativeCaptureStop(): Int
    external fun nativeGetState(): Int
    external fun nativeResumeCapture(): Int   // 再起動時の撮影再開(item2)。再開した計画数を返す
    external fun nativePump(): Int             // 遅延アームのポンプ(§7.4)。予約計画の開始スレッドを期日に生成。数秒毎に呼ぶ
    // 並行撮影(計画id指定。空文字=編集対象)。通知に planId が付く。
    external fun nativeCaptureStartPlan(planId: String): Int
    external fun nativeCaptureStopPlan(planId: String): Int
    external fun nativeGetStatePlan(planId: String): Int
    external fun nativePokeAcquire(planId: String): Int   // 継続: スマホ直接撮影のNOCAMERA計画に即再探索を促す
    external fun nativeScheduleJson(): String
    external fun nativeGetPlanJson(): String
    // 指定 id の計画JSON。表示中の計画を切り替えずに取り出す(エッジ送信用)
    external fun nativeGetPlanJsonById(planId: String): String
    external fun nativeSetPlanJson(json: String): Int   // 撮影計画(cs)JSONを現在の編集計画へ復元(変更の取り消し用)
    // 撮影シミュレーション(画面360)。恒星リスト(fixed_star.json)を一度読み込む(戻り=星数)。
    external fun nativeSimLoadStars(starsJson: String): Int
    // params(datetime/offMin/lat/lon/alt/az/el/landscape/fisheye/focal/sensorW/sensorH/magLimit)から
    // 画角内の天体を投影した JSON {"objects":[{name,x,y,mag,color,kind}],"aspect",...} を返す。
    external fun nativeSimulateSky(paramsJson: String): String
    external fun nativeSetPlanTimes(start: String, end: String, offMin: Int): Int
    external fun nativeSetPlanDirection(azimuth: Double, elevation: Double): Int  // 撮影方向/仰角を設定し再生成
    external fun nativeSetPlanInterval(seconds: Double): Int   // 撮影周期。最小(最長ss+2)未満は失敗
    external fun nativeRenamePlan(id: String, name: String): Int  // 計画名をid指定で変更(リスト直接リネーム)
    external fun nativeSetPlanLandscape(landscape: Int): Int   // 横向き(ランドスケープ)。再生成
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
    external fun nativeGetStandardExpoValues(): String   // 初期値のエディタ用(カメラに依らない標準目盛り)
    external fun nativeSunAltitudeTimes(altitudeDeg: Int): String   // {"start":"MM/dd HH:mm","end":...}
    external fun nativeSetListener(listener: HgeListener?)

    // --- 機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6) ---
    external fun nativeGetMasterCameras(): String   // [{"camera":{...}},...]
    external fun nativeGetMasterLenses(): String     // [{...},...]
    external fun nativeGetOwnedCameras(): String
    external fun nativeGetOwnedLenses(): String
    // 所持カメラのダイジェスト認証パスワードを平文で取得(編集画面の表示用。JSON側は暗号文)
    external fun nativeOwnedCameraAuthPass(name: String): String

    // 機材マスタを読み直す(公開リポジトリから取り込んだ直後に呼ぶ)
    external fun nativeReloadMaster()
    external fun nativeAddOwnedCamera(name: String): Int
    // スマホ内蔵カメラを所持カメラへ足す(まだ無いものだけ)。戻り=足した台数。
    //  端末そのものなので登録可否は聞かない(外付けカメラのプロンプトとは扱いが違う)。
    external fun nativeRegisterBuiltinCameras(): Int
    // RAW 加算(2026-09-06)。Camera2 から受け取った RAW を足して現像する。ループは C++(rawStack)。
    external fun nativeRawStackBegin(width: Int, height: Int, cfa: Int)
    external fun nativeRawStackAdd(buf: java.nio.ByteBuffer, rowStride: Int): Boolean
    external fun nativeRawStackFrames(): Int
    external fun nativeRawStackDevelop(bitmap: android.graphics.Bitmap, whiteLevel: Int, black: FloatArray,
                                       gains: FloatArray, ccm: FloatArray, shading: FloatArray?,
                                       shadingCols: Int, shadingRows: Int): Boolean
    external fun nativeAddOwnedLens(name: String): Int
    external fun nativeRemoveOwnedCamera(name: String): Int
    external fun nativeRemoveOwnedLens(name: String): Int
    external fun nativeSetOwnedCameraAutoInsert(name: String, autoInsert: Int): Int
    external fun nativeSetPlanCamera(name: String): Int   // 所持→撮影計画へ反映し再生成
    // 同期撮影(2026-08-25)。主カメラで測光した露出を追加カメラへも配る。
    external fun nativeSetPlanSyncShot(on: Int): Int
    // 追加カメラを所持カメラの名前配列 ["name",...] で差し替える。
    external fun nativeSetPlanSubCameras(json: String): Int
    external fun nativeSetPlanLens(name: String): Int
    external fun nativeSetPlanLocation(lat: Double, lng: Double, name: String): Int  // 撮影場所(緯度経度)を直接設定し再生成
    // --- 撮影場所リスト(§7.9)。登録した場所を撮影計画で選択する ---
    external fun nativeGetPlaces(): String            // [{"name","memo","latitude","longitude","altitude","autoInsert"},...]
    // --- 撮影計画ひな形(2026-09-04 UI依頼)。計画と同じ形で、計画一覧には出ない ---
    external fun nativeListTemplates(): String                 // 名前順
    external fun nativeSelectTemplate(id: String): Int         // ひな形を編集対象にする
    external fun nativeSaveTemplateFromPlan(name: String): Int // 今の計画をひな形へ(空=計画名)
    external fun nativeCopyTemplate(id: String): Int
    external fun nativeDeleteTemplate(id: String): Int
    external fun nativeRenameTemplate(id: String, name: String): Int
    external fun nativeNewPlanFromTemplate(id: String): Int    // 開始日=今日 / 名前は連番回避
    external fun nativeUpdatePlanFromTemplate(planId: String, tplId: String): Int

    external fun nativeAddPlace(name: String): Int    // 空可(自動採番)
    external fun nativeRemovePlace(name: String): Int
    external fun nativeSetPlaceAutoInsert(name: String, autoInsert: Int): Int
    external fun nativeSetPlaceDetail(origName: String, json: String): Int  // name/memo/latitude/longitude/altitude/autoInsert
    external fun nativeSetPlanPlace(name: String): Int  // 登録済みの場所を撮影計画へ反映し再生成
    external fun nativeSetOwnedCameraDetail(origName: String, json: String): Int  // 620 詳細編集
    external fun nativeSetOwnedLensDetail(origName: String, json: String): Int    // 630 詳細編集
    external fun nativeSearchDevicesList(): String         // 接続カメラ検索: [{"model","assignedName","serial"},...]
    // スマホ常駐プレゼンスマップ(§3.2/§5.4)。start でオンラインカメラの常時把握を開始、変化は EV_PRESENCE。
    external fun nativePresenceStart(): Int
    external fun nativePresenceStop(): Int
    external fun nativePresenceJson(): String              // [{"serial","model","ip","online"}]
    external fun nativeAddOwnedDetected(index: Int): Int   // 検出カメラを所持へ追加
    // 発見/接続カメラ識別情報を所持へ反映。allowAdd=true:未一致は自動追加 / false:追加せず区分のみ返す(裏の発見→登録可否UI)。
    // 返り値: 0=既存にassignedName反映, 1=未定義枠へserial確定, 2=新規(allowAdd時は追加済/非allowAdd時は未追加), <0=エラー。
    external fun nativeRecordCameraIdentity(model: String, serial: String, assignedName: String, allowAdd: Boolean): Int
    external fun nativeGetColors(): String                 // システム共通の色 {"night":{"text","bg"},...}
    external fun nativeSetColors(json: String): Int
    external fun nativeGetSmoothing(): String              // 露出平滑化 {"hysteresis":double,"movingAverage":int}
    external fun nativeSetSmoothing(json: String): Int
    external fun nativePruneOldLogs(offMin: Int): Int     // 起動時ログ整理(当日以外5件以上で古い順に削除)
    // --- 撮影レポート(撮影1回=1件) ---
    external fun nativeReportList(): String               // [{"name","plan","camera","shotAt","frames","noteCount"},...] 新しい順
    external fun nativeReportJson(name: String): String   // 1件の中身(JSON)。空=読めない
    external fun nativeRemoveReport(name: String): Int
    // 撮影制御方法の初期値プリセット(型ごとに複数)
    external fun nativeGetCcmPresets(type: String): String  // [ccm,...]
    external fun nativeSetCcmPreset(type: String, origName: String, json: String): Int
    external fun nativeRemoveCcmPreset(type: String, name: String): Int
    external fun nativeGetPreferredCcm(type: String): String
    external fun nativeSetPreferredCcm(type: String, name: String): Int

    // --- エッジ端末(ETP §6) ---
    // 通信路の切替(スマホだけが決める。2026-08-14 指示)。true=BLE。
    // エッジは常に Wi-Fi と BLE の両方で待ち受けているので、切替をエッジへ知らせる必要は無い。
    // **BLE のときは以降の host 引数に IP ではなくエッジの端末名を渡すこと**
    // (BLE にはブロードキャストが無く、探索も接続も名前で行うため)。
    external fun nativeEdgeSetBle(useBle: Boolean)

    // ネイティブ(edgeClient.cpp)から呼び返される BLE の1往復。
    //  Android の BLE API は Kotlin にしかないので、ここで受けて EdgeBleLink へ渡す。
    //  ネイティブの作業スレッドから同期で呼ばれる(UIスレッドではない)。
    @JvmStatic
    fun bleExchange(target: String, frame: ByteArray, timeoutMs: Int): ByteArray? =
        EdgeBleLink.exchange(target, frame, timeoutMs)

    // ネイティブから呼び返す。応答の取り違えを見つけたときに BLE の線を畳む。
    //  畳まないと、まだ飛んでいる本来の応答が次の要求の窓へ落ちて連鎖する。
    @JvmStatic
    fun bleDrop(target: String) = EdgeBleLink.drop(target)

    // ── スマホ内蔵カメラ(2026-09-05) ─────────────────────────
    // Camera2 は Kotlin にしか無いので、Entity(apiBuiltin/detectBuiltin)から呼び返す。
    //  ここは素通しにして、判断は一切しない(BLE の bleExchange と同じ役目)。
    @JvmStatic
    fun builtinList(): String = BuiltinCamera.listCameras()

    // 論理カメラの配下にぶら下がっている物理カメラ(超広角・望遠など)を調べる。触らない。
    @JvmStatic
    fun builtinPhysicals(): String = BuiltinCamera.physicalsJson()

    // 物理カメラを名指しして撮れるかの実験(1枚だけ撮って撮影結果を読む)。
    @JvmStatic
    fun builtinProbePhysical(logicalId: String, physId: String): String =
        BuiltinCamera.probePhysical(logicalId, physId)

    @JvmStatic
    fun builtinDescribe(id: String): String = BuiltinCamera.describe(id)

    @JvmStatic
    fun builtinOpen(logicalId: String, physId: String, raw: Boolean): String =
        BuiltinCamera.open(logicalId, physId, raw)

    @JvmStatic
    fun builtinClose() = BuiltinCamera.close()

    // 露出を載せて1枚撮り始める(露光の終わりは待たない)。frames>1 は RAW を足して1枚にする。
    @JvmStatic
    fun builtinCapture(logicalId: String, physId: String, iso: Int, expNs: Long,
                       aperture: Double, timeoutMs: Int, frames: Int, raw: Boolean): Boolean =
        BuiltinCamera.capture(logicalId, physId, iso, expNs, aperture, timeoutMs, frames, raw)

    // 端末の熱の状態(PowerManager の THERMAL_STATUS_*)。-1=取れない端末。
    @JvmStatic
    fun builtinThermal(): Int = BuiltinCamera.thermalStatus()

    // 直前のコマを実際に撮った物理カメラ id(狙いどおりかの確認用)。
    @JvmStatic
    fun builtinActivePhysical(): String = BuiltinCamera.activePhysicalId()

    // 直前に撮り始めた1枚を受け取る(まだ露光中なら待つ)。
    //  【2026-09-06 に一度消えていた】試し撮りの入口を外したとき、隣のこの2つまで一緒に消し、
    //  一晩の撮影で1枚も受け取れなかった(現像は毎コマ成功していたのに保存 0 枚・測光 stage=1)。
    @JvmStatic
    fun builtinTakeImage(timeoutMs: Int): ByteArray? = BuiltinCamera.takeImage(timeoutMs)

    // ── 動画の書き出し(2026-09-05) ───────────────────────────
    // 撮ったコマをその場で1枚ずつ足していく。撮影の終わりに必ず finish を呼ぶこと
    //  (MP4 は閉じないと再生できない)。
    // 戻り=ギャラリーでの名前(hgt_yymmddhhmmss.mp4)。"" =失敗。
    @JvmStatic
    fun videoStart(fps: Int, planName: String): String { BuiltinVideo.setPlanName(planName); return BuiltinVideo.start(fps) }

    @JvmStatic
    fun videoAddJpeg(jpeg: ByteArray?): Boolean = BuiltinVideo.addJpeg(jpeg)

    @JvmStatic
    fun videoFinish(): String = BuiltinVideo.finish()

    // ネイティブから呼び返される BLE のエッジ探索。見つかった端末名(HGC- を除く)を返す。
    //  BLE には UDP ブロードキャストが無いので、検索はアドバタイズのスキャンで代える。
    @JvmStatic
    fun bleScanNames(timeoutMs: Int): Array<String> =
        EdgeBleLink.scanEdgeNames(timeoutMs.toLong()).toTypedArray()

    external fun nativeEdgeSearch(timeoutMs: Int): String           // edgeInfo の JSON 配列
    // 【時刻は UTC のエポック秒で渡す(2026-09-03)】書式器(SimpleDateFormat)は生成時のタイムゾーンを
    //  抱えるため、現地でTZを変えた直後に「文字列は旧TZ・オフセットは新TZ」となり、エッジの時計が
    //  時差ぶんずれた(2026-09-02 実害)。整数なら混ざりようがない。offMin はエッジの表示用。
    external fun nativeEdgeStart(host: String, port: Int, utcSec: Long, offMin: Int, nameBmp: ByteArray, planId: String, planJson: String): Int
    // 直近のエッジ操作でエッジが返した「お知らせコード」(0=なし)。文言はUIが持つ。
    external fun nativeLastEdgeNotice(): Int
    // お知らせの付随数値(例: エッジが扱えるカメラ台数)
    external fun nativeLastEdgeNoticeN1(): Int
    // 直前の撮影開始(スマホ直結)が失敗した理由の付随数値(台数など)
    external fun nativeLastStartNoticeN1(): Int
    // 直前の撮影開始(スマホ直結)が失敗した理由コード(hgc::notice)。0=理由なし
    external fun nativeLastStartNotice(): Int
    external fun nativeEdgeStop(host: String, port: Int, planId: String): Int
    external fun nativeEdgeDeletePlan(host: String, port: Int, planId: String): Int   // 項目6: エッジから計画を削除(撮影中は停止してから)
    external fun nativeEdgeSyncTime(host: String, port: Int, utcSec: Long, offMin: Int): Int // 能動的な時刻同期(C_TIMEのみ)
    // 所持カメラ台帳をエッジへ渡す(C_CAMERA_BOOK)。送った内容がその時点の全量で、
    //  エッジは持っている台帳を捨てて入れ替える(追加/変更/削除がこれ1本で伝わる)。
    external fun nativeEdgeSendCameraBook(host: String, port: Int, book: String): Int
    // 送る台帳の中身。パスワードは暗号文。中身が変わったかの判定にもこの文字列を使う。
    external fun nativeCameraBookJson(): String
    // 台帳の中身の指紋。変化の判定はこちら(JSON そのものは暗号文の nonce で毎回変わる)。
    external fun nativeCameraBookSig(): String
    // デバッグログの取捨。撮影1コマごとの記録(SHOT/LVHIST)と電池の定期記録を採るかどうか。
    //  既定は両方とも採らない(量が多く、肝心の出来事が埋もれるため)。
    external fun nativeSetLogOptions(shot: Boolean)
    // 同じ設定をエッジへ送る。エッジは不揮発へ残さないので見つけるたびに送り直す。
    external fun nativeEdgeSendLogOpt(host: String, port: Int, shot: Boolean, batt: Boolean, sys: Boolean): Int
    external fun nativeEdgeResearch(host: String, port: Int, planId: String): Int // 継続: エッジへ即再探索を送る
    // 【2026-08-06 送信廃止】カメラIPのエッジへの通知はやめた(エッジが自分で見つける。複数AP構成では
    //  別の場所のカメラのIPを配ることになり有害)。ETPのコマンドとエッジ側の受信処理は互換のため残す。
    external fun nativeEdgeCameraInfo(host: String, port: Int, json: String): Int
    external fun nativeEdgeProgress(host: String, port: Int, planId: String): String // progress の JSON。planId 指定=計画別状態(空=集約)
    external fun nativeEdgeLogList(host: String, port: Int): String   // ログファイル名一覧の JSON 配列 ["hg_....log",...]
    external fun nativeEdgeLogRead(host: String, port: Int, name: String, offset: Int): ByteArray // ログの1チャンク(最大4KB)。空=EOF/失敗
    // 撮影レポートの回収。30秒スイープが edgeInfo.reports>0 のときだけ使う。
    // 取得→保存できたら削除、の順(取得だけで消さない)。
    external fun nativeEdgeReportList(host: String, port: Int): String            // [{"name",...},...]。失敗="[]"
    external fun nativeEdgeReportRead(host: String, port: Int, name: String): String // 1件のJSON本文。失敗=""
    external fun nativeEdgeReportDelete(host: String, port: Int, name: String): Int  // 0=削除済み

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
