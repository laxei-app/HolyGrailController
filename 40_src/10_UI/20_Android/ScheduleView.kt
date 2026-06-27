package app.laxei.holygrail

// 撮影計画スケジュール表示・編集ビュー(仕様書10 §7.3.2)。
//  左 : 太陽高度/薄明の帯(夕→夜→朝の明暗グラデーション)
//  中 : 撮影制御方法の境目の時刻・太陽高度(開始/終了/月出入りも)
//  右 : 撮影制御方法の色帯(背景色/文字色は設定値)
// 操作:
//  ・境目を上下ドラッグ → その時刻を ccm の開始/終了に設定(onMoveBoundary)
//  ・夕日/朝日の帯を右へドラッグ → 排除、日中候補帯を左へドラッグ → 挿入(onSetBand)
//  ・帯をタップ → その撮影制御方法の編集へ(onTapType)
// 時刻は縦軸に線形配置(上=撮影開始, 下=撮影終了)。

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.view.MotionEvent
import android.view.View
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.abs

class ScheduleView(context: Context) : View(context) {

    data class Win(val type: Int, val startMs: Long, val endMs: Long, val name: String?,
                   val color: Int, val textColor: Int, val startAlt: Double, val endAlt: Double)
    data class Ev(val kind: Int, val ms: Long)

    private var startMs = 0L
    private var endMs = 0L
    private var wins: List<Win> = emptyList()
    private var evs: List<Ev> = emptyList()

    // 現在の帯モード(0=自動,1=挿入,2=排除)。setBand 時に他方を保つために保持。
    var curSunriseMode = 0
    var curSunsetMode = 0

