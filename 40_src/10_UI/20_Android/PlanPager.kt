package app.laxei.holygrail

// 撮影計画画面の横スライドページャ(§7.3 画面分割)。
//  ・ページ0 = 先頭フォーム(XMLの子。リスト+概要+編集項目。縦スクロール可)
//  ・ページ1.. = 薄明スケジュール(ScheduleView。1ブロック=1ページ。縦スクロールなし)
// 子が requestDisallowInterceptTouchEvent(true) を出している間は横スワイプを奪わない。
// 薄明ページの ScheduleView は境目の上下移動/朝夕の挿入排除を自分で処理し、それ以外の
// 横移動では委譲を外す(false)ので、その瞬間から本ページャが横スワイプを引き取る。
import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.VelocityTracker
import android.view.ViewConfiguration
import android.widget.FrameLayout
import kotlin.math.abs

class PlanPager @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null)
    : FrameLayout(context, attrs) {

    var onPageChanged: ((Int) -> Unit)? = null
    var current = 0
        private set

    private val slop = ViewConfiguration.get(context).scaledTouchSlop
    private var downX = 0f
    private var downY = 0f
    private var dragging = false
    private var vt: VelocityTracker? = null

    val pageCount get() = childCount

    // 子を現在ページ基準で横に並べる(dx=ドラッグ中の追従量)。表示は現在±1のみ。
    private fun layoutPages(dx: Float) {
        val w = width
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            c.translationX = (i - current) * w + dx
            c.visibility = if (abs(i - current) <= 1) VISIBLE else GONE
        }
    }

    // ページ構成が変わった後に呼ぶ(現在ページを範囲内へ収め再配置)。
    fun refreshPages() {
        if (current > childCount - 1) current = (childCount - 1).coerceAtLeast(0)
        post { layoutPages(0f) }
    }

    fun setCurrent(idx: Int, animate: Boolean) {
        val t = idx.coerceIn(0, (childCount - 1).coerceAtLeast(0))
        if (animate) animateTo(t) else { current = t; layoutPages(0f); onPageChanged?.invoke(current) }
    }

    override fun onInterceptTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y; dragging = false }
            MotionEvent.ACTION_MOVE -> {
                val dx = e.x - downX; val dy = e.y - downY
                if (!dragging && childCount > 1 && abs(dx) > slop && abs(dx) > abs(dy) * 1.2f) {
                    dragging = true; startDrag(); return true
                }
            }
        }
        return false
    }

    private fun startDrag() {
        parent?.requestDisallowInterceptTouchEvent(true)
        vt = VelocityTracker.obtain()
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> { downX = e.x; downY = e.y; return true }
            MotionEvent.ACTION_MOVE -> {
                if (!dragging) {
                    val dx = e.x - downX; val dy = e.y - downY
                    if (childCount > 1 && abs(dx) > slop && abs(dx) > abs(dy) * 1.2f) { dragging = true; startDrag() }
                    else return true
                }
                vt?.addMovement(e)
                var dx = e.x - downX
                if ((current == 0 && dx > 0) || (current == childCount - 1 && dx < 0)) dx *= 0.35f  // 端で抵抗
                layoutPages(dx)
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                if (dragging) {
                    val dx = e.x - downX
                    vt?.addMovement(e); vt?.computeCurrentVelocity(1000)
                    val vx = vt?.xVelocity ?: 0f
                    var target = current
                    if ((dx < -width / 4f || vx < -800f) && current < childCount - 1) target = current + 1
                    else if ((dx > width / 4f || vx > 800f) && current > 0) target = current - 1
                    animateTo(target)
                }
                vt?.recycle(); vt = null; dragging = false
                return true
            }
        }
        return false
    }

    private fun animateTo(target: Int) {
        val changed = target != current
        current = target
        val w = width
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (abs(i - current) <= 1) c.visibility = VISIBLE
            c.animate().translationX((i - current) * w.toFloat()).setDuration(180).withEndAction {
                if (abs(i - current) > 1) c.visibility = GONE
            }.start()
        }
        if (changed) onPageChanged?.invoke(current)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        super.onLayout(changed, l, t, r, b)
        if (!dragging) layoutPages(0f)
    }
}
