package app.laxei.twylapse

// 撮影シミュレーション(§7.3 画面360)。撮影計画ページャの最後のページ。
//  ・撮影方向(方位磁石)/仰角(カメラ図)を貼り付け、期間スライダーで時刻を動かす。
//  ・時刻・方向・仰角・レンズ(平面/魚眼)・センサー比から、画角内の恒星/惑星/太陽/月を
//    ネイティブ(hge_simulateSky)で投影し、上部のセンサー比の領域に描画する。
//  ・平面レンズは端ほど間延び(ノモニック)、魚眼はそのまま(等距離)投影。縦向きは描画も縦。

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executor
import kotlin.math.max
import kotlin.math.min

// 画角内に投影された天体(x,y∈[-1,1] x右+ y上+)。
internal data class SkyObj(val x: Float, val y: Float, val mag: Float, val color: Int, val kind: String, val name: String)

// ── センサー比の領域に星空を描画するビュー ───────────────────────────
// 項目12/G: 空と地面を描く。
//  ・空: 夜は「濃いグレー」(真っ黒にしない=地面と区別する)。薄明で徐々に明るくなり、日中は青空。
//  ・地面: 草原のイメージ。夜は黒、明るくなるにつれ緑になる。
//  ・地平線: 仰角と縦画角から画面内の位置を求める(画角外なら地面は出ない)。
//  ・星の固まり(星座など)の名称も表示する。
//  ・項目G: 画像枠は常に「横向き(ランドスケープ)のセンサー比」で固定し、縦向き時は枠内へ
//    レターボックスして描く。これで横向きチェックを外しても枠が拡大しない(=大きくならない)。
//    日時は画像内に描かず、ページ側の欄外テキストに出す。
class SkyRenderView(context: Context) : View(context) {
    private var objs: List<SkyObj> = emptyList()
    private var aspect = 1.5f          // 現在の内容の横/縦(縦向きだと<1)
    private var sunAlt = -90.0f        // 太陽高度[°](空/地面の色に使う)
    private var camEl = 10.0f          // 撮影仰角[°]
    private var fovV = 50.0f           // 縦画角[°]
    // 地平線(高度0°)をレンズの投影どおりに引くための点列。x昇順・[-1,1]の正規化座標。
    //  平面レンズなら直線に、魚眼なら弧になる(2026-08-30 UI依頼)。空なら従来の直線で描く。
    private var horizon: List<Pair<Float, Float>> = emptyList()
    private val magLimit = 6.5f
    // 背景(全天写真)を貼るための格子。ネイティブが返す銀河座標[°]。
    //  bgN × bgN の点を、画面左下から右上へ並べたもの。
    private var bgL = FloatArray(0)
    private var bgB = FloatArray(0)
    private var bgN = 0
    private val bgPaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var bgVerts = FloatArray(0)
    private var bgTexs = FloatArray(0)

