package app.laxei.holygrail

// 撮影計画スケジュール(仕様書10 §7.3.2)。1インスタンス=1薄明ブロック=1ページ(横スライドで切替)。
// 太陽高度軸(+6°〜-24°)で薄明帯・境目時刻・撮影制御方法(使用する/使用しない)を表示する。
//  左=高度目盛+薄明帯、中=境目の時刻/太陽高度(開始/終了/月出入り)、右=撮影制御方法の2列。
// 操作(すべて1本指。縦スクロールはしない=ページ内に収める):
//  ・境目付近の縦スライド=境目を上下に動かして時刻(=太陽高度)を変更(onMoveBoundary)
//  ・朝日/夕日の帯の上での横スライド=左へ挿入(戻す)/右へ排除(onSetBand)。排除した箱(使用しない列)も左で戻せる
//  ・上記以外の横スライド=ページ移動(親 PlanPager へ委譲)
//  ・タップ=その撮影制御方法の編集へ(onTapType)

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import kotlin.math.abs

class ScheduleView(context: Context) : View(context) {

    data class Seg(val type: Int, val name: String?, val altTop: Double, val altBottom: Double,
                   val used: Boolean, val color: Int, val textColor: Int)
    data class Mark(val label: String, val time: String, val alt: Double)
    data class Block(val title: String, val axisDown: Boolean, val date: String,
                     val segs: List<Seg>, val marks: List<Mark>)

    private var blocks: List<Block> = emptyList()

    var onTapType: ((Int) -> Unit)? = null
    var onMoveBoundary: ((before: Int, after: Int, occ: Int, altDeg: Double, rising: Int) -> Unit)? = null
    var onSetBand: ((rising: Boolean, insert: Boolean) -> Unit)? = null

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity
    private val slop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()
    // ドラッグ(境目移動/朝夕帯/ページ送り)と判定する移動量。指タップの微妙なぶれで
    // 撮影制御方法タップが奪われないよう、素のタッチスロップより大きめにする。
    private val dragSlop get() = dp(16f)

    private val TOP = 6.0; private val BOT = -24.0; private val RANGE = TOP - BOT  // 30°
    private val headerH get() = dp(34f)
    private val bottomPad get() = dp(20f)
    private var fillH = dp(500f)
    // 1ページ=1ブロックを縦いっぱいに収める(縦スクロールしないため画面高から算出)。
    private val blockH get() = (fillH - headerH - bottomPad).coerceAtLeast(dp(160f))
    private val axisLabelW get() = dp(26f)
    private val bandW get() = dp(40f)
    private val markW get() = dp(94f)

