package app.laxei.holygrail

// 撮影シミュレーション(§7.3 画面360)。撮影計画ページャの最後のページ。
//  ・撮影方向(方位磁石)/仰角(カメラ図)を貼り付け、期間スライダーで時刻を動かす。
//  ・時刻・方向・仰角・レンズ(平面/魚眼)・センサー比から、画角内の恒星/惑星/太陽/月を
//    ネイティブ(hge_simulateSky)で投影し、下部のセンサー比の領域に描画する。
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
import android.widget.SeekBar
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
class SkyRenderView(context: Context) : View(context) {
    private var objs: List<SkyObj> = emptyList()
    private var aspect = 1.5f          // 横/縦
    private val magLimit = 6.5f

    private val bg = Paint().apply { color = 0xFF05070F.toInt() }
    private val frame = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = dp(1f); color = 0xFF3A3F55.toInt() }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xCCFFFFFF.toInt(); textSize = sp(10f) }
    private val empty = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF8890A8.toInt(); textAlign = Paint.Align.CENTER; textSize = sp(12f) }

    private fun dp(v: Float) = v * resources.displayMetrics.density
    private fun sp(v: Float) = v * resources.displayMetrics.scaledDensity

    internal fun setData(list: List<SkyObj>, asp: Float) {
        objs = list; if (asp > 0f) aspect = asp; invalidate()
    }

    // ビュー内にアスペクト比を保った最大の矩形(レターボックス)。
    private fun sensorRect(): RectF {
        val pad = dp(6f)
        val w = width - pad * 2; val h = height - pad * 2
        if (w <= 0 || h <= 0) return RectF(0f, 0f, 0f, 0f)
        var rw = w.toFloat(); var rh = rw / aspect
        if (rh > h) { rh = h.toFloat(); rw = rh * aspect }
        val l = pad + (w - rw) / 2f; val t = pad + (h - rh) / 2f
        return RectF(l, t, l + rw, t + rh)
    }

    override fun onDraw(c: Canvas) {
        val r = sensorRect()
        if (r.width() <= 0f) return
        c.drawRect(r, bg)
        c.drawRect(r, frame)
        c.save(); c.clipRect(r)
        for (o in objs) {
            val sx = r.left + (o.x + 1f) / 2f * r.width()
            val sy = r.top + (1f - (o.y + 1f) / 2f) * r.height()   // y上+ → 画面は下向きなので反転
            when (o.kind) {
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
                else -> {  // star
                    val rad = magToR(o.mag)
                    val a = (255f * min(1f, max(0.35f, (magLimit + 0.5f - o.mag) / (magLimit + 0.5f)))).toInt()
                    dot.color = (o.color and 0x00FFFFFF) or (a shl 24)
                    c.drawCircle(sx, sy, rad, dot)
                    if (o.mag < 1.6f) drawName(c, o.name, sx + rad + dp(2f), sy + dp(3f))
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

    private val compass = CompassView(context)
    private val elevationView = ElevationView(context)
    private val landscapeCheck = CheckBox(context)
    private val seek = SeekBar(context)
    private val timeLabel = TextView(context)
    private val render = SkyRenderView(context)

    // 撮影計画から読んだパラメータ
    private var lat = 0.0; private var lon = 0.0; private var altM = 0.0
    private var az = 90.0; private var el = 10.0
    private var landscape = true; private var fisheye = false
    private var focal = 50.0; private var sensorW = 36.0; private var sensorH = 24.0
    private var startMillis = 0L; private var endMillis = 0L; private var offMin = 0
    private var fraction = 0.5f     // スライダー位置(0..1)。再bind でも保持。
    private var suppress = false

    private val fmt = SimpleDateFormat("yyyy/MM/dd HH:mm:ss", Locale.US)
    private val labelFmt = SimpleDateFormat("M/d(E) HH:mm", Locale.JAPAN)

    private fun dp(v: Float) = (v * resources.displayMetrics.density).toInt()

    init {
        orientation = VERTICAL
        setPadding(dp(12f), dp(8f), dp(12f), dp(8f))

        val title = TextView(context).apply { text = "撮影シミュレーション"; textSize = 15f; setTypeface(typeface, android.graphics.Typeface.BOLD) }
        addView(title)

        landscapeCheck.text = "横向きで撮る(ランドスケープ)"
        landscapeCheck.textSize = 13f
        landscapeCheck.setOnCheckedChangeListener { _, checked ->
            if (suppress) return@setOnCheckedChangeListener
            landscape = checked
            onLandscape(checked)     // 撮影計画へ反映(先頭ページと同じ)。再生成後に再bindされる。
            renderSky()
        }
        addView(landscapeCheck)

        val dir = LinearLayout(context).apply { orientation = HORIZONTAL }
        dir.addView(compass, LinearLayout.LayoutParams(0, dp(150f), 1f))
        dir.addView(elevationView, LinearLayout.LayoutParams(0, dp(150f), 1f))
        addView(dir, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        compass.onCommit = { a -> az = a.toDouble(); renderSky() }          // プレビューのみ(計画は変えない)
        elevationView.onCommit = { e -> el = e.toDouble(); renderSky() }

        timeLabel.gravity = Gravity.CENTER
        timeLabel.textSize = 13f
        addView(timeLabel, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        seek.max = 1000
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) {
                fraction = p / 1000f
                updateTimeLabel()
                renderSky()
            }
            override fun onStartTrackingTouch(sb: SeekBar?) {}
            override fun onStopTrackingTouch(sb: SeekBar?) {}
        })
        addView(seek, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        addView(render, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
    }

    // 撮影計画(nativeGetPlanJson の JSON)を読み込み、ウィジェット初期化＆描画。
    //  masterLensesJson: マスターレンズ一覧(nativeGetMasterLenses)。起動時にアセットから再コピー
    //  されるので、lenses_list.json の "fisheye" を編集→ビルドすれば既存計画でも即反映される
    //  (計画に保存済みの古い値でなく、レンズ名でマスターを引いた fisheye を優先する)。
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
                //  マスターに無ければ計画保存値、それも無ければレンズ名でフォールバック。
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
        seek.progress = (fraction * 1000f).toInt()
        suppress = false
        updateTimeLabel()
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

    private fun updateTimeLabel() {
        timeLabel.text = labelFmt.format(currentMillis())
    }

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

    private var renderSeq = 0
    private fun renderSky() {
        val params = JSONObject()
        params.put("datetime", fmt.format(currentMillis()))
        params.put("offMin", offMin)
        params.put("lat", lat); params.put("lon", lon); params.put("alt", altM)
        params.put("az", az); params.put("el", el)
        params.put("landscape", if (landscape) 1 else 0)
        params.put("fisheye", if (fisheye) 1 else 0)
        params.put("focal", focal); params.put("sensorW", sensorW); params.put("sensorH", sensorH)
        val s = params.toString()
        val seq = ++renderSeq
        exec.execute {
            val res = try { HgeNative.nativeSimulateSky(s) } catch (_: Exception) { "{\"objects\":[]}" }
            val list = ArrayList<SkyObj>()
            var asp = 1.5f
            try {
                val o = JSONObject(res)
                asp = o.optDouble("aspect", 1.5).toFloat()
                val arr = o.optJSONArray("objects")
                if (arr != null) for (i in 0 until arr.length()) {
                    val e = arr.getJSONObject(i)
                    val col = try { Color.parseColor(e.optString("color", "#FFFFFF")) } catch (_: Exception) { Color.WHITE }
                    list.add(SkyObj(e.optDouble("x", 0.0).toFloat(), e.optDouble("y", 0.0).toFloat(),
                        e.optDouble("mag", 6.0).toFloat(), col, e.optString("kind", "star"), e.optString("name", "")))
                }
            } catch (_: Exception) {}
            post { if (seq == renderSeq) render.setData(list, asp) }
        }
    }
}