    private val sky = Paint()
    private val ground = Paint()
    private val frame = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1f); color = 0xFF3A3F55.toInt() }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xCCFFFFFF.toInt(); textSize = sp(10f) }
    private val groupLabel = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xDD9FE8FF.toInt(); textSize = sp(12f); textAlign = Paint.Align.CENTER; isFakeBoldText = true
    }
    private val empty = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF8890A8.toInt(); textAlign = Paint.Align.CENTER; textSize = sp(12f) }

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    internal fun setData(list: List<SkyObj>, asp: Float, sunAltDeg: Float, elDeg: Float, fovVDeg: Float,
                         hz: List<Pair<Float, Float>> = emptyList(),
                         gl: FloatArray = FloatArray(0), gb: FloatArray = FloatArray(0), gn: Int = 0) {
        objs = list
        horizon = hz
        bgL = gl; bgB = gb; bgN = gn
        var flipped = false
        if (asp > 0f) {
            val wasLandscape = aspect >= 1f
            aspect = asp
            // 縦横が入れ替わると枠の高さが変わる(縦向きは背が高くなる)ので測り直す。
            if (wasLandscape != (aspect >= 1f)) flipped = true
        }
        sunAlt = sunAltDeg; camEl = elDeg; fovV = fovVDeg
        // 時刻/方向/仰角の変更では向きは変わらない=invalidate だけ(スライド追従が軽い)。
        // 横向きチェックの切替(縦↔横)の時だけ測り直す。
        if (flipped) requestLayout()
        invalidate()
    }

    // 撮影イメージの矩形。項目1: 横向きの長方形を「そのまま縦にしただけ(実寸そのまま・90°回転)」にする。
    //  長辺 = 表示幅。横向き=幅いっぱい(高さ=幅/横比)。縦向き=同じ長方形を縦にしただけ(高さ=幅・幅=幅/横比)で
    //  左右に余白。=横向きと同じ大きさの四角を縦向きにするだけ(縦向きで縮めない)。
    private fun sensorRect(): RectF {
        val pad = dp(6f)
        val w = width - pad * 2; val h = height - pad * 2
        if (w <= 0 || h <= 0) return RectF(0f, 0f, 0f, 0f)
        var rw = w.toFloat(); var rh = rw / aspect
        if (rh > h) { rh = h.toFloat(); rw = rh * aspect }
        val l = pad + (w - rw) / 2f
        val t = pad + (h - rh) / 2f
        return RectF(l, t, l + rw, t + rh)
    }

    // 枠の高さ: 横向き=幅/横比(幅いっぱい)。縦向き=表示幅(=横向きの長方形を縦にした長辺)。
    //  → 縦向きでも横向きと同じ大きさの長方形になる(項目1。縦向きだけ背が高くなる)。
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val w = MeasureSpec.getSize(widthSpec)
        val pad = dp(6f) * 2
        var availW = (w - pad).coerceAtLeast(1f)
        val a = if (aspect > 0.01f) aspect else 1.5f
        val la = max(a, 1f / a)                               // 横向き時の横比(≧1、例1.5)
        var frameH = if (a >= 1f) availW / la else availW    // 横向き=幅/横比 / 縦向き=幅(長辺を縦に)
        // 高さに上限があるとき(端末が横向きで画面の左半分に置くときなど)は、はみ出さないよう
        //  幅の方を詰める。縦向きは高さが wrap_content なのでここは通らない(2026-08-30)。
        val hMode = MeasureSpec.getMode(heightSpec)
        val hSize = MeasureSpec.getSize(heightSpec)
        if (hMode != MeasureSpec.UNSPECIFIED && frameH + pad > hSize && hSize > pad) {
            frameH = (hSize - pad).coerceAtLeast(1f)
            availW = if (a >= 1f) frameH * la else frameH
            setMeasuredDimension((availW + pad).toInt().coerceAtLeast(1), hSize)
            return
        }
        setMeasuredDimension(w, (frameH + pad).toInt().coerceAtLeast(1))
    }

    // 明るさ 0(夜)〜1(日中)。太陽高度 -18°(天文薄明の始まり)〜 +6°(日中)で滑らかに変化させる。
    private fun daylight(): Float {
        val t = (sunAlt + 18f) / 24f
        return min(1f, max(0f, t))
    }

    private fun lerp(a: Int, b: Int, t: Float): Int {
        val ar = (a shr 16) and 0xFF; val ag = (a shr 8) and 0xFF; val ab = a and 0xFF
        val br = (b shr 16) and 0xFF; val bg2 = (b shr 8) and 0xFF; val bb = b and 0xFF
        val r = (ar + (br - ar) * t).toInt(); val g = (ag + (bg2 - ag) * t).toInt(); val bl = (ab + (bb - ab) * t).toInt()
        return (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
    }

    override fun onDraw(c: Canvas) {
        val r = sensorRect()
        if (r.width() <= 0f) return
        val d = daylight()

        // 空: 夜=濃いグレー → 薄明 → 日中=青空
        sky.color = if (d < 0.5f) lerp(0xFF2B2F3A.toInt(), 0xFF6E7FA8.toInt(), d / 0.5f)  // 濃いグレー → 薄明の青灰
                    else          lerp(0xFF6E7FA8.toInt(), 0xFF6FB3E8.toInt(), (d - 0.5f) / 0.5f)  // → 青空
        c.drawRect(r, sky)

        drawSkyBackground(c, r, d)   // 全天写真を地に敷く(空のあと、地面の前)

        // 地面(草原): 夜=黒 → 明るくなると緑。
        ground.color = lerp(0xFF000000.toInt(), 0xFF3E7B3A.toInt(), d)   // 黒 → 草原の緑
        if (horizon.size >= 2) {
            // 地平線の点列(ネイティブがレンズの投影で出したもの)に沿って塗る。
            //  平面レンズなら直線、魚眼なら弧になる。線より下(=地面側)を埋める。
            fun sx(nx: Float) = r.left + (nx + 1f) / 2f * r.width()
            fun sy(ny: Float) = r.top + (1f - (ny + 1f) / 2f) * r.height()
            val p = android.graphics.Path()
            p.moveTo(r.left, sy(horizon.first().second))          // 左端は端の高さで水平に伸ばす
            for (q in horizon) p.lineTo(sx(q.first), sy(q.second))
            p.lineTo(r.right, sy(horizon.last().second))          // 右端も同様
            p.lineTo(r.right, r.bottom); p.lineTo(r.left, r.bottom); p.close()
            c.save(); c.clipRect(r); c.drawPath(p, ground); c.restore()
        } else {
            // 地平線が画角に入らない(真上/真下を向いている等)。仰角だけで塗り分ける。
            val halfV = if (fovV > 1f) fovV / 2f else 25f
            val horizonNorm = -camEl / halfV
            if (horizonNorm > -1f) {
                val hy = r.top + (1f - (horizonNorm.coerceIn(-1f, 1f) + 1f) / 2f) * r.height()
                c.drawRect(r.left, max(hy, r.top), r.right, r.bottom, ground)
            }
        }
        c.drawRect(r, frame)

        c.save(); c.clipRect(r)
        for (o in objs) {
            val sx = r.left + (o.x + 1f) / 2f * r.width()
            val sy = r.top + (1f - (o.y + 1f) / 2f) * r.height()   // y上+ → 画面は下向きなので反転
            when (o.kind) {
                "group" -> {   // 星の固まり(星座など)の名称。重心に置く。
                    c.drawText(o.name, sx, sy, groupLabel)
                }
                "sun" -> {
                    val rad = dp(13f)
                    glow.color = (o.color and 0x00FFFFFF) or 0x55000000; c.drawCircle(sx, sy, rad * 1.8f, glow)
                    dot.color = o.color; c.drawCircle(sx, sy, rad, dot)
                    drawName(c, o.name, sx, sy + rad + dp(2f))
                }
                "moon" -> {
                    val rad = dp(10f)
                    dot.color = o.color; c.drawCircle(sx, sy, rad, dot)
                    drawName(c, o.name, sx, sy + rad + dp(2f))
                }
                "planet" -> {
                    val rad = max(dp(2.5f), magToR(o.mag) * 1.2f)
                    dot.color = o.color; c.drawCircle(sx, sy, rad, dot)
                    drawName(c, o.name, sx, sy + rad + dp(2f))
                }
                else -> {  // star。空が明るいほど星は見えにくくする。
                    val rad = magToR(o.mag)
                    var a = 255f * min(1f, max(0.35f, (magLimit + 0.5f - o.mag) / (magLimit + 0.5f)))
                    a *= (1f - d * 0.9f)      // 日中はほぼ見えない
                    dot.color = (o.color and 0x00FFFFFF) or (a.toInt().coerceIn(0, 255) shl 24)
                    c.drawCircle(sx, sy, rad, dot)
                    if (o.mag < 1.6f && d < 0.6f) drawName(c, o.name, sx + rad + dp(2f), sy + dp(3f))
                }
            }
        }
        c.restore()

        if (objs.isEmpty()) {
            c.drawText(context.getString(R.string.sim_empty), r.centerX(), r.centerY(), empty)
        }
    }

    // 格子ごとに三角形2枚で全天写真を貼る。
    //  drawBitmapMesh では**逆向き**で使えない(あれは「画像の格子→画面」)ので、
    //  「画面の格子→画像」をそのまま渡せる drawVertices を使う。**1回の描画**で済む。
    //
    // 【継ぎ目(銀河経 180°)の扱い】セルの4隅で u が 0.9 と 0.1 のように分かれると、
    //  そのセルだけ画像を横断して帯が流れる。セルごとに基準を決めて ±1.0 ずらし、
    //  横を REPEAT にしてつなげる。そのため頂点は**セルごとに独立**させている。
    private fun drawSkyBackground(c: Canvas, r: RectF, d: Float) {
        val n = bgN
        if (n < 2 || bgL.size < n * n) return
        val sh = SkyBackground.shader(context) ?: return
        val sw = SkyBackground.width().toFloat()
        val shh = SkyBackground.height().toFloat()
        if (sw <= 0f || shh <= 0f) return

        val cells = (n - 1) * (n - 1)
        val need = cells * 6 * 2
        if (bgVerts.size != need) { bgVerts = FloatArray(need); bgTexs = FloatArray(need) }

        fun sx(i: Int) = r.left + (i.toFloat() / (n - 1)) * r.width()
        fun sy(j: Int) = r.top + (1f - j.toFloat() / (n - 1)) * r.height()   // y上+ → 画面は下向き
        fun uOf(k: Int) = 0.5f - bgL[k] / 360f
        fun vOf(k: Int) = 0.5f - bgB[k] / 180f

        var p = 0
        for (j in 0 until n - 1) {
            for (i in 0 until n - 1) {
                val k00 = j * n + i; val k10 = j * n + i + 1
                val k01 = (j + 1) * n + i; val k11 = (j + 1) * n + i + 1
                val u0 = uOf(k00)
                // セル内で u をつなげる(基準から ±0.5 を超えたら一周ずらす)
                fun uw(k: Int): Float {
                    var u = uOf(k)
                    while (u - u0 > 0.5f) u -= 1f
                    while (u - u0 < -0.5f) u += 1f
                    return u
                }
                val xs = floatArrayOf(sx(i), sx(i + 1), sx(i), sx(i + 1))
                val ys = floatArrayOf(sy(j), sy(j), sy(j + 1), sy(j + 1))
                val us = floatArrayOf(u0, uw(k10), uw(k01), uw(k11))
                val vs = floatArrayOf(vOf(k00), vOf(k10), vOf(k01), vOf(k11))
                //  三角形 (0,1,2) と (1,3,2)
                val order = intArrayOf(0, 1, 2, 1, 3, 2)
                for (q in order) {
                    bgVerts[p] = xs[q]; bgTexs[p] = us[q] * sw; p++
                    bgVerts[p] = ys[q]; bgTexs[p] = vs[q] * shh; p++
                }
            }
        }

        //  星の点と同じ見え方にする: 明るいときはほぼ消す。
        val a = ((1f - d * 0.9f) * 255f).toInt().coerceIn(0, 255)
        if (a <= 0) return
        bgPaint.shader = sh
        bgPaint.alpha = a
        c.save()
        c.clipRect(r)
        try {
            c.drawVertices(Canvas.VertexMode.TRIANGLES, bgVerts.size, bgVerts, 0,
                           bgTexs, 0, null, 0, null, 0, 0, bgPaint)
        } catch (_: Exception) {
            // 描けなくても星は出る。背景が無いだけ。
        }
        c.restore()
        bgPaint.shader = null
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        SkyBackground.release()     // 16MB ほど持つので、画面を離れたら放す
    }

    private fun magToR(mag: Float): Float {
        // 明るい(mag小)ほど大きく。おおよそ mag 6.5→0.8dp, -1→4.5dp。
        val v = 1.0f + (magLimit - mag) * 0.55f
        return dp(min(4.5f, max(0.8f, v)))
    }

    private fun drawName(c: Canvas, name: String, x: Float, y: Float) {
        c.drawText(name, x, y, label)
    }
}