    private val zoneFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val segFill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1f); color = 0x66607D8B }
    private val degTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF607D8B.toInt(); textSize = sp(9f); textAlign = Paint.Align.RIGHT }
    private val zoneTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = sp(9f); textAlign = Paint.Align.CENTER }
    private val markTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF263238.toInt(); textSize = sp(10f) }
    private val nameTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER; textSize = sp(11f); isFakeBoldText = true }
    private val titleTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF37474F.toInt(); textSize = sp(12f); isFakeBoldText = true }
    private val colHdr = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF607D8B.toInt(); textSize = sp(10f); textAlign = Paint.Align.CENTER }
    private val badgeFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF66BB6A.toInt() }
    private val badgeTxt = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFFFFFFFF.toInt(); textSize = sp(11f); textAlign = Paint.Align.CENTER; isFakeBoldText = true }

    private data class Zone(val top: Double, val bottom: Double, val label: String, val color: Int)
    private val zones = listOf(
        Zone(6.0, 0.0, "昼", 0xFFCFE8FF.toInt()),
        Zone(0.0, -6.0, "市民薄明", 0xFFAEC6E0.toInt()),
        Zone(-6.0, -12.0, "航海薄明", 0xFF6E7FA0.toInt()),
        Zone(-12.0, -18.0, "天文薄明", 0xFF3C4A66.toInt()),
        Zone(-18.0, -24.0, "夜", 0xFF1A1F33.toInt()))

    fun setData(blocks: List<Block>) { this.blocks = blocks; invalidate() }

    override fun onMeasure(wSpec: Int, hSpec: Int) {
        val w = MeasureSpec.getSize(wSpec)
        val h = MeasureSpec.getSize(hSpec)
        fillH = h.toFloat()
        setMeasuredDimension(w, h)
    }

    private fun blockTop(bi: Int) = bi * (headerH + blockH)
    private fun bodyTop(bi: Int) = blockTop(bi) + headerH
    private fun yOf(bi: Int, alt: Double): Float {
        val b = blocks[bi]; val bt = bodyTop(bi)
        val f = if (b.axisDown) (TOP - alt) / RANGE else (alt - BOT) / RANGE
        return bt + (f * blockH).toFloat()
    }
    private fun altOfY(bi: Int, y: Float): Double {
        val b = blocks[bi]; val f = ((y - bodyTop(bi)) / blockH).toDouble().coerceIn(0.0, 1.0)
        return if (b.axisDown) TOP - f * RANGE else BOT + f * RANGE
    }

    private fun ccmX0() = axisLabelW + bandW + markW
    private fun usedX1() = ccmX0() + (width - ccmX0()) * 0.6f

    private val previewPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(2f); color = 0xFFD32F2F.toInt() }

    override fun onDraw(c: Canvas) {
        super.onDraw(c)
        if (blocks.isEmpty() || width == 0) return
        for (bi in blocks.indices) drawBlock(c, bi)
        if (previewY >= 0f) {
            c.drawLine(axisLabelW + bandW, previewY, width.toFloat(), previewY, previewPaint)
            val bi = targetBi
            if (bi >= 0) { markTxt.color = 0xFFD32F2F.toInt(); c.drawText("%.1f°".format(altOfY(bi, previewY)), axisLabelW + bandW + dp(2f), previewY - dp(3f), markTxt); markTxt.color = 0xFF263238.toInt() }
        }
    }

    private fun drawBlock(c: Canvas, bi: Int) {
        val blk = blocks[bi]
        val hTop = blockTop(bi)
        c.drawText(blk.title, dp(2f), hTop + sp(14f), titleTxt)
        if (blk.date.isNotEmpty()) {
            val cx = (axisLabelW + bandW + markW) / 2f + dp(20f)
            val tw = badgeTxt.measureText(blk.date)
            val r = RectF(cx - tw / 2 - dp(8f), hTop + dp(3f), cx + tw / 2 + dp(8f), hTop + dp(21f))
            c.drawRoundRect(r, dp(9f), dp(9f), badgeFill)
            c.drawText(blk.date, cx, hTop + dp(16f), badgeTxt)
        }
        c.drawText("使用する", (ccmX0() + usedX1()) / 2f, hTop + sp(13f), colHdr)
        c.drawText("使用しない", (usedX1() + width) / 2f, hTop + sp(13f), colHdr)

        val bandX0 = axisLabelW
        for (z in zones) {
            val yt = yOf(bi, z.top); val yb = yOf(bi, z.bottom)
            val top = minOf(yt, yb); val bot = maxOf(yt, yb)
            zoneFill.color = z.color
            c.drawRect(bandX0, top, bandX0 + bandW, bot, zoneFill)
            if (bot - top >= sp(12f)) {
                zoneTxt.color = if (z.color and 0xFF < 0x60) 0xFFFFFFFF.toInt() else 0xFF263238.toInt()
                c.drawText(z.label, bandX0 + bandW / 2f, (top + bot) / 2f + sp(3f), zoneTxt)
            }
        }
        for (deg in listOf(6, 0, -6, -12, -18, -24)) {
            val y = yOf(bi, deg.toDouble())
            c.drawText(if (deg >= 0) "+$deg°" else "$deg°", axisLabelW - dp(2f), y + sp(3f), degTxt)
        }

        for (s in blk.segs) {
            val yt = yOf(bi, s.altTop); val yb = yOf(bi, s.altBottom)
            val top = minOf(yt, yb); val bot = maxOf(yt, yb)
            if (bot - top <= 0f) continue
            val x0 = if (s.used) ccmX0() else usedX1()
            val x1 = if (s.used) usedX1() else width.toFloat()
            if (s.color != 0) { segFill.color = s.color; c.drawRect(x0, top, x1, bot, segFill) }
            val nm = s.name
            if (nm != null && bot - top >= nameTxt.textSize * 1.1f) {
                nameTxt.color = s.textColor
                c.drawText(nm, (x0 + x1) / 2f, (top + bot) / 2f - (nameTxt.descent() + nameTxt.ascent()) / 2f, nameTxt)
            }
        }

        val markX0 = axisLabelW + bandW
        for (m in blk.marks) {
            val inRange = m.alt <= TOP + 0.01 && m.alt >= BOT - 0.01
            if (inRange) {
                val y = yOf(bi, m.alt)
                c.drawLine(bandX0 + bandW, y, width.toFloat(), y, line)
                val right = if (m.label.isNotEmpty()) m.label else "%.1f°".format(m.alt)
                c.drawText("${m.time}  $right", markX0 + dp(2f), y + sp(11f), markTxt)
            } else {
                val above = yOf(bi, m.alt) < bodyTop(bi)
                val y = if (above) bodyTop(bi) - dp(3f) else bodyTop(bi) + blockH + sp(11f)
                val label = if (m.label.isNotEmpty()) m.label else "%.1f°".format(m.alt)
                c.drawText("${m.time}  $label", markX0 + dp(2f), y, markTxt)
            }
        }
    }

    // ── タッチ(すべて1本指。境目=縦 / 朝夕帯=横 / それ以外の横=ページ移動へ委譲) ──
    private var axisLocked = false
    private var mode = 0            // 0=未確定/無効, 1=境目移動(縦), 2=挿入排除(横), 3=ページ委譲(横)
    private var downX = 0f; private var downY = 0f
    private var previewY = -1f
    private var targetBi = -1; private var targetBoundary = -1; private var targetSeg = -1

    private data class Bnd(val bi: Int, val before: Int, val after: Int, val occ: Int, val alt: Double, val y: Float)
    private fun boundaries(): List<Bnd> {
        val res = ArrayList<Bnd>(); val cnt = HashMap<Int, Int>()
        for (bi in blocks.indices) {
            val u = blocks[bi].segs.filter { it.used }
            for (k in 0 until u.size - 1) {
                val bef = u[k].type; val aft = u[k + 1].type
                val key = bef * 100 + aft; val occ = cnt.getOrDefault(key, 0); cnt[key] = occ + 1
                val alt = if (blocks[bi].axisDown) u[k].altBottom else u[k].altTop
                res.add(Bnd(bi, bef, aft, occ, alt, yOf(bi, alt)))
            }
        }
        return res
    }
    // 撮影制御方法移動範囲(仕様 7.3.2)。ネイティブ側 hge_setBoundaryByAlt と同じ表にすること。
    // 【2026-08-17 修正】種別番号がずれていた。ccmType は
    //  invalid=0 night=1 sunrise=2 sunset=3 day=4 **preNight=5 postNight=6**。
    //  ここは preNight=6 postNight=7 のつもりで書かれていたため、夕日↔夜間前移行と
    //  夜間前移行↔夜間が既定(-24〜+6)に落ちて事実上チェックされていなかった(夕方だけ効かない)。
    private object CcmT { const val NIGHT = 1; const val SUNRISE = 2; const val SUNSET = 3
                          const val DAY = 4; const val PRE_NIGHT = 5; const val POST_NIGHT = 6 }
    private fun clampRange(before: Int, after: Int): Pair<Double, Double> {
        fun has(t: Int) = before == t || after == t
        val sun   = has(CcmT.SUNRISE) || has(CcmT.SUNSET)
        val trans = has(CcmT.PRE_NIGHT) || has(CcmT.POST_NIGHT)
        return when {
            has(CcmT.DAY) && sun -> 3.0 to 6.0
            sun && trans -> -1.0 to 2.0
            has(CcmT.DAY) && trans -> -3.0 to 4.0
            has(CcmT.NIGHT) && trans -> -19.0 to -12.0
            else -> -24.0 to 6.0
        }
    }
    private fun blockAt(y: Float): Int { for (bi in blocks.indices) { if (y >= blockTop(bi) && y < blockTop(bi) + headerH + blockH) return bi }; return -1 }
    private fun segAtXY(bi: Int, x: Float, y: Float): Int {
        val b = blocks[bi]
        for (i in b.segs.indices) {
            val s = b.segs[i]
            val yt = yOf(bi, s.altTop); val yb = yOf(bi, s.altBottom)
            val top = minOf(yt, yb); val bot = maxOf(yt, yb)
            if (bot - top <= 0f) continue
            val x0 = if (s.used) ccmX0() else usedX1()
            val x1 = if (s.used) usedX1() else width.toFloat()
            if (x >= x0 && x < x1 && y >= top && y < bot) return i
        }
        return -1
    }
    // 最寄りの境目(downY から dp22 以内)。無ければ -1。
    private fun nearestBoundary(bi: Int, y: Float): Int {
        var best = -1; var bestD = dp(22f); val bs = boundaries()
        for (idx in bs.indices) { if (bs[idx].bi == bi) { val d = abs(bs[idx].y - y); if (d < bestD) { bestD = d; best = idx } } }
        return best
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (!isEnabled) return false   // 撮影中(読取専用)は編集不可→ページャの横スワイプに委ねる
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; downY = e.y; axisLocked = false; mode = 0; previewY = -1f
                targetBi = blockAt(e.y); targetBoundary = -1; targetSeg = -1
                parent?.requestDisallowInterceptTouchEvent(true)   // まず掴む(1本指の意図が定まるまで)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = e.x - downX; val dy = e.y - downY
                if (!axisLocked) {
                    if (abs(dx) < dragSlop && abs(dy) < dragSlop) return true
                    axisLocked = true
                    if (abs(dy) >= abs(dx)) {
                        // 縦=境目移動。最寄り境目を掴む。無ければ無効(ページ移動もしない)。
                        mode = 1
                        targetBoundary = if (targetBi >= 0) nearestBoundary(targetBi, downY) else -1
                        if (targetBoundary < 0) mode = 0
                    } else {
                        // 横=朝夕帯なら挿入排除、それ以外はページ移動へ委譲。
                        val si = if (targetBi >= 0) segAtXY(targetBi, downX, downY) else -1
                        val ty = if (si >= 0) blocks[targetBi].segs[si].type else 0
                        if (si >= 0 && (ty == 2 || ty == 3 || ty == 4)) { mode = 2; targetSeg = si }
                        else { mode = 3; parent?.requestDisallowInterceptTouchEvent(false) }  // ページャが横スワイプを引き取る
                    }
                }
                if (mode == 3) return true   // 親が intercept するまで保持
                if (mode == 1 && targetBoundary >= 0) {
                    val bnd = boundaries().getOrNull(targetBoundary)
                    if (bnd != null) {
                        val (lo, hi) = clampRange(bnd.before, bnd.after)
                        val a = altOfY(bnd.bi, e.y).coerceIn(lo, hi)
                        previewY = yOf(bnd.bi, a); invalidate()
                    }
                }
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (e.actionMasked == MotionEvent.ACTION_UP && !axisLocked) {
                    // タップ=撮影制御方法の編集(色付き箱の中のみ)。
                    val bi = blockAt(e.y)
                    if (bi >= 0) { val si = segAtXY(bi, e.x, e.y); if (si >= 0) onTapType?.invoke(blocks[bi].segs[si].type) }
                } else if (e.actionMasked == MotionEvent.ACTION_UP && mode == 1 && targetBoundary >= 0) {
                    val bnd = boundaries().getOrNull(targetBoundary)
                    if (bnd != null) {
                        val (lo, hi) = clampRange(bnd.before, bnd.after)
                        val a = altOfY(bnd.bi, e.y).coerceIn(lo, hi)
                        val rising = if (blocks[bnd.bi].axisDown) 0 else 1
                        onMoveBoundary?.invoke(bnd.before, bnd.after, bnd.occ, a, rising)
                    }
                } else if (e.actionMasked == MotionEvent.ACTION_UP && mode == 2 && targetSeg >= 0) {
                    val s = blocks[targetBi].segs.getOrNull(targetSeg)
                    val dx = e.x - downX
                    if (s != null && abs(dx) > slop) {
                        val rising = !blocks[targetBi].axisDown
                        val right = dx > 0
                        // 朝日/夕日の帯: 右へ=「使用しない」へ(排除) / 左へ=「使用する」へ(挿入・戻す)。
                        // 排除した帯(使用しない列の箱)を左へドラッグすればスケジュールへ戻せる。
                        if (s.type == 2 || s.type == 3) onSetBand?.invoke(rising, !right)
                        // 日中の帯を左へドラッグでも朝日/夕日を挿入(戻す)できる。
                        else if (s.type == 4 && !right) onSetBand?.invoke(rising, true)
                    }
                }
                mode = 0; previewY = -1f; targetBoundary = -1; targetSeg = -1; axisLocked = false; invalidate()
                return true
            }
        }
        return super.onTouchEvent(e)
    }
}