    var onTapType: ((Int) -> Unit)? = null
    var onMoveBoundary: ((Int, Int, Int, Long) -> Unit)? = null   // before, after, occ, newMs
    var onSetBand: ((Boolean, Boolean) -> Unit)? = null           // rising, insert

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val bandW get() = dp(30f)
    private val labelW get() = dp(104f)
    private val topPad get() = dp(10f)
    private val botPad get() = dp(10f)
    private val ccmX0 get() = bandW + labelW

    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1f); color = 0xFF607D8B.toInt() }
    private val dragLine = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(2f); color = 0xFFD32F2F.toInt() }
    private val txt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF263238.toInt(); textSize = sp(10f) }
    private val txtR = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF263238.toInt(); textSize = sp(10f); textAlign = Paint.Align.RIGHT }
    private val bandTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt(); textSize = sp(10f); textAlign = Paint.Align.CENTER; isFakeBoldText = true }
    private val nameTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; textSize = sp(12f); isFakeBoldText = true }

    private val fmtDateTime = SimpleDateFormat("M/d HH:mm", Locale.US)
    private val fmtTime = SimpleDateFormat("HH:mm", Locale.US)
    private val fmtDay = SimpleDateFormat("M/d(E)", Locale.JAPAN)

    fun setData(startMs: Long, endMs: Long, wins: List<Win>, evs: List<Ev>) {
        this.startMs = startMs; this.endMs = endMs; this.wins = wins; this.evs = evs
        invalidate()
    }

    private fun yOf(ms: Long): Float {
        val span = (endMs - startMs).coerceAtLeast(1L)
        return topPad + (ms - startMs).toFloat() / span * (height - topPad - botPad)
    }
    private fun msOf(y: Float): Long {
        val span = (height - topPad - botPad).coerceAtLeast(1f)
        val f = ((y - topPad) / span).coerceIn(0f, 1f)
        return startMs + (f * (endMs - startMs)).toLong()
    }

    // 薄明イベント種別 → 暗さ(0=昼,1=夜)。横軸の太陽高度帯の色付けに使う。
    private fun darknessOf(kind: Int): Float = when (kind) {
        2 -> 0.28f; 3 -> 0.5f; 4 -> 0.72f; 5 -> 0.9f          // 夕(日没→市民→航海→天文)
        6 -> 0.9f; 7 -> 0.72f; 8 -> 0.5f; 9 -> 0.28f          // 朝(天文→航海→市民→日の出)
        else -> -1f
    }
    private fun lerp(a: Int, b: Int, t: Float): Int {
        val tt = t.coerceIn(0f, 1f)
        val ar = (a ushr 16) and 0xFF; val ag = (a ushr 8) and 0xFF; val ab = a and 0xFF
        val br = (b ushr 16) and 0xFF; val bg = (b ushr 8) and 0xFF; val bb = b and 0xFF
        val r = (ar + (br - ar) * tt).toInt(); val g = (ag + (bg - ag) * tt).toInt(); val bl = (ab + (bb - ab) * tt).toInt()
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
    }
    private val dayCol = 0x90CAF9   // 昼の空(薄青)
    private val nightCol = 0x0D1B2A // 夜の空(濃紺)

    private fun drawTwilightBand(c: Canvas) {
        val h = (height - topPad - botPad)
        if (h <= 0) return
        // 暗さの stop を作る(薄明イベント)。端は前後関係から昼/夜を推定。
        val stops = evs.filter { darknessOf(it.kind) >= 0f && it.ms in startMs..endMs }
            .sortedBy { it.ms }
        val pos = ArrayList<Float>(); val cols = ArrayList<Int>()
        fun add(p: Float, dark: Float) { pos.add(p.coerceIn(0f, 1f)); cols.add(lerp(dayCol, nightCol, dark)) }
        val startDark = when {
            stops.isEmpty() -> 0.5f
            darknessOf(stops.first().kind) in floatArrayOf(0.28f,0.5f,0.72f,0.9f).toList() && stops.first().kind in 2..5 -> 0.1f // 夕の前=昼
            else -> 1.0f  // 朝の前=夜
        }
        val endDark = when {
            stops.isEmpty() -> 0.5f
            stops.last().kind in 2..5 -> 1.0f   // 夕のあと=夜
            else -> 0.1f                          // 朝のあと=昼
        }
        add(0f, startDark)
        for (s in stops) add((s.ms - startMs).toFloat() / (endMs - startMs).coerceAtLeast(1L), darknessOf(s.kind))
        add(1f, endDark)
        val grad = LinearGradient(0f, topPad, 0f, topPad + h,
            cols.toIntArray(), pos.toFloatArray(), Shader.TileMode.CLAMP)
        fill.shader = grad
        c.drawRect(0f, topPad, bandW, topPad + h, fill)
        fill.shader = null
        // 夕/朝の見出し(帯の該当領域に小さく)
        val dusk = stops.firstOrNull { it.kind in 2..5 }
        val dawn = stops.firstOrNull { it.kind in 6..9 }
        if (dusk != null) c.drawText("夕", bandW / 2f - sp(6f), yOf(dusk.ms), bandTxt)
        if (dawn != null) c.drawText("朝", bandW / 2f - sp(6f), yOf(dawn.ms), bandTxt)
    }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        if (wins.isEmpty() || endMs <= startMs || height <= 0) return
        drawTwilightBand(c)

        // 右: 撮影制御方法の色帯
        for (w in wins) {
            val top = yOf(w.startMs); val bot = yOf(w.endMs)
            if (bot - top <= 0f) continue
            if (w.color != 0) { fill.color = w.color; c.drawRect(ccmX0, top, width.toFloat(), bot, fill) }
            val nm = w.name
            if (nm != null && bot - top >= nameTxt.textSize * 1.2f) {
                nameTxt.color = w.textColor
                c.drawText(nm, (ccmX0 + width) / 2f, (top + bot) / 2f - (nameTxt.descent() + nameTxt.ascent()) / 2f, nameTxt)
            }
        }

        // 中: 境目の時刻・太陽高度。先頭=開始、末尾=終了、各境目=その時刻+次窓の開始高度。
        // 開始
        drawLabel(c, startMs, wins.first().startAlt, true, isEdge = true)
        // 各境目
        for (i in 0 until wins.size - 1) {
            val tb = wins[i].endMs
            c.drawLine(bandW, yOf(tb), width.toFloat(), yOf(tb), line)
            drawLabel(c, tb, wins[i + 1].startAlt, false, isEdge = false)
        }
        // 終了
        drawLabel(c, endMs, wins.last().endAlt, false, isEdge = true)

        // 月の出入りマーカー(中列の右寄せ)
        for (e in evs) {
            if (e.kind != 10 && e.kind != 11) continue
            if (e.ms !in startMs..endMs) continue
            val y = yOf(e.ms)
            txtR.color = 0xFF8D6E63.toInt()
            c.drawText(if (e.kind == 10) "月出 ${fmtTime.format(Date(e.ms))}" else "月入 ${fmtTime.format(Date(e.ms))}",
                ccmX0 - dp(2f), y + sp(10f), txtR)
            txtR.color = 0xFF263238.toInt()
        }

        // ドラッグ中の境目プレビュー
        if (dragMode == DRAG_V && dragBoundary >= 0) {
            c.drawLine(bandW, dragY, width.toFloat(), dragY, dragLine)
            txt.color = 0xFFD32F2F.toInt()
            c.drawText(fmtTime.format(Date(msOf(dragY))), bandW + dp(2f), dragY - dp(3f), txt)
            txt.color = 0xFF263238.toInt()
        }
    }

    private fun drawLabel(c: Canvas, ms: Long, alt: Double, top: Boolean, isEdge: Boolean) {
        val y = yOf(ms)
        val spansDay = run {
            val a = java.util.Calendar.getInstance().apply { timeInMillis = startMs }.get(java.util.Calendar.DAY_OF_YEAR)
            val b = java.util.Calendar.getInstance().apply { timeInMillis = endMs }.get(java.util.Calendar.DAY_OF_YEAR)
            a != b
        }
        val timeStr = if (spansDay) fmtDateTime.format(Date(ms)) else fmtTime.format(Date(ms))
        val ly = when {
            top -> y + sp(11f)
            else -> y - dp(3f)
        }
        txt.isFakeBoldText = isEdge
        c.drawText(timeStr, bandW + dp(3f), ly, txt)
        c.drawText("%.0f°".format(alt), bandW + dp(3f), ly + sp(11f), txt)
        txt.isFakeBoldText = false
    }

    // ── タッチ(境目=縦ドラッグ / 帯=横ドラッグで挿入排除 / タップ / 縦スワイプは親へ) ──
    private val DRAG_NONE = 0; private val DRAG_UNDECIDED = 1; private val DRAG_V = 2; private val DRAG_H = 3; private val DRAG_SCROLL = 4
    private var dragMode = DRAG_NONE
    private var dragBoundary = -1
    private var dragWin = -1
    private var downX = 0f; private var downY = 0f
    private var dragY = 0f

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; downY = e.y; dragY = e.y
                dragBoundary = -1; dragWin = -1
                // 境目ヒット(中/右列、±dp12)→ 縦ドラッグ。親のスクロールを止める。
                if (e.x > bandW) {
                    for (i in 0 until wins.size - 1) {
                        if (abs(e.y - yOf(wins[i].endMs)) <= dp(12f)) { dragBoundary = i; break }
                    }
                }
                if (dragBoundary >= 0) { dragMode = DRAG_V; parent?.requestDisallowInterceptTouchEvent(true); invalidate(); return true }
                // 帯上(右列)→ 方向未確定(横=挿入排除 / 縦=親スクロール / 微動=タップ)。
                // ここでは親スクロールを止めない(縦スワイプはページスクロールへ委ねる)。
                if (e.x >= ccmX0) {
                    for (i in wins.indices) { if (e.y >= yOf(wins[i].startMs) && e.y < yOf(wins[i].endMs)) { dragWin = i; break } }
                    dragMode = DRAG_UNDECIDED
                    return true
                }
                dragMode = DRAG_NONE
                return false   // 左の高度帯/ラベル列はタッチを取らない(親スクロール優先)
            }
            MotionEvent.ACTION_MOVE -> {
                when (dragMode) {
                    DRAG_V -> { dragY = e.y.coerceIn(yOf(wins[dragBoundary].startMs), yOf(wins[dragBoundary + 1].endMs)); invalidate() }
                    DRAG_UNDECIDED -> {
                        val dx = abs(e.x - downX); val dy = abs(e.y - downY)
                        if (dx > dp(14f) && dx > dy) { dragMode = DRAG_H; parent?.requestDisallowInterceptTouchEvent(true) }
                        else if (dy > dp(10f)) { dragMode = DRAG_SCROLL; parent?.requestDisallowInterceptTouchEvent(false) }  // 縦=親スクロールへ委譲
                    }
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (e.actionMasked == MotionEvent.ACTION_UP) {
                    when (dragMode) {
                        DRAG_V -> {
                            val i = dragBoundary
                            if (i in 0 until wins.size - 1) {
                                val before = wins[i].type; val after = wins[i + 1].type
                                var occ = 0
                                for (k in 0 until i) if (wins[k].type == before && wins[k + 1].type == after) occ++
                                val lo = wins[i].startMs + 60000L; val hi = wins[i + 1].endMs - 60000L
                                val ms = msOf(dragY).coerceIn(lo, hi)
                                onMoveBoundary?.invoke(before, after, occ, ms)
                            }
                        }
                        DRAG_H -> {
                            val i = dragWin
                            if (i in wins.indices) {
                                val w = wins[i]
                                val rising = w.endAlt > w.startAlt
                                val dirRight = e.x - downX > 0
                                if ((w.type == 2 || w.type == 3) && dirRight) { onSetBand?.invoke(rising, false) }       // 朝日/夕日を右へ=排除
                                else if (w.type == 4 && !dirRight && candidate(w)) { onSetBand?.invoke(rising, true) }   // 日中を左へ=挿入
                            }
                        }
                        DRAG_UNDECIDED -> { val i = dragWin; if (i in wins.indices) onTapType?.invoke(wins[i].type) }  // 微動=タップ
                    }
                }
                dragMode = DRAG_NONE; dragBoundary = -1; invalidate()
                return true
            }
        }
        return super.onTouchEvent(e)
    }

    // 日中窓が薄明帯(高度 -6..+12)に隣接=朝日/夕日を挿入できる候補か。
    private fun candidate(w: Win): Boolean {
        val lo = minOf(w.startAlt, w.endAlt); val hi = maxOf(w.startAlt, w.endAlt)
        return lo <= 12.0 && hi >= -6.0
    }
}
