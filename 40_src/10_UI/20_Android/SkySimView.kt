package app.laxei.holygrail

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
    private var boxAspect = 1.5f       // 枠の横/縦(常に横向き=≧1に固定。高さの拡大を防ぐ)
    private var sunAlt = -90.0f        // 太陽高度[°](空/地面の色に使う)
    private var camEl = 10.0f          // 撮影仰角[°]
    private var fovV = 50.0f           // 縦画角[°]
    private val magLimit = 6.5f

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

    internal fun setData(list: List<SkyObj>, asp: Float, sunAltDeg: Float, elDeg: Float, fovVDeg: Float) {
        objs = list
        var boxChanged = false
        if (asp > 0f) {
            aspect = asp
            // 枠は常に横向きのセンサー比(≧1)。縦向きでも枠サイズは変えない(項目G)。
            val nb = max(asp, 1f / asp)
            if (kotlin.math.abs(nb - boxAspect) > 0.001f) { boxAspect = nb; boxChanged = true }
        }
        sunAlt = sunAltDeg; camEl = elDeg; fovV = fovVDeg
        // 時刻/方向/仰角の変更では枠(高さ)は不変なので invalidate だけ=スライド追従が軽い。
        // カメラ変更などで枠アスペクトが実際に変わった時だけ測り直す。
        if (boxChanged) requestLayout()
        invalidate()
    }

    // 枠の中に、現在の内容アスペクト(縦向きなら縦長)を収まるように配置する。
    //  横向き=枠いっぱい / 縦向き=左右に余白を付けて中央へ(レターボックス)。
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

    // 高さは「幅 ÷ 枠アスペクト(横向き固定)」。横向きチェックの切替では枠が変わらない(項目G)。
    override fun onMeasure(widthSpec: Int, heightSpec: Int) {
        val w = MeasureSpec.getSize(widthSpec)
        val pad = dp(6f) * 2
        val a = if (boxAspect > 0.1f) boxAspect else 1.5f
        val h = ((w - pad) / a + pad).toInt().coerceAtLeast(1)
        setMeasuredDimension(w, h)
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

        // 地面(草原): 夜=黒 → 明るくなると緑。地平線(高度0°)の画面内の位置を仰角と縦画角から求める。
        val halfV = if (fovV > 1f) fovV / 2f else 25f
        val horizonNorm = -camEl / halfV
        if (horizonNorm > -1f) {                              // 地平線が画角内(または上方)なら地面が見える
            val hy = r.top + (1f - (horizonNorm.coerceIn(-1f, 1f) + 1f) / 2f) * r.height()
            ground.color = lerp(0xFF000000.toInt(), 0xFF3E7B3A.toInt(), d)   // 黒 → 草原の緑
            c.drawRect(r.left, max(hy, r.top), r.right, r.bottom, ground)
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
            c.drawText("この方向・時刻では画角内に天体がありません", r.centerX(), r.centerY(), empty)
        }
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
class SimPage(
    context: Context,
    private val exec: Executor,
    private val onLandscape: (Boolean) -> Unit
) : LinearLayout(context) {

    private val titleView = TextView(context)
    private val compass = CompassView(context)
    private val elevationView = ElevationView(context)
    private val landscapeCheck = CheckBox(context)
    // 時刻スライダー。他画面と同じ Material Slider(大きなつまみ=●)を使う。
    private val seek = com.google.android.material.slider.Slider(context)
    private val render = SkyRenderView(context)
    // 項目G: 日時は画像内でなく欄外の下に出す(年なし)。
    private val dateLabel = TextView(context)

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

        // ⓪ タイトル(項目G: 以前は無かった)
        titleView.text = "撮影シミュレーション"
        titleView.textSize = 16f
        titleView.gravity = Gravity.CENTER
        titleView.setTypeface(titleView.typeface, android.graphics.Typeface.BOLD)
        titleView.setPadding(0, 0, 0, dp(4f))
        addView(titleView, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // ① 撮影イメージ(高さは幅と横向きセンサー比から決まる=横向き/縦向きで枠は不変)
        addView(render, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // ② 日時(欄外の下・年なし)
        dateLabel.textSize = 13f
        dateLabel.gravity = Gravity.CENTER
        dateLabel.setPadding(0, dp(2f), 0, dp(2f))
        addView(dateLabel, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // ③ 余白(操作部を画面下部へ寄せる=下部固定位置)
        addView(View(context), LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

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
        addView(seek, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // ⑤ 撮影方向 / 仰角(タイトル文字つき)。ドラッグ中もイメージがリアルタイムに追従する。
        val titles = LinearLayout(context).apply { orientation = HORIZONTAL }
        titles.addView(TextView(context).apply {
            text = "撮影方向"; textSize = 13f; gravity = Gravity.CENTER
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        titles.addView(TextView(context).apply {
            text = "仰角"; textSize = 13f; gravity = Gravity.CENTER
            setTypeface(typeface, android.graphics.Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addView(titles, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        val dir = LinearLayout(context).apply { orientation = HORIZONTAL }
        dir.addView(compass, LinearLayout.LayoutParams(0, dp(140f), 1f))
        dir.addView(elevationView, LinearLayout.LayoutParams(0, dp(140f), 1f))
        addView(dir, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        // 指を離した時だけでなく、動かしている最中(onChange)にも描き直す。
        compass.onChange = { a -> az = a.toDouble(); renderSky() }
        compass.onCommit = { a -> az = a.toDouble(); renderSky() }
        elevationView.onChange = { e -> el = e.toDouble(); renderSky() }
        elevationView.onCommit = { e -> el = e.toDouble(); renderSky() }

        // ⑥ 横向きで撮る(下部固定)
        landscapeCheck.text = "横向きで撮る(ランドスケープ)"
        landscapeCheck.textSize = 13f
        landscapeCheck.setOnCheckedChangeListener { _, checked ->
            if (suppress) return@setOnCheckedChangeListener
            landscape = checked
            onLandscape(checked)     // 撮影計画へ反映(先頭ページと同じ)。再生成後に再bindされる。
            renderSky()
        }
        addView(landscapeCheck)
    }

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
                sensorW = it.optDouble("sensorSize", 36.0); sensorH = it.optDouble("sensorSizeV", 24.0)
                if (sensorH <= 0.0) sensorH = sensorW * 2.0 / 3.0
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
        latestParams = buildParams()
        dateLabel.text = labelFmt.format(currentMillis())   // 欄外の日時は即時更新(軽い)
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
            var asp = 1.5f
            var sunAlt = -90f; var elDeg = elNow.toFloat(); var fovVDeg = 50f
            try {
                val o = JSONObject(res)
                asp = o.optDouble("aspect", 1.5).toFloat()
                sunAlt = o.optDouble("sunAlt", -90.0).toFloat()      // 空/地面の色に使う
                elDeg = o.optDouble("camEl", elNow).toFloat()
                fovVDeg = o.optDouble("fovV", 50.0).toFloat()
                val arr = o.optJSONArray("objects")
                if (arr != null) for (i in 0 until arr.length()) {
                    val e = arr.getJSONObject(i)
                    val col = try { Color.parseColor(e.optString("color", "#FFFFFF")) } catch (_: Exception) { Color.WHITE }
                    list.add(SkyObj(e.optDouble("x", 0.0).toFloat(), e.optDouble("y", 0.0).toFloat(),
                        e.optDouble("mag", 6.0).toFloat(), col, e.optString("kind", "star"), e.optString("name", "")))
                }
            } catch (_: Exception) {}
            post {
                render.setData(list, asp, sunAlt, elDeg, fovVDeg)
                simBusy = false
                if (simDirty) kickSim()   // 実行中に変更あり → 最新パラメータだけを1回再計算
            }
        }
    }
}
