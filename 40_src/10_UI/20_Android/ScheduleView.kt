package app.laxei.holygrail

// 撮影計画スケジュール(仕様書10 §7.3.2 / 330.撮影計画設定.png に忠実)。
// 太陽高度軸(+6°〜-24°)で「夕方の計画」「朝の計画」を別ブロックに分けて表示する。
//  各ブロック: 左=高度目盛+薄明帯(昼/市民/航海/天文/夜)、中=境目の時刻・太陽高度(開始/終了/月出入り)、
//             右=撮影制御方法を「使用する/使用しない」の2列(排除した夕日/朝日は使用しない側)。
// 操作(スクロールとの衝突回避のため編集は2本指):
//  ・2本指の縦ドラッグ=境目を上下に動かして時刻(=太陽高度)を変更(onMoveBoundary)
//  ・2本指の横ドラッグ=夕日/朝日の挿入(左)・排除(右)(onSetBand)
//  ・タップ=その撮影制御方法の編集へ(onTapType)
//  ・1本指の移動=ページスクロール(親へ委譲)。その際 onNeedTwoFinger で「2本指で」を促す。

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View
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
    var onNeedTwoFinger: (() -> Unit)? = null

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    private val TOP = 6.0; private val BOT = -24.0; private val RANGE = TOP - BOT  // 30°
    private val headerH get() = dp(34f)
    private val blockH get() = dp(450f)   // 指操作しやすいよう縦を拡大(従来比1.5倍)
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

    // 薄明帯(高度→色/ラベル)。明るい昼→濃い夜。
    private data class Zone(val top: Double, val bottom: Double, val label: String, val color: Int)
    private val zones = listOf(
        Zone(6.0, 0.0, "昼", 0xFFCFE8FF.toInt()),
        Zone(0.0, -6.0, "市民薄明", 0xFFAEC6E0.toInt()),
        Zone(-6.0, -12.0, "航海薄明", 0xFF6E7FA0.toInt()),
        Zone(-12.0, -18.0, "天文薄明", 0xFF3C4A66.toInt()),
        Zone(-18.0, -24.0, "夜", 0xFF1A1F33.toInt()))

    fun setData(blocks: List<Block>) { this.blocks = blocks; requestLayout(); invalidate() }

    override fun onMeasure(wSpec: Int, hSpec: Int) {
        val w = MeasureSpec.getSize(wSpec)
        val h = (blocks.size.coerceAtLeast(1) * (headerH + blockH)).toInt()
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
        // 2本指縦ドラッグ中の境目プレビュー線
        if (previewY >= 0f) {
            c.drawLine(axisLabelW + bandW, previewY, width.toFloat(), previewY, previewPaint)
            val bi = targetBi
            if (bi >= 0) { markTxt.color = 0xFFD32F2F.toInt(); c.drawText("%.1f°".format(altOfY(bi, previewY)), axisLabelW + bandW + dp(2f), previewY - dp(3f), markTxt); markTxt.color = 0xFF263238.toInt() }
        }
    }

    private fun drawBlock(c: Canvas, bi: Int) {
        val blk = blocks[bi]
        val hTop = blockTop(bi)
        // ヘッダ: タイトル / 日付バッジ / 列見出し
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

        // 薄明帯 + 高度目盛
        val bandX0 = axisLabelW
        for (z in zones) {
            val yt = yOf(bi, z.top); val yb = yOf(bi, z.bottom)
            val top = minOf(yt, yb); val bot = maxOf(yt, yb)
            zoneFill.color = z.color
            c.drawRect(bandX0, top, bandX0 + bandW, bot, zoneFill)
            if (bot - top >= sp(12f)) {
                zoneTxt.color = if (z.color and 0xFF < 0x60) 0xFFFFFFFF.toInt() else 0xFF263238.toInt()
                // 縦書き風に1文字ずつは複雑なので横書きを中央に
                c.drawText(z.label, bandX0 + bandW / 2f, (top + bot) / 2f + sp(3f), zoneTxt)
            }
        }
        for (deg in listOf(6, 0, -6, -12, -18, -24)) {
            val y = yOf(bi, deg.toDouble())
            c.drawText(if (deg >= 0) "+$deg°" else "$deg°", axisLabelW - dp(2f), y + sp(3f), degTxt)
        }

        // 撮影制御方法バンド(使用する=左列 / 使用しない=右列)
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

        // 境目の時刻・太陽高度 / 開始終了 / 月出入り(中列)
        val markX0 = axisLabelW + bandW
        for (m in blk.marks) {
            val y = yOf(bi, m.alt)
            c.drawLine(bandX0 + bandW, y, width.toFloat(), y, line)
            val right = if (m.label.isNotEmpty()) m.label else "%.1f°".format(m.alt)
            c.drawText("${m.time}  $right", markX0 + dp(2f), y + sp(11f), markTxt)
        }
    }

    // ── タッチ(2本指=編集 / 1本指=スクロール+ヒント) ──
    private var twoFinger = false
    private var hintShown = false
    private var downX = 0f; private var downY = 0f
    private var startMidX = 0f; private var startMidY = 0f
    private var curMidX = 0f; private var curMidY = 0f
    private var previewY = -1f   // 2本指縦ドラッグ中の境目プレビュー
    private var targetBi = -1; private var targetBoundary = -1; private var targetSeg = -1

    private fun midX(e: MotionEvent) = if (e.pointerCount >= 2) (e.getX(0) + e.getX(1)) / 2f else e.x
    private fun midY(e: MotionEvent) = if (e.pointerCount >= 2) (e.getY(0) + e.getY(1)) / 2f else e.y

    // 全ブロックの「使用する」セグメント境目を ccmList 順(=時系列)で列挙し occ を付与。
    private data class Bnd(val bi: Int, val before: Int, val after: Int, val occ: Int, val alt: Double, val y: Float)
    private fun boundaries(): List<Bnd> {
        val res = ArrayList<Bnd>(); val cnt = HashMap<Int, Int>()
        for (bi in blocks.indices) {
            val u = blocks[bi].segs.filter { it.used }
            for (k in 0 until u.size - 1) {
                val bef = u[k].type; val aft = u[k + 1].type
                val key = bef * 100 + aft; val occ = cnt.getOrDefault(key, 0); cnt[key] = occ + 1
                val alt = u[k].altBottom
                res.add(Bnd(bi, bef, aft, occ, alt, yOf(bi, alt)))
            }
        }
        return res
    }
    private fun blockAt(y: Float): Int { for (bi in blocks.indices) { if (y >= blockTop(bi) && y < blockTop(bi) + headerH + blockH) return bi }; return -1 }
    private fun segAt(bi: Int, y: Float): Int {
        val b = blocks[bi]
        for (i in b.segs.indices) { val s = b.segs[i]; val yt = yOf(bi, s.altTop); val yb = yOf(bi, s.altBottom); if (y >= minOf(yt, yb) && y < maxOf(yt, yb)) return i }
        return -1
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                downX = e.x; downY = e.y; curMidX = e.x; curMidY = e.y
                twoFinger = false; hintShown = false; previewY = -1f
                targetBi = -1; targetBoundary = -1; targetSeg = -1
                // まずジェスチャを保持(2本指を確実に拾う)。1本指で動いたら親へ返す。
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_POINTER_DOWN -> {
                if (e.pointerCount >= 2) {
                    twoFinger = true
                    parent?.requestDisallowInterceptTouchEvent(true)
                    startMidX = midX(e); startMidY = midY(e); curMidX = startMidX; curMidY = startMidY
                    targetBi = blockAt(startMidY)
                    // 最寄りの境目を掴む(距離制限なし=近いものを選ぶ)。
                    targetBoundary = -1; targetSeg = -1
                    if (targetBi >= 0) {
                        var best = -1; var bestD = Float.MAX_VALUE; val bs = boundaries()
                        for (idx in bs.indices) { if (bs[idx].bi == targetBi) { val d = abs(bs[idx].y - startMidY); if (d < bestD) { bestD = d; best = idx } } }
                        targetBoundary = best
                        targetSeg = segAt(targetBi, startMidY)
                    }
                }
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (twoFinger) {
                    if (e.pointerCount >= 2) { curMidX = midX(e); curMidY = midY(e) }
                    // 縦移動が優勢なら境目プレビュー線を出す
                    if (targetBoundary >= 0 && abs(curMidY - startMidY) >= abs(curMidX - startMidX)) { previewY = curMidY; invalidate() }
                    return true
                }
                // 1本指で動いたらヒント+親スクロールへ委譲
                if (abs(e.x - downX) > dp(10f) || abs(e.y - downY) > dp(10f)) {
                    if (!hintShown) { hintShown = true; onNeedTwoFinger?.invoke() }
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
                return true
            }
            MotionEvent.ACTION_POINTER_UP -> { return true }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (e.actionMasked == MotionEvent.ACTION_UP && twoFinger) {
                    commitEdit()
                } else if (e.actionMasked == MotionEvent.ACTION_UP && !twoFinger &&
                           abs(e.x - downX) < dp(10f) && abs(e.y - downY) < dp(10f)) {
                    val bi = blockAt(e.y); if (bi >= 0) { val si = segAt(bi, e.y); if (si >= 0) onTapType?.invoke(blocks[bi].segs[si].type) }
                }
                twoFinger = false; previewY = -1f; invalidate()
                return true
            }
        }
        return super.onTouchEvent(e)
    }

    private fun commitEdit() {
        val bi = targetBi
        if (bi < 0) return
        val mvx = curMidX - startMidX; val mvy = curMidY - startMidY
        if (targetBoundary >= 0 && abs(mvy) >= abs(mvx) && abs(mvy) > dp(8f)) {
            // 縦ドラッグ=境目の時刻(=高度)変更
            val bnd = boundaries().getOrNull(targetBoundary) ?: return
            val newAlt = altOfY(bi, curMidY)
            val rising = if (blocks[bi].axisDown) 0 else 1
            onMoveBoundary?.invoke(bnd.before, bnd.after, bnd.occ, newAlt, rising)
        } else if (targetSeg >= 0 && abs(mvx) > abs(mvy) && abs(mvx) > dp(20f)) {
            // 横ドラッグ=夕日/朝日の挿入(左)/排除(右)
            val s = blocks[bi].segs.getOrNull(targetSeg) ?: return
            val rising = !blocks[bi].axisDown
            val right = mvx > 0
            if ((s.type == 2 || s.type == 3) && right) onSetBand?.invoke(rising, false)
            else if (s.type == 4 && !right) onSetBand?.invoke(rising, true)
        }
    }
}
