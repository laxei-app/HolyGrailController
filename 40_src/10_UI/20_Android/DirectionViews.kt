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
import kotlin.math.max
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
    // 項目12: ドラッグ中(指を離す前)も呼ばれる。撮影シミュレーションのリアルタイム追従に使う。
    var onChange: ((Float) -> Unit)? = null

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
        drawMarkers(c)
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

    // 日の出/日の入/月の出/月の入(2026-09-02 UI依頼で描き方を変更)。
    //
    // 【なぜ外へ出すか】以前は丸の真上/真下に文字を置いていたので、方位の輪・画角の扇・
    //  E/W の方位ラベルと重なって読めなかった。これらは**必ず東か西の周りに出る**ので
    //  円の左右には余白がある。文字は丸の外(東側は右へ、西側は左へ)に逃がす。
    // 【角度も出す】何度から昇る/沈むのかは構図を決めるのに要る。目盛りを読ませずに済ませる。
    // 【重なりをほどく】太陽と月が近い日は丸も文字も重なる。同じ側のものを上から順に見て、
    //  行の高さぶん空いていなければ下へずらす(3つ以上でも順に押し下がる)。
    private class Mark(val az: Float, val label: String, val dot: Paint) { var ly = 0f }

    private fun drawMarkers(c: Canvas) {
        val all = ArrayList<Mark>()
        fun add(az: Float, label: String, p: Paint) { if (!az.isNaN()) all.add(Mark(norm(az), label, p)) }
        add(sunriseAz, "日の出", sunP)
        add(sunsetAz, "日の入", sunP)
        add(moonriseAz, "月の出", moonP)
        add(moonsetAz, "月の入", moonP)
        if (all.isEmpty()) return
        for (m in all) c.drawCircle(px(m.az, rad), py(m.az, rad), dp(5f), m.dot)

        val lineH = markTxt.textSize * 1.25f
        for (east in listOf(true, false)) {
            val side = all.filter { (it.az < 180f) == east }
            if (side.isEmpty()) continue
            for (m in side) m.ly = py(m.az, rad) + markTxt.textSize * 0.35f   // 丸の中心に文字の高さを合わせる
            val sorted = side.sortedBy { it.ly }
            for (i in 1 until sorted.size) {
                if (sorted[i].ly - sorted[i - 1].ly < lineH) sorted[i].ly = sorted[i - 1].ly + lineH
            }
            markTxt.textAlign = if (east) Paint.Align.LEFT else Paint.Align.RIGHT
            for (m in sorted) {
                val s = m.label + " " + "%.1f°".format(m.az)
                val tw = markTxt.measureText(s)
                // 方位ラベル(E/W)より外側から始める。長い文字は画面外へ出ないよう内側へ寄せる。
                val x = if (east) min(cx + rad + dp(24f), width - tw - dp(2f))
                        else       max(cx - rad - dp(24f), tw + dp(2f))
                c.drawText(s, x, m.ly, markTxt)
            }
        }
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false   // 撮影中(読取専用)は方向変更不可
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                parent?.requestDisallowInterceptTouchEvent(true)
                val dx = e.x - cx; val dy = e.y - cy
                azimuth = norm(Math.toDegrees(atan2(dx.toDouble(), -dy.toDouble())).toFloat())
                invalidate()
                onChange?.invoke(azimuth)   // 項目12: ドラッグ中もリアルタイムに反映する
                return true
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
    // 項目12: ドラッグ中(指を離す前)も呼ばれる。撮影シミュレーションのリアルタイム追従に使う。
    var onChange: ((Float) -> Unit)? = null

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

    // カメラの絵(2026-08-08 UI依頼で図形描画から画像へ)。撮る向きで絵を替える:
    //  横向きで撮る(ランドスケープ)=ICOカメラ(右透) / 縦向き=ICOカメラ(上)。
    //  どちらも 80×80 の透過PNG。仰角に合わせてピボット中心に回転させる。
    private var landscape = true
    private val camSide: android.graphics.Bitmap? =
        try { android.graphics.BitmapFactory.decodeResource(resources, R.drawable.ic_cam_side) } catch (_: Exception) { null }
    private val camUp: android.graphics.Bitmap? =
        try { android.graphics.BitmapFactory.decodeResource(resources, R.drawable.ic_cam_up) } catch (_: Exception) { null }
    private val camPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { isFilterBitmap = true }

    // 横向き/縦向きの切替(撮影計画の「横向きで撮る」に追従)。
    fun setLandscape(l: Boolean) { if (landscape != l) { landscape = l; invalidate() } }

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
        // カメラの絵(ピボット中心に -elevation 回転=上を向く)。
        // 2026-08-08 UI依頼: 図形描画から画像へ。横向き/縦向きで絵を替える。
        val bmp = if (landscape) camSide else camUp
        c.save()
        c.rotate(-angle, cx, cy)
        if (bmp != null) {
            // 扇(画角)と水平線を隠しすぎない大きさにする。半径の 0.9 倍を一辺とする正方形。
            val side = rad * 0.9f
            val dst = RectF(cx - side / 2f, cy - side / 2f, cx + side / 2f, cy + side / 2f)
            c.drawBitmap(bmp, null, dst, camPaint)
        } else {
            // 画像が読めない環境向けの従来描画(保険)。
            val bw = dp(22f); val bh = dp(15f)
            c.drawRect(cx - bw, cy - bh, cx + bw * 0.4f, cy + bh, bodyP)
            c.drawRect(cx - bw * 0.2f, cy - bh * 0.6f, cx + bw, cy + bh * 0.6f, lensP)
            c.drawRect(cx - bw * 0.5f, cy - bh - dp(4f), cx + dp(2f), cy - bh, bodyP)
        }
        c.restore()
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false   // 撮影中(読取専用)は仰角変更不可
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
                    onChange?.invoke(angle)    // 項目12: ドラッグ中もリアルタイムに反映する
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
