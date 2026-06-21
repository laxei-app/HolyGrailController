package app.laxei.holygrail

// 撮影計画の「開始時の撮影方向(方位磁石+矢印)」「開始時の仰角(カメラの絵を回転)」の入力ウィジェット。
// 仕様書10 §7.3.1 画面330。ドラッグで角度を変え、指を離したとき onCommit を呼ぶ。
// 画角(fov)を半透明の扇で重ね、太陽が画角に入るかを目で確認できるようにする。

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.util.AttributeSet
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin

// ── 方位磁石(方位[°] 0=北・90=東、時計回り) ──────────────────────────────
class CompassView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : View(context, attrs) {

    var azimuth: Float = 90f
        private set
    private var fovH = 80f
    private var sunriseAz = Float.NaN
    private var sunsetAz = Float.NaN
    private var moonriseAz = Float.NaN
    private var moonsetAz = Float.NaN
    var onCommit: ((Float) -> Unit)? = null

    private var cx = 0f; private var cy = 0f; private var rad = 0f

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val ringP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(2f); color = 0xFF888888.toInt() }
    private val fovP  = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0x447E57C2 }
    private val arrowP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xFF7E57C2.toInt() }
    private val tailP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xFFB0BEC5.toInt() }
    private val sunP  = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFA000.toInt() }
    private val moonP = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFDD835.toInt() }
    private val markTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF666666.toInt(); textAlign = Paint.Align.CENTER; textSize = sp(9f) }
    private val cardP = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; textSize = sp(12f); isFakeBoldText = true }
    private val valP  = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF222222.toInt(); textAlign = Paint.Align.CENTER; textSize = sp(18f); isFakeBoldText = true }

    fun setAzimuth(a: Float) { azimuth = norm(a); invalidate() }
    fun setFov(h: Float) { fovH = h; invalidate() }
    fun setMarkers(sr: Float, ss: Float, mr: Float, ms: Float) { sunriseAz = sr; sunsetAz = ss; moonriseAz = mr; moonsetAz = ms; invalidate() }

    private fun norm(a: Float): Float { var x = a % 360f; if (x < 0f) x += 360f; return x }

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        val valH = sp(22f)
        cx = w / 2f
        cy = valH + (h - valH) / 2f
        rad = min(w.toFloatSafe(), (h - valH)) / 2f - dp(16f)
    }
    private fun Int.toFloatSafe() = this.toFloat()

    private fun px(az: Float, r: Float) = cx + r * sin(Math.toRadians(az.toDouble())).toFloat()
    private fun py(az: Float, r: Float) = cy - r * cos(Math.toRadians(az.toDouble())).toFloat()

    override fun onDraw(c: Canvas) {
        if (rad <= 0f) return
        // 数値(上部中央)
        c.drawText("%.1f°".format(azimuth), cx, sp(18f), valP)
        // 画角の扇(撮影方向±fovH/2)。Android arc角は+x軸基準・時計回りなので az-90 が起点。
        val rect = RectF(cx - rad, cy - rad, cx + rad, cy + rad)
        c.drawArc(rect, (azimuth - fovH / 2f) - 90f, fovH, true, fovP)
        c.drawCircle(cx, cy, rad, ringP)
        // 方位ラベル
        cardP.color = 0xFFD32F2F.toInt(); c.drawText("N", cx, cy - rad - dp(3f), cardP)
        cardP.color = 0xFF555555.toInt()
        c.drawText("E", cx + rad + dp(10f), cy + dp(4f), cardP)
        c.drawText("S", cx, cy + rad + dp(13f), cardP)
        c.drawText("W", cx - rad - dp(10f), cy + dp(4f), cardP)
        // 太陽/月マーカー
        drawMarker(c, sunriseAz, sunP, markTxt, "日の出")
        drawMarker(c, sunsetAz, sunP, markTxt, "日の入")
        drawMarker(c, moonriseAz, moonP, markTxt, "月出")
        drawMarker(c, moonsetAz, moonP, markTxt, "月入")
        // 矢印(撮影方向)
        val tipR = rad - dp(8f)
        val tipX = px(azimuth, tipR); val tipY = py(azimuth, tipR)
        val bL = azimuth - 150f; val bR = azimuth + 150f
        val path = Path()
        path.moveTo(tipX, tipY)
        path.lineTo(px(bL, dp(9f)), py(bL, dp(9f)))
        path.lineTo(cx, cy)
        path.lineTo(px(bR, dp(9f)), py(bR, dp(9f)))
        path.close()
        c.drawPath(path, arrowP)
        c.drawCircle(cx, cy, dp(4f), tailP)
    }

    private fun drawMarker(c: Canvas, az: Float, dot: Paint, txt: Paint, label: String) {
        if (az.isNaN()) return
        val x = px(az, rad); val y = py(az, rad)
        c.drawCircle(x, y, dp(5f), dot)
        val ly = if (y < cy) y - dp(7f) else y + dp(14f)
        c.drawText(label, x, ly, txt)
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                val dx = e.x - cx; val dy = e.y - cy
                azimuth = norm(Math.toDegrees(atan2(dx.toDouble(), -dy.toDouble())).toFloat())
                invalidate(); return true
            }
            MotionEvent.ACTION_UP -> { onCommit?.invoke(azimuth); return true }
        }
        return super.onTouchEvent(e)
    }
}

// ── 仰角(カメラの絵を回転。上=正、下=負) ──────────────────────────────────
class ElevationView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : View(context, attrs) {

    var angle: Float = 10f   // 仰角[°]。View.elevation(Z方向)と衝突するため別名。
        private set
    private var fovV = 50f
    var onCommit: ((Float) -> Unit)? = null

