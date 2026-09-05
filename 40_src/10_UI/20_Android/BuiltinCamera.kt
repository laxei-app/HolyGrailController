package app.laxei.holygrail

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.hardware.camera2.CaptureResult
import android.hardware.camera2.TotalCaptureResult
import android.graphics.Bitmap
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.util.Size
import java.io.ByteArrayOutputStream
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

// スマホ内蔵カメラを「1台のカメラ」として扱うための Camera2 実装(2026-09-05)。
//
// 【なぜ Kotlin に置くか】Camera2 は Java/Kotlin にしか無い。Entity(C++)からは
//  HgeNative の静的メソッド経由で呼び返す(BLE の edgeClient.cpp と同じ形)。
//  この画層は**カメラを開いて撮って JPEG を返すだけ**にし、露出の決め方や測光の解釈は
//  すべて C++ 側(apiBuiltin)に置く。キヤノン機との違いを1か所に集めるため。
//
// 【対象は物理カメラだけ(2026-09-05 ユーザー判断)】論理カメラ(複数の物理カメラを束ねた
//  もの)は出さない。広角・超広角・望遠がそれぞれ別のカメラとして一覧に並ぶ。
//
// 【機種に依存しないこと】Pixel 6 で実装・検証するが、CameraCharacteristics は端末に
//  よって欠ける項目がある。**取れなかったものは空/0 のままにして落とさない**。
object BuiltinCamera {

    private var appCtx: Context? = null
    fun init(ctx: Context) { appCtx = ctx.applicationContext }

    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    // 【星を消す工程を切る(2026-09-05)】暗い星はノイズリダクションに「ノイズ」と見なされて
    //  消される。輪郭強調も点光源を不自然にする。どちらも切れる端末では切る。
    //  切れるかは端末が答える。**対応していない値を要求すると撮影要求ごと弾かれる**ので、
    //  開くときに確かめて覚えておき、使えるときだけ載せる。
    private var canNrOff = false
    private var canEdgeOff = false

    private var openId: String? = null      // いま開いている物理カメラ id
    private var openLogical: String? = null // その入口になっている論理カメラ id
    // 直前のコマを実際に撮った物理カメラ id(端末の申告)。狙いどおりか確かめるために持つ。
    @Volatile private var activePhys: String = ""
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null

    // ── RAW 加算(2026-09-06) ─────────────────────────────────
    // 【なぜ RAW か】1コマの露光には上限がある(Pixel 6 広角 8.3秒)。星空には 20〜48 秒が欲しいので、
    //  上限以下のコマを続けて撮って**線形の画素値で足す**。JPEG は階調カーブ済みの 8bit で暗部が
    //  潰れているので足せない。足すのも現像も C++(rawStack)で行い、ここは受け渡しだけ。
    //  1コマで足りる露光でも同じ道を通す(コマ数で色や階調が変わると動画に段差が出る)。
    //  RAW を出せない端末(または Bayer でない端末)は従来の JPEG に落ちる(足せないので上限はセンサーのまま)。
    private var openRaw = false          // open に頼まれた形(RAW を望んだか)
    private var useRaw = false           // 実際に RAW で動いているか
    private var rawW = 0; private var rawH = 0
    private var cfa = 0                  // Bayer の並び(SENSOR_INFO_COLOR_FILTER_ARRANGEMENT)
    private var whiteLevel = 1023
    private var blackPos = floatArrayOf(64f, 64f, 64f, 64f)   // 黒レベル。位置順(左上,右上,左下,右下)
    private var canShadingMap = false    // 周辺減光の地図を撮影結果に付けられるか
    @Volatile var lastStackMs = 0        // 直前の現像にかかった時間[ms](実測用)

