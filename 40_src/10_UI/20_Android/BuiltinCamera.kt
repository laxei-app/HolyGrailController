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
import android.media.ImageReader
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Size
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

    private var openId: String? = null
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null

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
    // 【物理カメラをそのまま並べる】論理カメラの配下にある物理カメラは getCameraIdList に
    //  出てこないので、論理カメラを開いて physicalCameraIds を辿る…という手もあるが、
    //  端末によって「物理カメラ単体では開けない」ものがある(実装依存)。まずは
    //  getCameraIdList が返す id をそのまま1台ずつ扱う(これは必ず開ける)。
    @JvmStatic
    fun listCameras(): String {
        val m = mgr() ?: return "[]"
        val arr = JSONArray()
        runCatching {
            for (id in m.cameraIdList) {
                val c = runCatching { m.getCameraCharacteristics(id) }.getOrNull() ?: continue
                val o = JSONObject()
                o.put("id", id)
                o.put("name", displayName(id, c))
                o.put("facing", facingName(c))
                arr.put(o)
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
        val face = if (facingName(c) == "front") "前" else "後"
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
        return "$model $face$kind"
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

    // ── 開く / 閉じる ───────────────────────────────────────
    // 撮るたびに開き直すと1コマに数秒かかる。撮影の間は開いたままにする。
    @JvmStatic
    fun open(id: String): String {
        if (openId == id && camera != null && session != null) { return "" }
        close()
        val m = mgr() ?: return "camera service not available"
        val c = runCatching { m.getCameraCharacteristics(id) }.getOrNull()
            ?: return "unknown camera id: $id"
        val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val size: Size = map?.getOutputSizes(ImageFormat.JPEG)
            ?.maxByOrNull { it.width.toLong() * it.height }
            ?: return "no jpeg output size"
        val nrModes = c.get(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES)
        canNrOff = nrModes?.contains(CameraMetadata.NOISE_REDUCTION_MODE_OFF) ?: false
        val edModes = c.get(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES)
        canEdgeOff = edModes?.contains(CameraMetadata.EDGE_MODE_OFF) ?: false

        val h = ensureThread()
        val rd = ImageReader.newInstance(size.width, size.height, ImageFormat.JPEG, 2)
        reader = rd

        val opened = CountDownLatch(1)
        var err: String = ""
        try {
            m.openCamera(id, object : CameraDevice.StateCallback() {
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
                dev.createCaptureSession(SessionConfiguration(
                    SessionConfiguration.SESSION_REGULAR,
                    listOf(OutputConfiguration(rd.surface)),
                    { r -> h.post(r) }, cb))
            } else {
                @Suppress("DEPRECATION")
                dev.createCaptureSession(listOf(rd.surface), cb, h)
            }
        } catch (e: Exception) { close(); return "createCaptureSession failed: ${e.message}" }
        if (!configured.await(8, TimeUnit.SECONDS) || session == null) {
            close(); return if (serr.isEmpty()) "session timeout" else serr
        }
        openId = id
        return ""
    }

    @JvmStatic
    fun close() {
        runCatching { session?.close() }
        runCatching { camera?.close() }
        runCatching { reader?.close() }
        session = null; camera = null; reader = null; openId = null
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
    fun capture(id: String, iso: Int, expNs: Long, aperture: Double, timeoutMs: Int): Boolean {
        val e = open(id)
        if (e.isNotEmpty()) { return false }
        val dev = camera ?: return false
        val s = session ?: return false
        val rd = reader ?: return false
        val h = handler ?: return false

        val got = CountDownLatch(1)
        pending = got; pendingJpeg = null
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

        try {
            val req = dev.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE)
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
            s.capture(req.build(), null, h)
        } catch (ex: Exception) { pending = null; return false }
        return true
    }

    // 直前に始めた1枚を受け取る。まだ露光中なら終わるまで待つ。取れなければ null。
    //  一度受け取ったら捨てる(同じ画像を次のコマの測光へ使い回さないため)。
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