// ── シミュレーションページ本体 ───────────────────────────────────────
//  bind(rawPlanJson) で撮影計画(nativeGetPlanJson の JSON)を読み込み初期化する。
//  onLandscape: 横向きチェックボックスの変更を撮影計画へ反映(先頭ページと同じ設定)。
//  onDirection: 撮影方向/仰角(コンパス/仰角)の確定を撮影計画へ保存(再表示・時刻変更でも保持)。
class SimPage(
    context: Context,
    private val exec: Executor,
    private val onLandscape: (Boolean) -> Unit,
    private val onDirection: (Float, Float) -> Unit
) : LinearLayout(context) {

    // タイトル行(2026-08-08 UI依頼): 1行目=撮影計画名(左詰め) / 2行目=「撮影シミュレーション」。
    // 2行目は薄明ページの見出し(ScheduleView の titleTxt = 12sp 太字)と同じ大きさに揃える。
    private val planNameView = TextView(context)
    private val titleView = TextView(context)
    // センサー/焦点距離/画角(2026-08-08 UI依頼で撮影計画1ページ目から移設)。
    private val gearView = TextView(context)
    private val compass = CompassView(context)
    private val elevationView = ElevationView(context)
    private val landscapeCheck = CheckBox(context)
    // 【画面ごとスクロールさせる(2026-10-02 ユーザー指示)】
    //  横向きで広告が出ていると高さが足りず、一番下の「横向きで撮る」に手が届かない。
    //  【fillViewport が要】これが無いと、中身が画面より小さいときに引き伸ばされず、
    //  余白(spacer の weight)が効かなくなって**縦向きの並びが上に詰まる**。
    private val scroller = android.widget.ScrollView(context).apply {
        isFillViewport = true
        isScrollbarFadingEnabled = false
    }
    // 時刻スライダー。他画面と同じ Material Slider(大きなつまみ=●)を使う。
    private val seek = com.google.android.material.slider.Slider(context)
    private val render = SkyRenderView(context)
    // センサー寸法が分からないと画角が決まらず、星を投影する場所が決められない。
    //  そのときは絵の代わりにこの文言を出す(2026-08-19)。
    private val noSensorView = TextView(context)
    // 項目G: 日時は画像内でなく欄外の下に出す(年なし)。
    private val dateLabel = TextView(context)
    // 縦向き/横向きで並べ替えるための入れ物(2026-08-30 UI依頼)。
    //  縦向き = bodyBox に縦一列 / 横向き = 左に景色イメージ・右に操作。
    private val bodyBox = LinearLayout(context)
    private val leftBox = LinearLayout(context)
    private val rightBox = LinearLayout(context)
    private val spacer = View(context)
    private val titlesRow = LinearLayout(context)
    private val dirRow = LinearLayout(context)

    // 撮影計画から読んだパラメータ
    private var lat = 0.0; private var lon = 0.0; private var altM = 0.0
    private var az = 90.0; private var el = 10.0
    private var landscape = true; private var fisheye = false
    private var focal = 50.0; private var sensorW = 36.0; private var sensorH = 24.0
    private var startMillis = 0L; private var endMillis = 0L; private var offMin = 0
    private var fraction = 0.5f     // スライダー位置(0..1)。再bind でも保持。
    private var suppress = false

    private val fmt = SimpleDateFormat("yyyy/MM/dd HH:mm:ss", Locale.US)
    // 項目G: 欄外に出す日時。年は出さない(月/日 時:分)。
    private val labelFmt = SimpleDateFormat("M/d HH:mm", Locale.JAPAN)

    private fun dp(v: Float) = (v * resources.displayMetrics.density).toInt()

    init {
        // 項目G の並び: タイトル → 撮影イメージ → 日時(欄外) →〈下部固定〉時刻スライダー → 撮影方向/仰角 → 横向きチェック。
        // 操作部はイメージの下に置き、イメージが拡大しない(枠固定)ので位置がずれない。
        orientation = VERTICAL
        setPadding(dp(12f), dp(8f), dp(12f), dp(8f))

        // ⓪ タイトル(2026-08-08 UI依頼で2行構成へ)
        //   1行目: 撮影計画名を左詰め。薄明ページの見出しと同じ見え方(15sp 太字・淡い青地)に揃える。
        //   2行目: 「撮影シミュレーション」。薄明ページの帯見出し(12sp 太字)と同じ大きさ。
        planNameView.text = context.getString(R.string.title_plan)
        planNameView.textSize = 15f
        planNameView.gravity = Gravity.START
        planNameView.setTypeface(planNameView.typeface, android.graphics.Typeface.BOLD)
        planNameView.maxLines = 1
        planNameView.ellipsize = android.text.TextUtils.TruncateAt.END
        planNameView.setPadding(dp(12f), dp(6f), dp(12f), dp(6f))
        planNameView.setBackgroundColor(0xFFE3F2FD.toInt())
        // 親の左右パディング(12dp)を打ち消して、薄明ページと同じく画面幅いっぱいの帯にする。
        addView(planNameView, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            leftMargin = -dp(12f); rightMargin = -dp(12f)
        })

        titleView.text = context.getString(R.string.sim_title)
        titleView.textSize = 12f
        titleView.gravity = Gravity.START
        titleView.setTypeface(titleView.typeface, android.graphics.Typeface.BOLD)
        titleView.setTextColor(0xFF37474F.toInt())
        titleView.setPadding(0, dp(4f), 0, dp(2f))
        addView(titleView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // ① 撮影イメージ(高さは幅と横向きセンサー比から決まる=横向き/縦向きで枠は不変)
        //    並べる場所は relayout() が決める(縦向き=縦一列 / 横向き=左半分)。

        // ①' センサー寸法が未登録のときの代替表示(絵と入れ替える)。
        noSensorView.text = context.getString(R.string.sim_no_sensor)
        noSensorView.textSize = 13f
        noSensorView.gravity = Gravity.CENTER
        noSensorView.setTextColor(0xFF888888.toInt())
        noSensorView.setPadding(dp(12f), dp(48f), dp(12f), dp(48f))
        noSensorView.visibility = View.GONE

        // ②-0 センサー/焦点距離/画角(2026-08-08 UI依頼で計画1ページ目から移設)。
        //     ここは実際に画角が見える画面なので、数値の確認もここでできるようにする。
        gearView.textSize = 12f
        gearView.setTextColor(0xFF888888.toInt())
        gearView.gravity = Gravity.CENTER
        gearView.setPadding(0, dp(2f), 0, 0)

        // ② 日時(欄外の下・年なし)
        dateLabel.textSize = 13f
        dateLabel.gravity = Gravity.CENTER
        dateLabel.setPadding(0, dp(2f), 0, dp(2f))

        // ④ 時刻スライダー(下部固定)。他の画面と同じ Material Slider。値ラベル(数字)は出さない(項目G)。
        seek.valueFrom = 0f
        seek.valueTo = 1000f
        seek.value = (fraction * 1000f).coerceIn(0f, 1000f)
        seek.labelBehavior = com.google.android.material.slider.LabelFormatter.LABEL_GONE
        // 項目G: 他画面と同じ ● つまみにする(Material3 既定の縦棒つまみでなく thumb_dot)。
        try { seek.setCustomThumbDrawable(R.drawable.thumb_dot) } catch (_: Exception) {}
        seek.setPadding(dp(16f), dp(6f), dp(16f), dp(2f))
        seek.addOnChangeListener { _, v, _ ->
            fraction = v / 1000f
            renderSky()
        }

        // ⑤ 撮影方向 / 仰角(タイトル文字つき)。ドラッグ中もイメージがリアルタイムに追従する。
        titlesRow.orientation = HORIZONTAL
        titlesRow.addView(TextView(context).apply {
            text = context.getString(R.string.sim_direction); textSize = 13f; gravity = Gravity.CENTER
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        titlesRow.addView(TextView(context).apply {
            text = context.getString(R.string.sim_elevation); textSize = 13f; gravity = Gravity.CENTER
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        dirRow.orientation = HORIZONTAL
        dirRow.addView(compass, LinearLayout.LayoutParams(0, dp(140f), 1f))
        dirRow.addView(elevationView, LinearLayout.LayoutParams(0, dp(140f), 1f))

        // 指を離した時だけでなく、動かしている最中(onChange)にも描き直す。
        // 確定(onCommit=指を離した時)にだけ撮影計画へ保存する(ドラッグ中の毎フレーム保存は避ける)。
        // これで再表示や開始/終了時刻の変更で戻ってきても向きが保持される。
        compass.onChange = { a -> az = a.toDouble(); renderSky() }
        compass.onCommit = { a -> az = a.toDouble(); renderSky(); onDirection(az.toFloat(), el.toFloat()) }
        elevationView.onChange = { e -> el = e.toDouble(); renderSky() }
        elevationView.onCommit = { e -> el = e.toDouble(); renderSky(); onDirection(az.toFloat(), el.toFloat()) }

        // ⑥ 横向きで撮る(下部固定)
        landscapeCheck.text = context.getString(R.string.plan_landscape)
        landscapeCheck.textSize = 13f
        landscapeCheck.setOnCheckedChangeListener { _, checked ->
            if (suppress) return@setOnCheckedChangeListener
            landscape = checked
            elevationView.setLandscape(checked)   // 仰角のカメラ絵を横向き/縦向きで差し替える(2026-08-08 UI依頼)
            onLandscape(checked)     // 撮影計画へ反映(先頭ページと同じ)。再生成後に再bindされる。
            renderSky()
        }

        relayout()
    }

    // 端末の向きで並べ替える(2026-08-30 UI依頼)。
    //  縦向き … 上から 撮影イメージ / 機材 / 日時 / (余白) / 時刻スライダー / 方向・仰角 / 横向きで撮る
    //  横向き … 左に撮影イメージ、右に操作(スライダー・撮影方向・仰角)
    // 部品は作り直さず付け替えるだけなので、操作中の値や設定はそのまま残る。
    private fun relayout() {
        val land = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
        for (v in listOf<View>(render, noSensorView, gearView, dateLabel, spacer, seek, titlesRow, dirRow, landscapeCheck)) {
            (v.parent as? ViewGroup)?.removeView(v)
        }
        leftBox.removeAllViews(); rightBox.removeAllViews(); bodyBox.removeAllViews()
        if (scroller.parent == null) {
            scroller.addView(bodyBox, android.widget.FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            addView(scroller, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        val mp = ViewGroup.LayoutParams.MATCH_PARENT
        val wc = ViewGroup.LayoutParams.WRAP_CONTENT
        if (land) {
            bodyBox.orientation = HORIZONTAL
            leftBox.orientation = VERTICAL; leftBox.gravity = Gravity.CENTER
            rightBox.orientation = VERTICAL
            leftBox.addView(render, LinearLayout.LayoutParams(mp, wc))
            leftBox.addView(noSensorView, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(gearView, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(dateLabel, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(spacer, LinearLayout.LayoutParams(mp, 0, 1f))
            rightBox.addView(seek, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(titlesRow, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(dirRow, LinearLayout.LayoutParams(mp, wc))
            rightBox.addView(landscapeCheck, LinearLayout.LayoutParams(wc, wc))
            // 【高さは中身に合わせる(2026-10-02)】画面の高さに固定すると、
            //  中身がそれより高いときに**切れてスクロールもできない**。
            bodyBox.gravity = Gravity.CENTER_VERTICAL
            bodyBox.addView(leftBox, LinearLayout.LayoutParams(0, wc, 1f))
            bodyBox.addView(rightBox, LinearLayout.LayoutParams(0, wc, 1f))
        } else {
            bodyBox.orientation = VERTICAL
            bodyBox.addView(render, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(noSensorView, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(gearView, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(dateLabel, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(spacer, LinearLayout.LayoutParams(mp, 0, 1f))
            bodyBox.addView(seek, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(titlesRow, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(dirRow, LinearLayout.LayoutParams(mp, wc))
            bodyBox.addView(landscapeCheck, LinearLayout.LayoutParams(wc, wc))
        }
    }

    // 回転はアクティビティが configChanges で受け止めるので、ここへ配られてくる。
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration?) {
        super.onConfigurationChanged(newConfig)
        relayout()
        renderSky()
    }

    // タイトル1行目の撮影計画名(2026-08-08 UI依頼)。薄明ページの見出しと同じ文言を出す。
    fun setPlanName(n: String) { planNameView.text = n }

    // センサー/焦点距離/画角の表示(2026-08-08 UI依頼)。撮影計画側で整形した文字列をそのまま出す。
    fun setGearText(t: String) { gearView.text = t }

    // 撮影計画(nativeGetPlanJson の JSON)を読み込み、ウィジェット初期化＆描画。
    //  masterLensesJson: マスターレンズ一覧(nativeGetMasterLenses)。起動時にアセットから再コピー
    //  されるので、lenses_list.json の "fisheye" を編集→ビルドすれば既存計画でも即反映される。
    fun bind(rawPlanJson: String, masterLensesJson: String = "[]") {
        // マスターの レンズ名→fisheye マップ(真の情報源)。
        val masterFisheye = HashMap<String, Boolean>()
        try {
            val arr = org.json.JSONArray(masterLensesJson)
            for (i in 0 until arr.length()) {
                val l = arr.getJSONObject(i)
                if (l.has("fisheye")) masterFisheye[l.optString("name")] = l.optBoolean("fisheye")
            }
        } catch (_: Exception) {}
        try {
            val o = JSONObject(rawPlanJson)
            o.optJSONObject("place")?.let {
                lat = it.optDouble("latitude", 0.0); lon = it.optDouble("longitude", 0.0); altM = it.optDouble("altitude", 0.0)
            }
            az = o.optDouble("azimuth", 90.0); el = o.optDouble("elevation", 10.0)
            landscape = o.optBoolean("landscape", true)
            o.optJSONObject("camera")?.let {
                // 未登録(0)はそのまま 0 で持つ。既定値(フルサイズ36×24)や横幅からの推定で
                // 埋めると、別のカメラの画角を本物のように見せてしまう(2026-08-19)。
                sensorW = it.optDouble("sensorSize", 0.0); sensorH = it.optDouble("sensorSizeV", 0.0)
                if (sensorW > 0.0 && sensorH <= 0.0) sensorH = sensorW * 2.0 / 3.0
            }
            o.optJSONObject("lens")?.let {
                focal = it.optDouble("focalLength", 50.0)
                val lensName = it.optString("name", "")
                // 魚眼判定は マスターの fisheye(ファイル由来・即反映)を最優先。
                fisheye = masterFisheye[lensName]
                          ?: if (it.has("fisheye")) it.optBoolean("fisheye")
                             else lensName.contains("fisheye", ignoreCase = true)
            }
            startMillis = dtToMillis(o.optJSONObject("start"))
            endMillis = dtToMillis(o.optJSONObject("end"))
            if (endMillis <= startMillis) endMillis = startMillis + 12L * 3600_000L
            offMin = TimeZone.getDefault().getOffset(startMillis) / 60000
        } catch (_: Exception) {}

        suppress = true
        landscapeCheck.isChecked = landscape
        elevationView.setLandscape(landscape)   // 仰角のカメラ絵を計画の横向き設定に合わせる(2026-08-08 UI依頼)
        compass.setAzimuth(az.toFloat())
        elevationView.setAngle(el.toFloat())
        val (fh, fv) = fovDeg()
        compass.setFov(fh.toFloat())
        elevationView.setFov(fv.toFloat())
        seek.value = (fraction * 1000f).coerceIn(seek.valueFrom, seek.valueTo)
        suppress = false
        renderSky()
    }

    // 撮影方向の方位磁石に日の出/日の入・月の出/月の入マーカーを反映(表示JSONから)。
    fun setMarkers(sunriseAz: Float, sunsetAz: Float, moonriseAz: Float, moonsetAz: Float) {
        compass.setMarkers(sunriseAz, sunsetAz, moonriseAz, moonsetAz)
    }

    private fun dtToMillis(dt: JSONObject?): Long {
        if (dt == null) return 0L
        val cal = Calendar.getInstance()
        cal.clear()
        cal.set(dt.optInt("year", 2025), dt.optInt("month", 1) - 1, dt.optInt("day", 1),
                dt.optInt("hour", 0), dt.optInt("min", 0), dt.optInt("sec", 0))
        return cal.timeInMillis
    }

    private fun currentMillis(): Long = startMillis + (fraction * (endMillis - startMillis)).toLong()

    // ウィジェットの扇に使う画角[°]。平面=ノモニック / 魚眼=等距離。
    private fun fovDeg(): Pair<Double, Double> {
        val fw = if (landscape) sensorW else sensorH
        val fh = if (landscape) sensorH else sensorW
        val f = if (focal > 0) focal else 50.0
        return if (!fisheye) {
            Pair(2.0 * Math.toDegrees(Math.atan((fw / 2.0) / f)), 2.0 * Math.toDegrees(Math.atan((fh / 2.0) / f)))
        } else {
            Pair(min(180.0, Math.toDegrees(fw / f)), min(180.0, Math.toDegrees(fh / f)))
        }
    }

    // 現在のパラメータから、ネイティブ天球シミュレーション用の JSON を組む。
    private fun buildParams(): String {
        val params = JSONObject()
        params.put("datetime", fmt.format(currentMillis()))
        params.put("offMin", offMin)
        params.put("lat", lat); params.put("lon", lon); params.put("alt", altM)
        params.put("az", az); params.put("el", el)
        params.put("landscape", if (landscape) 1 else 0)
        params.put("fisheye", if (fisheye) 1 else 0)
        params.put("focal", focal); params.put("sensorW", sensorW); params.put("sensorH", sensorH)
        return params.toString()
    }

    // 項目H: 単一実行の合体(single-flight coalescing)。
    //  スライドは高頻度に onChange を撒くが、ネイティブ計算は「常に1件だけ」実行する。
    //  実行中に来た変更は latestParams に上書き保持し、完了した瞬間に「最新だけ」を再計算する。
    //  → 途中の計算をキュー溜めせず、指の動きに連続追従する(間引き・離してから表示はしない)。
    //  日時ラベルは軽いのでメインスレッドで即更新し、数値フィードバックだけは常に即時。
    private var simBusy = false          // ネイティブ計算が実行中
    private var simDirty = false         // 実行中に新しい要求が来た(=完了後に再計算する)
    private var latestParams = ""        // 最新の要求パラメータ

    private fun renderSky() {
        dateLabel.text = labelFmt.format(currentMillis())   // 欄外の日時は即時更新(軽い)
        // センサー寸法が未登録なら画角が決まらない=星をどこへ置くか決められない。
        //  適当な既定値で描くと別のカメラの絵になるので、理由を出して計算もしない(2026-08-19)。
        if (sensorW <= 0.0 || sensorH <= 0.0) {
            render.visibility = View.GONE
            noSensorView.visibility = View.VISIBLE
            return
        }
        render.visibility = View.VISIBLE
        noSensorView.visibility = View.GONE
        latestParams = buildParams()
        if (simBusy) { simDirty = true; return }            // 実行中なら最新を保持するだけ
        kickSim()
    }

    private fun kickSim() {
        simBusy = true; simDirty = false
        val s = latestParams
        val elNow = el
        exec.execute {
            val res = try { HgeNative.nativeSimulateSky(s) } catch (_: Exception) { "{\"objects\":[]}" }
            val list = ArrayList<SkyObj>()
            val hz = ArrayList<Pair<Float, Float>>()
            var asp = 1.5f
            var sunAlt = -90f; var elDeg = elNow.toFloat(); var fovVDeg = 50f
            var bgl = FloatArray(0); var bgb = FloatArray(0); var bgn = 0
            try {
                val o = JSONObject(res)
                asp = o.optDouble("aspect", 1.5).toFloat()
                sunAlt = o.optDouble("sunAlt", -90.0).toFloat()      // 空/地面の色に使う
                elDeg = o.optDouble("camEl", elNow).toFloat()
                fovVDeg = o.optDouble("fovV", 50.0).toFloat()
                val gn = o.optInt("bgN", 0)
                o.optJSONArray("bgGrid")?.let { ga ->
                    if (gn >= 2 && ga.length() >= gn * gn) {
                        val al = FloatArray(gn * gn); val ab = FloatArray(gn * gn)
                        for (i in 0 until gn * gn) {
                            val e = ga.optJSONObject(i) ?: continue
                            al[i] = e.optDouble("l", 0.0).toFloat()
                            ab[i] = e.optDouble("b", 0.0).toFloat()
                        }
                        bgl = al; bgb = ab; bgn = gn
                    }
                }
                o.optJSONArray("horizon")?.let { ha ->
                    for (i in 0 until ha.length()) {
                        val e = ha.optJSONObject(i) ?: continue
                        hz.add(Pair(e.optDouble("x", 0.0).toFloat(), e.optDouble("y", 0.0).toFloat()))
                    }
                }
                val arr = o.optJSONArray("objects")
                if (arr != null) for (i in 0 until arr.length()) {
                    val e = arr.getJSONObject(i)
                    val col = try { Color.parseColor(e.optString("color", "#FFFFFF")) } catch (_: Exception) { Color.WHITE }
                    list.add(SkyObj(e.optDouble("x", 0.0).toFloat(), e.optDouble("y", 0.0).toFloat(),
                        e.optDouble("mag", 6.0).toFloat(), col, e.optString("kind", "star"), e.optString("name", "")))
                }
            } catch (_: Exception) {}
            post {
                render.setData(list, asp, sunAlt, elDeg, fovVDeg, hz, bgl, bgb, bgn)
                simBusy = false
                if (simDirty) kickSim()   // 実行中に変更あり → 最新パラメータだけを1回再計算
            }
        }
    }
}

// 【撮影シミュレーションの背景(2026-10-01 依頼)】全天の実写を地に敷く。
//
// 素材: ESO "The Milky Way panorama" (eso0932a) / 撮影 Serge Brunier / **CC BY 4.0**
//        表記は "ESO/S. Brunier"。詳細は 40_src/50_asset/README.md
//        正距円筒(plate carrée) / 銀河座標 / 4096×2048
//        並びは **u = 0.5 - l/360, v = 0.5 - b/180**(銀河経は左へ増える)。
//        大マゼラン雲(l=280.5,b=-32.9)で検算済み — この式でのみ明暗差が出る。
//
// 【点をやめない理由】月と惑星は動くので固定画像に写っていない。
//  望遠では画像がぼやける(4096px 全天 = 1px 0.088°。200mm なら横117px)。
//  名前もカタログから来ている。だから**画像の上に点を重ねる**。
internal object SkyBackground {
    private const val ASSET = "milkyway_gal_4096.jpg"
    private var bmp: android.graphics.Bitmap? = null
    private var shader: android.graphics.BitmapShader? = null
    private var tried = false

    @Synchronized
    fun shader(ctx: Context): android.graphics.BitmapShader? {
        if (tried) return shader
        tried = true
        for (sample in intArrayOf(1, 2, 4)) {       // 挿さらなければ半分の大きさでやり直す
            try {
                val op = android.graphics.BitmapFactory.Options()
                op.inSampleSize = sample
                //  RGB_565 で半分の重さにする。地に敷くだけなので透過度は要らない。
                op.inPreferredConfig = android.graphics.Bitmap.Config.RGB_565
                val b = ctx.assets.open(ASSET).use { android.graphics.BitmapFactory.decodeStream(it, null, op) }
                    ?: continue
                bmp = b
                shader = android.graphics.BitmapShader(b,
                    android.graphics.Shader.TileMode.REPEAT,    // 銀河経は一周するので横は繰り返し
                    android.graphics.Shader.TileMode.CLAMP)     // 縦(銀河緯)は端で止める
                return shader
            } catch (_: OutOfMemoryError) {
            } catch (_: Exception) { return null }
        }
        return null
    }

    @Synchronized
    fun release() {
        try { bmp?.recycle() } catch (_: Exception) {}
        bmp = null; shader = null; tried = false
    }

    fun width(): Int = bmp?.width ?: 0
    fun height(): Int = bmp?.height ?: 0
}