    private var cx = 0f; private var cy = 0f; private var rad = 0f

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val horizP = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1.5f); color = 0xFFBDBDBD.toInt() }
    private val fovP   = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0x447E57C2 }
    private val bodyP  = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xFF455A64.toInt() }
    private val lensP  = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL; color = 0xFF7E57C2.toInt() }
    private val arcP   = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1.5f); color = 0xFF7E57C2.toInt() }
    private val valP   = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF222222.toInt(); textAlign = Paint.Align.CENTER; textSize = sp(18f); isFakeBoldText = true }

    private var prevAng = 0f   // 直前のタッチ角(ピボット周り, ラジアン)
    private var hasPrev = false

    fun setAngle(a: Float) { angle = clamp(a); invalidate() }
    fun setFov(v: Float) { fovV = v; invalidate() }
    private fun clamp(a: Float) = if (a > 90f) 90f else if (a < -90f) -90f else a

    override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) {
        val valH = sp(22f)
        cx = w / 2f
        cy = valH + (h - valH) / 2f
        rad = min(w.toFloat(), (h - valH)) / 2f - dp(14f)
    }

    override fun onDraw(c: Canvas) {
        if (rad <= 0f) return
        c.drawText("%.1f°".format(angle), cx, sp(18f), valP)
        // 水平線(地平)
        c.drawLine(cx - rad, cy, cx + rad, cy, horizP)
        // 垂直画角の扇(仰角±fovV/2)。右向き(+x)を 0° とし、上向き(仰角+)へ。
        val rect = RectF(cx - rad, cy - rad, cx + rad, cy + rad)
        c.drawArc(rect, -(angle + fovV / 2f), fovV, true, fovP)
        // 仰角を示す円弧(水平→撮影方向)
        c.drawArc(rect, -angle, angle, false, arcP)
        // カメラの絵(ピボット中心に -elevation 回転=上を向く)
        c.save()
        c.rotate(-angle, cx, cy)
        val bw = dp(22f); val bh = dp(15f)
        c.drawRect(cx - bw, cy - bh, cx + bw * 0.4f, cy + bh, bodyP)        // 本体
        c.drawRect(cx - bw * 0.2f, cy - bh * 0.6f, cx + bw, cy + bh * 0.6f, lensP) // レンズ(右=撮影方向)
        c.drawRect(cx - bw * 0.5f, cy - bh - dp(4f), cx + dp(2f), cy - bh, bodyP)   // ホットシュー
        c.restore()
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                prevAng = atan2((e.y - cy).toDouble(), (e.x - cx).toDouble()).toFloat()
                hasPrev = true
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (!hasPrev) return true
                // ピボット周りの「回転量」を積算して仰角に反映する。指を弧状に回しても
                // 素直に追従し、±90°でクランプして止まる(逆に回せばすぐ戻る=跳ね返らない)。
                val cur = atan2((e.y - cy).toDouble(), (e.x - cx).toDouble()).toFloat()
                val r = kotlin.math.hypot((e.x - cx).toDouble(), (e.y - cy).toDouble())
                if (r >= dp(10f)) {   // 中心付近の微小半径はジッタするので無視
                    var d = Math.toDegrees((cur - prevAng).toDouble()).toFloat()
                    while (d > 180f) d -= 360f
                    while (d < -180f) d += 360f
                    angle = clamp(angle - d)   // 仰角 = -(画面ポインティング角)
                    invalidate()
                }
                prevAng = cur
                return true
            }
            MotionEvent.ACTION_UP -> { hasPrev = false; onCommit?.invoke(angle); return true }
        }
        return super.onTouchEvent(e)
    }
}

// ── スケジュール右の撮影制御方法バンド ──────────────────────────────
// 行(イベント)数 rows に対して全高を等分し、各区間(seg)を top..bottom(行単位)で塗る。
// 区間の境目は撮影制御方法の実開始時刻から MainActivity 側で算出して渡す
// (イベント±10分は行中心、前後ならイベントの手前/後ろ)。左のイベント列と高さが揃う。
class BandView(context: Context) : View(context) {
    data class Seg(val top: Float, val bottom: Float, val color: Int, val label: String?, val type: Int,
                   val textColor: Int = 0xFF212121.toInt())
    var segs: List<Seg> = emptyList()
    var rows: Int = 0
    // タップされた区間の撮影制御方法 種別を返す(編集画面へ遷移するため)。
    var onTapType: ((Int) -> Unit)? = null

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFF212121.toInt(); textAlign = Paint.Align.CENTER
    }

    private val gd = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent): Boolean = true
        override fun onSingleTapUp(e: MotionEvent): Boolean {
            if (rows <= 0 || height <= 0) return false
            val pos = e.y / height * rows
            for (s in segs) {
                if (pos >= s.top && pos < s.bottom) { onTapType?.invoke(s.type); return true }
            }
            return false
        }
    })

    init { isClickable = true }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        return gd.onTouchEvent(e) || super.onTouchEvent(e)
    }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        if (rows <= 0 || height <= 0) return
        val unit = height.toFloat() / rows
        txt.textSize = resources.displayMetrics.scaledDensity * 13f
        for (s in segs) {
            val top = s.top * unit
            val bot = s.bottom * unit
            if (bot - top <= 0f) continue
            if (s.color != 0) { fill.color = s.color; c.drawRect(0f, top, width.toFloat(), bot, fill) }
            val lbl = s.label
            if (lbl != null && bot - top >= txt.textSize * 1.3f) {
                txt.color = s.textColor
                val cy = (top + bot) / 2f - (txt.descent() + txt.ascent()) / 2f
                c.drawText(lbl, width / 2f, cy, txt)
            }
        }
    }
}