    private fun rawSupported(c: CameraCharacteristics): Boolean {
        val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: return false
        if (!caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW)) return false
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        if (map.getOutputSizes(ImageFormat.RAW_SENSOR).isNullOrEmpty()) return false
        val f = c.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT) ?: return false
        return f in 0..3   // Bayer 以外(モノクロ/近赤外)は足し方が違うので JPEG へ
    }

    // 端末の熱の状態。PowerManager の THERMAL_STATUS_*(0=平常 … 6=停止直前)。取れない端末は -1。
    @JvmStatic
    fun thermalStatus(): Int {
        if (Build.VERSION.SDK_INT < 29) return -1
        val pm = appCtx?.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return -1
        return runCatching { pm.currentThermalStatus }.getOrDefault(-1)
    }

    private fun mgr(): CameraManager? =
        appCtx?.getSystemService(Context.CAMERA_SERVICE) as? CameraManager

    private fun ensureThread(): Handler {
        var h = handler
        if (h == null) {
            val t = HandlerThread("builtin-cam").apply { start() }
            h = Handler(t.looper)
            thread = t; handler = h
        }
        return h
    }

    // ── 列挙 ────────────────────────────────────────────────
    // 【背面の物理カメラだけを並べる(2026-09-05 ユーザー指示)】
    //  ・前面カメラは星景に使えないので出さない
    //  ・超広角などは論理カメラ(複数を束ねたもの)の配下に隠れていて getCameraIdList に
    //    出てこない。配下まで辿って**物理カメラを1台ずつ**出す
    //  ・物理カメラは単体で開けないことが多いが、論理カメラを入口にして名指しすれば使える
    //    (2026-09-05 実機で確認。狙ったセンサーで撮れ、露出も指定どおり乗る)
    //  配下を持たない端末では、その論理カメラ自身を1台として扱う。
    @JvmStatic
    fun listCameras(): String {
        val m = mgr() ?: return "[]"
        val arr = JSONArray()
        runCatching {
            for (id in m.cameraIdList) {
                val c = runCatching { m.getCameraCharacteristics(id) }.getOrNull() ?: continue
                val subs = if (Build.VERSION.SDK_INT >= 28)
                    (runCatching { c.physicalCameraIds }.getOrNull() ?: emptySet()) else emptySet()
                if (subs.isEmpty()) {
                    if (facingName(c) != "back") { continue }
                    arr.put(JSONObject().put("id", id).put("logical", id)
                                        .put("name", displayName(id, c)).put("facing", "back"))
                    continue
                }
                for (sub in subs) {
                    val pc = runCatching { m.getCameraCharacteristics(sub) }.getOrNull() ?: continue
                    if (facingName(pc) != "back") { continue }
                    arr.put(JSONObject().put("id", sub).put("logical", id)
                                        .put("name", displayName(sub, pc)).put("facing", "back"))
                }
            }
        }
        return arr.toString()
    }

    // 【束ねられているカメラを調べる(2026-09-05)】getCameraIdList に出てくるのは、
    //  端末が「アプリが直に開いてよい」と決めたカメラだけである。超広角や望遠は
    //  **論理カメラ(複数の物理カメラを束ねたもの)の配下**に隠れていて一覧に出てこない。
    //  ここでは触らずに、何がぶら下がっていて、それぞれ何ができるかだけを見る。
    @JvmStatic
    fun physicalsJson(): String {
        val m = mgr() ?: return "[]"
        val arr = JSONArray()
        if (Build.VERSION.SDK_INT < 28) { return "[]" }
        runCatching {
            for (id in m.cameraIdList) {
                val c = runCatching { m.getCameraCharacteristics(id) }.getOrNull() ?: continue
                val subs = runCatching { c.physicalCameraIds }.getOrNull() ?: emptySet<String>()
                for (sub in subs) {
                    val pc = runCatching { m.getCameraCharacteristics(sub) }.getOrNull() ?: continue
                    val o = JSONObject()
                    o.put("logical", id)
                    o.put("id", sub)
                    val sz = pc.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
                    o.put("sensorW", sz?.width?.toDouble() ?: 0.0)
                    o.put("sensorH", sz?.height?.toDouble() ?: 0.0)
                    o.put("focalMm", pc.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                                       ?.firstOrNull()?.toDouble() ?: 0.0)
                    o.put("fn", pc.get(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES)
                                  ?.firstOrNull()?.toDouble() ?: 0.0)
                    val exp = pc.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
                    o.put("expMaxNs", exp?.upper ?: 0L)
                    o.put("expMinNs", exp?.lower ?: 0L)
                    val ppx = pc.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
                    o.put("pixelW", ppx?.width ?: 0)
                    o.put("pixelH", ppx?.height ?: 0)
                    val piso = pc.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
                    o.put("isoMin", piso?.lower ?: 0)
                    o.put("isoMax", piso?.upper ?: 0)
                    o.put("facing", facingName(pc))
                    val pmap = pc.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                    val pbest = pmap?.getOutputSizes(ImageFormat.JPEG)
                                    ?.maxByOrNull { it.width.toLong() * it.height }
                    o.put("jpegW", pbest?.width ?: 0)
                    o.put("jpegH", pbest?.height ?: 0)
                    val caps = pc.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                    o.put("manual", caps?.contains(
                        CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) ?: false)
                    // 一覧に出ている id なら、そのまま単体で開ける
                    o.put("standalone", m.cameraIdList.contains(sub))
                    arr.put(o)
                }
            }
        }
        return arr.toString()
    }

    private fun facingName(c: CameraCharacteristics): String =
        when (c.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> "front"
            CameraCharacteristics.LENS_FACING_BACK  -> "back"
            else -> "external"
        }

    // 人が見分けられる名前を焦点距離から作る。35mm換算に直してから広角/標準/望遠を当てる。
    //  換算値が出せない端末では素の焦点距離を出す(それでも区別は付く)。
    private fun displayName(id: String, c: CameraCharacteristics): String {
        val model = Build.MODEL ?: "Phone"
        val f = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull()
        val sz = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        var kind = ""
        if (f != null && sz != null && sz.width > 0f) {
            // 35mm判の対角 43.27mm に対する比で換算する
            val diag = Math.hypot(sz.width.toDouble(), sz.height.toDouble())
            if (diag > 0.0) {
                val eq = f * (43.27 / diag)
                kind = when {
                    eq < 20.0 -> "超広角"
                    eq < 45.0 -> "広角"
                    else      -> "望遠"
                }
            }
        }
        if (kind.isEmpty()) { kind = "cam$id" }
        return "$model $kind"
    }

    // ── 諸元 ────────────────────────────────────────────────
    // C++ 側が設定可能値のテーブルを合成するための材料。取れないものは 0 / 空で返す。
    @JvmStatic
    fun describe(id: String): String {
        val m = mgr() ?: return "{}"
        val c = runCatching { m.getCameraCharacteristics(id) }.getOrNull() ?: return "{}"
        val o = JSONObject()
        val sz = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        o.put("sensorW", sz?.width?.toDouble() ?: 0.0)
        o.put("sensorH", sz?.height?.toDouble() ?: 0.0)
        val px = c.get(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE)
        o.put("pixelW", px?.width ?: 0)
        o.put("pixelH", px?.height ?: 0)
        val iso = c.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE)
        o.put("isoMin", iso?.lower ?: 0)
        o.put("isoMax", iso?.upper ?: 0)
        val exp = c.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE)
        o.put("expMinNs", exp?.lower ?: 0L)
        o.put("expMaxNs", exp?.upper ?: 0L)
        val ap = JSONArray()
        c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES)?.forEach { ap.put(it.toDouble()) }
        o.put("apertures", ap)
        val fl = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull()
        o.put("focalMm", fl?.toDouble() ?: 0.0)
        o.put("name", displayName(id, c))
        // マニュアル露出が使えるか。使えない端末では露出を指定しても効かない。
        val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
        val map  = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        o.put("manual", caps?.contains(
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) ?: false)
        // 【ノイズリダクションを切れるか(2026-09-05)】星は暗い点なので、端末の
        //  ノイズリダクションに「ノイズ」と見なされて消される。切れるなら、端末の映像処理の
        //  良いところ(デモザイクと色)はそのまま使い、星を消す工程だけ外せる。
        //  切れない端末では RAW から自前で作るしかないので、その判断材料としてここで返す。
        val nrModes = c.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES)
        o.put("nrOff",     nrModes?.contains(CameraMetadata.NOISE_REDUCTION_MODE_OFF) ?: false)
        o.put("nrMinimal", nrModes?.contains(CameraMetadata.NOISE_REDUCTION_MODE_MINIMAL) ?: false)
        val edModes = c.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES)
        o.put("edgeOff",   edModes?.contains(CameraMetadata.EDGE_MODE_OFF) ?: false)
        o.put("postProc",  caps?.contains(
            CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING) ?: false)
        // RAW(DNG)が撮れるか。切れない端末の逃げ道になる。
        o.put("raw", (caps?.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW) ?: false) &&
                     (map?.getOutputSizes(ImageFormat.RAW_SENSOR)?.isNotEmpty() ?: false))
        // 端末の映像処理の水準。LEGACY はマニュアル露出そのものが使えない。
        o.put("hwLevel", c.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL) ?: -1)

        // 出せる JPEG のうち最大のもの(撮影に使う)
        val best = map?.getOutputSizes(ImageFormat.JPEG)?.maxByOrNull { it.width.toLong() * it.height }
        o.put("jpegW", best?.width ?: 0)
        o.put("jpegH", best?.height ?: 0)
        return o.toString()
    }

    // ── 物理カメラを名指しできるかの実験(2026-09-05) ────────
    // 【なぜ確かめるか】論理カメラを普通に開くと、どの物理センサーで撮るかは端末側の
    //  制御ソフトが決める。露出制御を成り立たせるには「狙ったセンサーで、指定した露出で
    //  撮れている」ことが要る。仕様上は名指しできるが、守るかどうかは端末の実装による。
    //  ここでは1枚だけ撮って、**撮影結果が申告する物理カメラ id と露出**を読み取る。
    //  返すのは JSON。使うかどうかの判断材料にするだけで、通常の撮影には影響しない。
    @JvmStatic
    fun probePhysical(logicalId: String, physId: String): String {
        val o = JSONObject()
        o.put("logical", logicalId); o.put("want", physId)
        val m = mgr() ?: return o.put("error", "no camera service").toString()
        if (Build.VERSION.SDK_INT < 28) { return o.put("error", "needs Android 9").toString() }
        val c = runCatching { m.getCameraCharacteristics(physId) }.getOrNull()
            ?: return o.put("error", "no characteristics").toString()
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val size = map?.getOutputSizes(ImageFormat.JPEG)?.maxByOrNull { it.width.toLong() * it.height }
            ?: return o.put("error", "no jpeg size").toString()

        val h = ensureThread()
        var dev: CameraDevice? = null
        var ses: CameraCaptureSession? = null
        var rd: ImageReader? = null
        try {
            rd = ImageReader.newInstance(size.width, size.height, ImageFormat.JPEG, 2)
            val opened = CountDownLatch(1)
            m.openCamera(logicalId, object : CameraDevice.StateCallback() {
                override fun onOpened(d: CameraDevice) { dev = d; opened.countDown() }
                override fun onDisconnected(d: CameraDevice) { d.close(); opened.countDown() }
                override fun onError(d: CameraDevice, e: Int) {
                    o.put("error", "open error $e"); d.close(); opened.countDown() }
            }, h)
            if (!opened.await(8, TimeUnit.SECONDS) || dev == null) {
                return o.put("error", o.optString("error", "open timeout")).toString()
            }

            // 【ここが本題】出力を物理カメラに結び付ける。
            val oc = OutputConfiguration(rd.surface)
            oc.setPhysicalCameraId(physId)
            val cfg = CountDownLatch(1)
            dev!!.createCaptureSession(SessionConfiguration(
                SessionConfiguration.SESSION_REGULAR, listOf(oc), { r -> h.post(r) },
                object : CameraCaptureSession.StateCallback() {
                    override fun onConfigured(s: CameraCaptureSession) { ses = s; cfg.countDown() }
                    override fun onConfigureFailed(s: CameraCaptureSession) {
                        o.put("error", "session configure failed"); cfg.countDown() }
                }))
            if (!cfg.await(8, TimeUnit.SECONDS) || ses == null) {
                return o.put("error", o.optString("error", "session timeout")).toString()
            }

            val wantNs = 1_000_000_000L / 30   // 1/30秒
            val wantIso = 800
            // 物理カメラ宛てに露出を送れるか。送れない端末では論理側に載せる。
            // 物理カメラ宛てに送れる項目の数(0 なら名指しの設定は受け付けない端末)
            val lc = runCatching { m.getCameraCharacteristics(logicalId) }.getOrNull()
            val physKeys = if (Build.VERSION.SDK_INT >= 28)
                runCatching { lc?.availablePhysicalCameraRequestKeys }.getOrNull() else null
            o.put("physKeys", physKeys?.size ?: -1)
            val req = dev!!.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE, setOf(physId))
            req.addTarget(rd.surface)
            req.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
            req.set(CaptureRequest.SENSOR_SENSITIVITY, wantIso)
            req.set(CaptureRequest.SENSOR_EXPOSURE_TIME, wantNs)
            runCatching {
                req.setPhysicalCameraKey(CaptureRequest.SENSOR_SENSITIVITY, wantIso, physId)
                req.setPhysicalCameraKey(CaptureRequest.SENSOR_EXPOSURE_TIME, wantNs, physId)
                o.put("perPhysicalSet", true)
            }.onFailure { o.put("perPhysicalSet", false) }

            val got = CountDownLatch(2)   // 画像と撮影結果の両方
            var bytes = 0
            rd.setOnImageAvailableListener({ r ->
                runCatching { r.acquireLatestImage()?.use { bytes = it.planes[0].buffer.remaining() } }
                got.countDown()
            }, h)
            ses!!.capture(req.build(), object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(s: CameraCaptureSession, rq: CaptureRequest,
                                                res: android.hardware.camera2.TotalCaptureResult) {
                    runCatching {
                        if (Build.VERSION.SDK_INT >= 29) {
                            o.put("activePhysicalId", res.get(
                                android.hardware.camera2.CaptureResult
                                    .LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID) ?: "(none)")
                            val per = res.physicalCameraTotalResults
                            o.put("perPhysicalResults", per.keys.joinToString(","))
                            per[physId]?.let { pr ->
                                o.put("physExposureNs", pr.get(android.hardware.camera2.CaptureResult.SENSOR_EXPOSURE_TIME) ?: -1L)
                                o.put("physIso", pr.get(android.hardware.camera2.CaptureResult.SENSOR_SENSITIVITY) ?: -1)
                            }
                        }
                        o.put("exposureNs", res.get(android.hardware.camera2.CaptureResult.SENSOR_EXPOSURE_TIME) ?: -1L)
                        o.put("iso", res.get(android.hardware.camera2.CaptureResult.SENSOR_SENSITIVITY) ?: -1)
                    }
                    got.countDown()
                }
                override fun onCaptureFailed(s: CameraCaptureSession, rq: CaptureRequest,
                                             f: android.hardware.camera2.CaptureFailure) {
                    o.put("error", "capture failed reason=" + f.reason); got.countDown(); got.countDown()
                }
            }, h)
            val done = got.await(15, TimeUnit.SECONDS)
            o.put("ok", done && bytes > 0 && !o.has("error"))
            o.put("jpegBytes", bytes)
            o.put("size", "${size.width}x${size.height}")
        } catch (e: Exception) {
            o.put("error", (e.javaClass.simpleName + ": " + e.message))
        } finally {
            runCatching { ses?.close() }
            runCatching { dev?.close() }
            runCatching { rd?.close() }
        }
        return o.toString()
    }

    // ── 開く / 閉じる ───────────────────────────────────────
    // 撮るたびに開き直すと1コマに数秒かかる。撮影の間は開いたままにする。
    //  logicalId = 入口になる論理カメラ / physId = 実際に使う物理カメラ。
    //  同じなら普通に開く(配下を持たない端末)。違えば**物理カメラを名指しして**開く。
    @JvmStatic
    fun open(logicalId: String, physId: String, raw: Boolean): String {
        if (openId == physId && openLogical == logicalId && openRaw == raw && camera != null && session != null) { return "" }
        close()
        val m = mgr() ?: return "camera service not available"
        // 諸元も出力の大きさも**物理カメラ本人**から取る(論理カメラのものとは違う)。
        val c = runCatching { m.getCameraCharacteristics(physId) }.getOrNull()
            ?: return "unknown camera id: $physId"
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val nrModes = c.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES)
        canNrOff = nrModes?.contains(CameraMetadata.NOISE_REDUCTION_MODE_OFF) ?: false
        val edModes = c.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES)
        canEdgeOff = edModes?.contains(CameraMetadata.EDGE_MODE_OFF) ?: false

        val h = ensureThread()
        openRaw = raw
        useRaw = raw && rawSupported(c)
        val rd: ImageReader
        if (useRaw) {
            val size: Size = map?.getOutputSizes(ImageFormat.RAW_SENSOR)
                ?.maxByOrNull { it.width.toLong() * it.height }
                ?: return "no raw output size"
            rawW = size.width; rawH = size.height
            cfa = c.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT) ?: 0
            whiteLevel = c.get(CameraCharacteristics.SENSOR_INFO_WHITE_LEVEL) ?: 1023
            c.get(CameraCharacteristics.SENSOR_BLACK_LEVEL_PATTERN)?.let { bp ->
                blackPos = floatArrayOf(bp.getOffsetForIndex(0, 0).toFloat(), bp.getOffsetForIndex(1, 0).toFloat(),
                                        bp.getOffsetForIndex(0, 1).toFloat(), bp.getOffsetForIndex(1, 1).toFloat())
            }
            canShadingMap = c.get(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES)
                ?.contains(CameraMetadata.STATISTICS_LENS_SHADING_MAP_MODE_ON) ?: false
            // 足す最中に次のコマが届くので、受け取り口は数枚ぶん持つ(1枚 25MB)。
            rd = ImageReader.newInstance(size.width, size.height, ImageFormat.RAW_SENSOR, 4)
        } else {
            val size: Size = map?.getOutputSizes(ImageFormat.JPEG)
                ?.maxByOrNull { it.width.toLong() * it.height }
                ?: return "no jpeg output size"
            rd = ImageReader.newInstance(size.width, size.height, ImageFormat.JPEG, 2)
        }
        reader = rd

        val opened = CountDownLatch(1)
        var err: String = ""
        try {
            m.openCamera(logicalId, object : CameraDevice.StateCallback() {
                override fun onOpened(dev: CameraDevice) { camera = dev; opened.countDown() }
                override fun onDisconnected(dev: CameraDevice) {
                    err = "camera disconnected"; dev.close(); camera = null; opened.countDown()
                }
                override fun onError(dev: CameraDevice, error: Int) {
                    err = "camera open error $error"; dev.close(); camera = null; opened.countDown()
                }
            }, h)
        } catch (e: SecurityException) {
            return "camera permission not granted"
        } catch (e: Exception) {
            return "openCamera failed: ${e.message}"
        }
        if (!opened.await(8, TimeUnit.SECONDS)) { close(); return "camera open timeout" }
        val dev = camera ?: run { close(); return if (err.isEmpty()) "camera open failed" else err }

        val configured = CountDownLatch(1)
        var serr = ""
        val cb = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) { session = s; configured.countDown() }
            override fun onConfigureFailed(s: CameraCaptureSession) {
                serr = "session configure failed"; configured.countDown()
            }
        }
        try {
            if (Build.VERSION.SDK_INT >= 28) {
                val oc = OutputConfiguration(rd.surface)
                // 【ここで物理カメラに結び付ける】これをしないと、どのセンサーで撮るかは
                //  端末側の制御ソフトが決めてしまう(途中で切り替わることもある)。
                if (physId != logicalId) { oc.setPhysicalCameraId(physId) }
                dev.createCaptureSession(SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR, listOf(oc), { r -> h.post(r) }, cb))
            } else {
                @Suppress("DEPRECATION")
                dev.createCaptureSession(listOf(rd.surface), cb, h)
            }
        } catch (e: Exception) { close(); return "createCaptureSession failed: ${e.message}" }
        if (!configured.await(8, TimeUnit.SECONDS) || session == null) {
            close(); return if (serr.isEmpty()) "session timeout" else serr
        }
        openId = physId; openLogical = logicalId
        return ""
    }

    @JvmStatic
    fun close() {
        runCatching { session?.close() }
        runCatching { camera?.close() }
        runCatching { reader?.close() }
        session = null; camera = null; reader = null; openId = null; openLogical = null
    }

    // ── 撮る ────────────────────────────────────────────────
    // 【シャッターは待たずに戻る(2026-09-05 実機で判明)】
    //  露光の終わりまで待つ作りにしたら、6秒露光で1コマ 11.9秒かかり、呼び出し側の
    //  予算(8秒)を超えて毎コマ失敗した。キヤノンの CCAPI も「シャッターのPOSTは露光を
    //  待たずに戻る」ので、そちらに合わせる。撮れた画像は takeImage で受け取る
    //  (露出制御は露光が終わってから測るので、待つ場所はそちらが正しい)。
    private var pending: CountDownLatch? = null
    private var pendingJpeg: ByteArray? = null

    // 露出を指定して1枚撮り始める。成功=要求を出せた。画像は takeImage で受け取る。
    @JvmStatic
    fun capture(logicalId: String, physId: String, iso: Int, expNs: Long,
                aperture: Double, timeoutMs: Int, frames: Int, raw: Boolean): Boolean {
        val e = open(logicalId, physId, raw)
        if (e.isNotEmpty()) { return false }
        val dev = camera ?: return false
        val s = session ?: return false
        val rd = reader ?: return false
        val h = handler ?: return false

        val got = CountDownLatch(1)
        pending = got; pendingJpeg = null
        // 【足す】RAW は届いたそばから足す(足すのは C++)。露光中に前のコマを足せるので、
        //  最後のコマの後に残るのは現像と JPEG 化だけ。
        val n = if (useRaw) frames.coerceAtLeast(1) else 1
        var images = 0; var results = 0
        var lastRes: TotalCaptureResult? = null
        val finish = {
            if (images >= n && results >= n) {
                pendingJpeg = developStack(lastRes, n)
                got.countDown()
            }
        }
        if (useRaw) {
            HgeNative.nativeRawStackBegin(rawW, rawH, cfa)
            rd.setOnImageAvailableListener({ r ->
                runCatching {
                    r.acquireNextImage()?.use { img ->
                        val pl = img.planes[0]
                        if (!HgeNative.nativeRawStackAdd(pl.buffer, pl.rowStride)) {
                            Log.w("HGC-RAW", "stack add failed ${img.width}x${img.height} stride ${pl.rowStride}")
                        }
                    }
                }
                images++; finish()
            }, h)
        } else {
            rd.setOnImageAvailableListener({ r ->
                runCatching {
                    r.acquireLatestImage()?.use { img ->
                        val buf = img.planes[0].buffer
                        val b = ByteArray(buf.remaining())
                        buf.get(b)
                        pendingJpeg = b
                    }
                }
                got.countDown()
            }, h)
        }

        try {
            // 露出を**その物理カメラ宛て**に送れるよう、要求の宛先に加える(2026-09-05 実機で確認)。
            val req = if (Build.VERSION.SDK_INT >= 28 && physId != logicalId)
                dev.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE, setOf(physId))
            else dev.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE)
            req.addTarget(rd.surface)
            // マニュアル露出。AE を切らないと指定した ISO / 露光時間が無視される。
            req.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_OFF)
            if (iso > 0)    { req.set(CaptureRequest.SENSOR_SENSITIVITY, iso) }
            if (expNs > 0L) { req.set(CaptureRequest.SENSOR_EXPOSURE_TIME, expNs) }
            if (aperture > 0.0) { req.set(CaptureRequest.LENS_APERTURE, aperture.toFloat()) }
            // 【フレーム時間も伸ばす(長秒の露光が切り詰められないように)】
            //  SENSOR_FRAME_DURATION が露光より短いと、端末によっては露光が縮む。
            if (expNs > 0L) { req.set(CaptureRequest.SENSOR_FRAME_DURATION, expNs) }
            // 星を撮るので、ぶれ補正と手ぶれ補正は切る(三脚前提)。無い端末では黙って無視される。
            req.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
            req.set(CaptureRequest.CONTROL_AWB_MODE, CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT)
            // 星を消さないための2行。端末が対応しているときだけ載せる(上の canNrOff/canEdgeOff)。
            if (canNrOff)   { req.set(CaptureRequest.NOISE_REDUCTION_MODE, CameraMetadata.NOISE_REDUCTION_MODE_OFF) }
            if (canEdgeOff) { req.set(CaptureRequest.EDGE_MODE, CameraMetadata.EDGE_MODE_OFF) }
            // 物理カメラ宛てにも同じ露出を載せる。受け付けない端末では黙って無視される。
            if (Build.VERSION.SDK_INT >= 28 && physId != logicalId) {
                runCatching {
                    if (iso > 0)    { req.setPhysicalCameraKey(CaptureRequest.SENSOR_SENSITIVITY, iso, physId) }
                    if (expNs > 0L) { req.setPhysicalCameraKey(CaptureRequest.SENSOR_EXPOSURE_TIME, expNs, physId) }
                }
            }
            // RAW は自前で現像するので、周辺減光の地図を撮影結果に付けてもらう(掛け戻しに使う)。
            if (useRaw && canShadingMap) {
                req.set(CaptureRequest.STATISTICS_LENS_SHADING_MAP_MODE, CameraMetadata.STATISTICS_LENS_SHADING_MAP_MODE_ON)
            }
            // 【狙ったセンサーで撮れたかを毎コマ確かめる】端末が勝手に切り替えていないかは
            //  推測できないので、撮影結果の申告を控えて上位が見られるようにする。
            val cb = object : CameraCaptureSession.CaptureCallback() {
                override fun onCaptureCompleted(ss: CameraCaptureSession, rq: CaptureRequest,
                                                res: TotalCaptureResult) {
                    if (Build.VERSION.SDK_INT >= 29) {
                        activePhys = runCatching {
                            res.get(CaptureResult.LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID) ?: ""
                        }.getOrDefault("")
                    }
                    if (useRaw) { lastRes = res; results++; finish() }
                }
                override fun onCaptureFailed(ss: CameraCaptureSession, rq: CaptureRequest,
                                             f: android.hardware.camera2.CaptureFailure) {
                    // 1コマでも落ちたら足しても正しい明るさにならない。この1枚は無しにして戻す。
                    Log.w("HGC-RAW", "capture failed reason=${f.reason}")
                    if (useRaw) { pendingJpeg = null; got.countDown() }
                }
            }
            val built = req.build()
            if (n > 1) {
                // 続けて撮る。要求をまとめて渡すので、読み出しの隙間は端末の最小で済む。
                s.captureBurst(List(n) { built }, cb, h)
            } else {
                s.capture(built, cb, h)
            }
        } catch (ex: Exception) { pending = null; return false }
        return true
    }

    // 足したものを現像して JPEG にする。ホワイトバランス・色行列・黒レベル・周辺減光は
    //  撮影結果(端末の映像処理が使った値)をそのまま使う。
    private fun developStack(res: TotalCaptureResult?, frames: Int): ByteArray? {
        val t0 = SystemClock.elapsedRealtime()
        val black = blackPos.copyOf()
        res?.get(CaptureResult.SENSOR_DYNAMIC_BLACK_LEVEL)?.let { if (it.size >= 4) for (i in 0..3) black[i] = it[i] }
        val gains = floatArrayOf(1f, 1f, 1f, 1f)
        res?.get(CaptureResult.COLOR_CORRECTION_GAINS)?.let {
            gains[0] = it.red; gains[1] = it.greenEven; gains[2] = it.greenOdd; gains[3] = it.blue
        }
        val ccm = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)
        res?.get(CaptureResult.COLOR_CORRECTION_TRANSFORM)?.let { t ->
            for (r in 0..2) for (c in 0..2) ccm[r * 3 + c] = t.getElement(c, r).toFloat()
        }
        var shading: FloatArray? = null; var cols = 0; var rows = 0
        res?.get(CaptureResult.STATISTICS_LENS_SHADING_CORRECTION_MAP)?.let { m ->
            cols = m.columnCount; rows = m.rowCount
            val a = FloatArray(4 * cols * rows)
            for (ch in 0..3) for (y in 0 until rows) for (x in 0 until cols) {
                a[ch * cols * rows + y * cols + x] = m.getGainFactor(ch, x, y)
            }
            shading = a
        }
        val bmp = Bitmap.createBitmap(rawW / 2, rawH / 2, Bitmap.Config.ARGB_8888)
        val ok = HgeNative.nativeRawStackDevelop(bmp, whiteLevel, black, gains, ccm, shading, cols, rows)
        if (!ok) { bmp.recycle(); Log.w("HGC-RAW", "develop failed"); return null }
        val bos = ByteArrayOutputStream(2 shl 20)
        bmp.compress(Bitmap.CompressFormat.JPEG, 92, bos)
        bmp.recycle()
        lastStackMs = (SystemClock.elapsedRealtime() - t0).toInt()
        Log.i("HGC-RAW", "stack $frames frames -> ${rawW / 2}x${rawH / 2} in ${lastStackMs}ms " +
                         "wb=${gains.toList()} black=${black.toList()} white=$whiteLevel shading=${cols}x$rows")
        return bos.toByteArray()
    }

    // 直前に始めた1枚を受け取る。まだ露光中なら終わるまで待つ。取れなければ null。
    //  一度受け取ったら捨てる(同じ画像を次のコマの測光へ使い回さないため)。
    // 直前のコマを実際に撮った物理カメラ id(端末の申告)。空=分からない端末。
    @JvmStatic
    fun activePhysicalId(): String = activePhys

    @JvmStatic
    fun takeImage(timeoutMs: Int): ByteArray? {
        val got = pending ?: return null
        val wait = if (timeoutMs > 0) timeoutMs.toLong() else 15000L
        if (!got.await(wait, TimeUnit.MILLISECONDS)) { return null }
        pending = null
        val b = pendingJpeg
        pendingJpeg = null
        return b
    }
}
