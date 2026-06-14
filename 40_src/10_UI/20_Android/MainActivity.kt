package app.laxei.holygrail

import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import android.widget.ViewFlipper
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.slider.LabelFormatter
import com.google.android.material.slider.RangeSlider
import com.google.android.material.slider.Slider
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Locale
import java.util.TimeZone

// 330撮影計画設定 / 430撮影中(仕様書10 §7.3)。ViewFlipperで切替。
// 開始/終了時刻を編集するとスケジュールを自動生成して表示し、撮影開始できる。
class MainActivity : AppCompatActivity(), HgeListener {

    private lateinit var flipper: ViewFlipper

    // 330
    private lateinit var planName: TextView
    private lateinit var startDate: Button
    private lateinit var startTime: Button
    private lateinit var endDate: Button
    private lateinit var endTime: Button
    private lateinit var resetButton: Button
    private lateinit var placeText: TextView
    private lateinit var latlngText: TextView
    private lateinit var cameraText: TextView
    private lateinit var lensText: TextView
    private lateinit var intervalText: TextView
    private lateinit var sensorText: TextView
    private lateinit var dirText: TextView
    private lateinit var compass: CompassView
    private lateinit var elevationView: ElevationView
    private lateinit var planSchedule: LinearLayout
    private lateinit var planStartButton: Button
    private lateinit var edgeSpinner: Spinner
    private lateinit var edgeSearchButton: Button
    private lateinit var searchButton: Button
    private lateinit var ipInput: EditText
    private lateinit var connectButton: Button

    // 撮影制御方法初期値: メニュー + 方法別エディタ
    private lateinit var planMenu: ImageView
    private var ccmJson: JSONObject? = null     // 編集中のccm全体(初期値 or 計画固有)
    private var editingKey = "night"            // 編集中の方法
    private var editingPlanCcm = false          // true=計画固有ccmを編集 / false=初期値ccm
    private var editColor = 0                    // 編集中の色(0xRRGGBB)

    // 露出(iso/ss/fn)はカメラ設定値の文字列配列からスライダーで選択する。
    private var isoValues = listOf<String>()    // hge_getExpoValues の iso 配列(real昇順)
    private var ssValues = listOf<String>()     // 同 ss 配列(real昇順)
    private var fnValues = listOf<String>()     // 同 fn 配列(レンズf範囲, 昇順)
    // 表示順は「左=暗い時の設定 → 右=明るい時の設定」で統一する(仕様4の方針)。
    //  iso: 左=高感度→右=低感度 / ss: 左=長秒→右=短秒 / fn: 左=開放→右=絞る。
    private var isoDisp = listOf<String>()
    private var ssDisp = listOf<String>()
    private var fnDisp = listOf<String>()
    private lateinit var fixEditor: ExposureEditor      // 夜間 固定露出(単一)
    private lateinit var moonInitEditor: ExposureEditor // 月 開始時露出(単一)
    private lateinit var editLimit: LimitEditor         // 自動露出 露出限界(優先度+明暗を一体化)
    private lateinit var moonLimit: LimitEditor         // 月 露出限界(同上)

    // 430
    private lateinit var capName: TextView
    private lateinit var capGear: TextView
    private lateinit var capDir: TextView
    private lateinit var capState: TextView
    private lateinit var capProgress: TextView
    private lateinit var capCaptured: TextView
    private lateinit var capSchedule: LinearLayout
    private lateinit var capStopButton: Button

    private val startCal = Calendar.getInstance()
    private val endCal = Calendar.getInstance()
    private var latestSchedule = ""

    // エッジ端末
    private data class Edge(val name: String, val ip: String, val port: Int)
    private val edges = mutableListOf<Edge>()
    private val handler = Handler(Looper.getMainLooper())
    private var edgeCapturing = false

    private val fmtDate = SimpleDateFormat("yyyy-MM-dd", Locale.US)
    private val fmtTime = SimpleDateFormat("HH:mm", Locale.US)
    private val fmtIso = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.US)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        bindViews()

        HgeNative.nativeSetLogDir(getExternalFilesDir(null)?.absolutePath ?: filesDir.absolutePath)
        HgeNative.nativeInit()
        HgeNative.nativeSetListener(this)
        loadExpoValues()
        buildExposureEditors()
        refreshEdgeSpinner()

        wireListeners()
        restorePlan()    // 保存済み計画があれば復元、無ければ出荷時計画を表示(再生成しない)
    }

    // Entity が持つ計画(保存済み or 出荷時)の時刻・内容を画面に反映する。
    private fun restorePlan() {
        val sched = HgeNative.nativeScheduleJson()
        var ok = false
        try {
            val o = JSONObject(sched)
            fmtIso.parse(o.optString("start"))?.let { startCal.time = it; ok = true }
            fmtIso.parse(o.optString("end"))?.let { endCal.time = it }
        } catch (_: Exception) {}
        if (!ok) {   // フォールバック: 現在〜2時間後で再生成
            endCal.timeInMillis = startCal.timeInMillis
            endCal.add(Calendar.HOUR_OF_DAY, 2)
            updateTimeButtons(); pushTimesToEntity(); return
        }
        updateTimeButtons()
        latestSchedule = sched
        updatePlanDisplay(sched)
    }

    private fun bindViews() {
        flipper = findViewById(R.id.flipper)
        planName = findViewById(R.id.plan_nameText)
        startDate = findViewById(R.id.plan_startDate)
        startTime = findViewById(R.id.plan_startTime)
        endDate = findViewById(R.id.plan_endDate)
        endTime = findViewById(R.id.plan_endTime)
        resetButton = findViewById(R.id.plan_resetButton)
        placeText = findViewById(R.id.plan_placeText)
        latlngText = findViewById(R.id.plan_latlngText)
        cameraText = findViewById(R.id.plan_cameraText)
        lensText = findViewById(R.id.plan_lensText)
        intervalText = findViewById(R.id.plan_intervalText)
        sensorText = findViewById(R.id.plan_sensorText)
        dirText = findViewById(R.id.plan_dirText)
        compass = findViewById(R.id.plan_compass)
        elevationView = findViewById(R.id.plan_elevation)
        planSchedule = findViewById(R.id.plan_scheduleContainer)
        planStartButton = findViewById(R.id.plan_startButton)
        edgeSpinner = findViewById(R.id.plan_edgeSpinner)
        edgeSearchButton = findViewById(R.id.plan_edgeSearchButton)
        searchButton = findViewById(R.id.searchButton)
        ipInput = findViewById(R.id.ipInput)
        connectButton = findViewById(R.id.connectButton)
        capName = findViewById(R.id.cap_nameText)
        capGear = findViewById(R.id.cap_gearText)
        capDir = findViewById(R.id.cap_dirText)
        capState = findViewById(R.id.cap_stateText)
        capProgress = findViewById(R.id.cap_progressText)
        capCaptured = findViewById(R.id.cap_capturedText)
        capSchedule = findViewById(R.id.cap_scheduleContainer)
        capStopButton = findViewById(R.id.cap_stopButton)
        planMenu = findViewById(R.id.plan_menu)
    }

    private fun wireListeners() {
        // 撮影方向(方位磁石)/仰角(カメラの絵)を離した時にEntityへ反映しスケジュール再生成。
        compass.onCommit = { az -> pushDirectionToEntity(az, elevationView.angle) }
        elevationView.onCommit = { el -> pushDirectionToEntity(compass.azimuth, el) }
        startDate.setOnClickListener { pickDate(startCal) }
        startTime.setOnClickListener { pickTime(startCal) }
        endDate.setOnClickListener { pickDate(endCal) }
        endTime.setOnClickListener { pickTime(endCal) }
        resetButton.setOnClickListener {
            val now = Calendar.getInstance()
            startCal.timeInMillis = now.timeInMillis
            endCal.timeInMillis = now.timeInMillis
            endCal.add(Calendar.HOUR_OF_DAY, 2)
            updateTimeButtons(); pushTimesToEntity()
        }
        findViewById<Button>(R.id.plan_saveButton).setOnClickListener {
            Thread {
                val r = HgeNative.nativeSavePlan()
                runOnUiThread {
                    Toast.makeText(this, if (r == 0) "撮影計画を保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
                }
            }.start()
        }
        planStartButton.setOnClickListener {
            val e = selectedEdge()
            if (e == null) {
                HgeNative.nativeCaptureStart()
                flipper.displayedChild = 1
            } else {
                startOnEdge(e)
            }
        }
        capStopButton.setOnClickListener {
            val e = selectedEdge()
            if (e == null) {
                HgeNative.nativeCaptureStop()
                flipper.displayedChild = 0
            } else {
                stopOnEdge(e)
            }
        }
        edgeSearchButton.setOnClickListener {
            Thread {
                val js = HgeNative.nativeEdgeSearch(2000)
                runOnUiThread { onEdgesFound(js) }
            }.start()
        }
        searchButton.setOnClickListener { HgeNative.nativeSearchDevices() }
        connectButton.setOnClickListener {
            val host = ipInput.text.toString().trim()
            Thread { HgeNative.nativeConnectManual(host) }.start()
        }
        // 初期値メニュー(plan_menu→メニュー画面)
        planMenu.setOnClickListener { openCcmMenu() }
        findViewById<ImageView>(R.id.cmenu_back).setOnClickListener { flipper.displayedChild = 0 }
        findViewById<Button>(R.id.cmenu_night).setOnClickListener { openCcmEdit("night") }
        findViewById<Button>(R.id.cmenu_sunrise).setOnClickListener { openCcmEdit("sunrise") }
        findViewById<Button>(R.id.cmenu_sunset).setOnClickListener { openCcmEdit("sunset") }
        findViewById<Button>(R.id.cmenu_day).setOnClickListener { openCcmEdit("day") }
        findViewById<Button>(R.id.cmenu_moon).setOnClickListener { openMoonEdit() }
        findViewById<ImageView>(R.id.edit_back).setOnClickListener { flipper.displayedChild = if (editingPlanCcm) 0 else 2 }
        findViewById<Button>(R.id.edit_save).setOnClickListener { saveCcmEdit() }
        findViewById<Button>(R.id.edit_color_btn).setOnClickListener {
            showColorPicker(editColor) { c -> editColor = c; findViewById<View>(R.id.edit_color_swatch).setBackgroundColor(0xFF000000.toInt() or c) }
        }
        // スライダーの値ラベル更新(露出スライダーと形を統一するため Material Slider・仕様8)
        setupValueSlider(R.id.edit_alt_seek, 14, gradient = true) {
            val deg = seekToAlt(it)
            findViewById<TextView>(R.id.edit_alt_val).text = altLabel(deg)
            updateAltTimes(deg.toInt())
        }
        setupValueSlider(R.id.edit_ev_seek, 30, gradient = true) {
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f ev", seekToEv(it))
        }
        // 朝日/夕日の太陽高度=範囲スライダー(2つまみ)。明暗バー下地・つまみ●。
        findViewById<RangeSlider>(R.id.edit_alt_range).apply {
            valueFrom = 0f; valueTo = 24f; stepSize = 1f
            isTickVisible = false; labelBehavior = LabelFormatter.LABEL_GONE
            setCustomThumbDrawablesForValues(R.drawable.thumb_dot, R.drawable.thumb_dot)
            trackActiveTintList = transparentTint; trackInactiveTintList = transparentTint
            addOnChangeListener { _, _, _ -> updateAltRangeLabels() }
        }
        // 月の影響への対処
        findViewById<ImageView>(R.id.moon_back).setOnClickListener { flipper.displayedChild = if (editingPlanCcm) 0 else 2 }
        findViewById<Button>(R.id.moon_save).setOnClickListener { saveMoonEdit() }
        findViewById<Button>(R.id.moon_color_btn).setOnClickListener {
            showColorPicker(editColor) { c -> editColor = c; findViewById<View>(R.id.moon_color_swatch).setBackgroundColor(0xFF000000.toInt() or c) }
        }
        setupValueSlider(R.id.moon_startlum_seek, 30, gradient = true, thumbRes = R.drawable.ic_moon) {
            findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", it * 0.1)
        }
        setupValueSlider(R.id.moon_ev_seek, 100, gradient = true, thumbRes = R.drawable.ic_moon) {
            findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -it * 0.1)
        }
        setupValueSlider(R.id.moon_extcoef_seek, 50) {
            findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + it * 0.01)
        }
        setupValueSlider(R.id.moon_skycoef_seek, 100) {
            findViewById<TextView>(R.id.moon_skycoef_val).text = "$it%"
        }
        val moonModes = arrayOf("補正しない", "画角全体の明るさで自動補正", "月に露出を合わせる")
        val ma = ArrayAdapter(this, android.R.layout.simple_spinner_item, moonModes)
        ma.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        val moonSpinner = findViewById<Spinner>(R.id.moon_mode)
        moonSpinner.adapter = ma
        moonSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: android.widget.AdapterView<*>?, v: View?, pos: Int, id: Long) {
                // 0=補正しない(中身なし) / 1=画角全体で自動補正 / 2=月に露出を合わせる
                findViewById<View>(R.id.moon_auto_section).visibility = if (pos == 1) View.VISIBLE else View.GONE
                findViewById<View>(R.id.moon_exp_section).visibility = if (pos == 2) View.VISIBLE else View.GONE
                findViewById<View>(R.id.moon_limit_section).visibility = if (pos == 0) View.GONE else View.VISIBLE
            }
            override fun onNothingSelected(p: android.widget.AdapterView<*>?) {}
        }
    }

    private fun openMoonEdit() {
        val o = ccmJson?.optJSONObject("moon") ?: return
        findViewById<TextView>(R.id.moon_title).text = "月の影響への対処" + (if (editingPlanCcm) "（この計画）" else "（初期値）")
        editColor = o.optInt("color", 0)
        findViewById<View>(R.id.moon_color_swatch).setBackgroundColor(0xFF000000.toInt() or editColor)
        findViewById<Spinner>(R.id.moon_mode).setSelection(o.optInt("mode", 0))
        val slP = (o.optDouble("startLuminance", 0.0) * 10).toInt().coerceIn(0, 30)
        setSliderProgress(R.id.moon_startlum_seek, slP)
        findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", slP * 0.1)
        val evP = (-o.optDouble("ev", 0.0) * 10).toInt().coerceIn(0, 100)
        setSliderProgress(R.id.moon_ev_seek, evP)
        findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -evP * 0.1)
        val ecP = ((o.optDouble("extinctionCoef", 0.2) - 0.1) * 100).toInt().coerceIn(0, 50)
        setSliderProgress(R.id.moon_extcoef_seek, ecP)
        findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + ecP * 0.01)
        val skP = o.optDouble("skyBrightnessCoef", 100.0).toInt().coerceIn(0, 100)
        setSliderProgress(R.id.moon_skycoef_seek, skP)
        findViewById<TextView>(R.id.moon_skycoef_val).text = "$skP%"
        findViewById<CheckBox>(R.id.moon_atmext).isChecked = o.optBoolean("atmosphericExtinction", false)
        findViewById<CheckBox>(R.id.moon_geocorr).isChecked = o.optBoolean("geocentricCorrection", false)
        moonInitEditor.set(o.optJSONObject("initialExposure"))
        // 月撮影時露出限界: 暗所側は夜間撮影の設定値(limitBright)で固定。
        val nightLimit = ccmJson?.optJSONObject("night")?.optJSONObject("limitBright")
        moonLimit.set(o.optJSONObject("limitBright"), o.optJSONObject("limitDark"),
            o.optJSONArray("priority"), o.optBoolean("initialBright", true),
            moonMode = true, nightLimit = nightLimit)
        flipper.displayedChild = 4
    }

    private fun saveMoonEdit() {
        val all = ccmJson ?: return
        val o = all.optJSONObject("moon") ?: return
        o.put("color", editColor)
        o.put("mode", findViewById<Spinner>(R.id.moon_mode).selectedItemPosition)
        o.put("startLuminance", sliderProgress(R.id.moon_startlum_seek) * 0.1)
        o.put("ev", -sliderProgress(R.id.moon_ev_seek) * 0.1)
        o.put("extinctionCoef", 0.1 + sliderProgress(R.id.moon_extcoef_seek) * 0.01)
        o.put("skyBrightnessCoef", sliderProgress(R.id.moon_skycoef_seek).toDouble())
        o.put("atmosphericExtinction", findViewById<CheckBox>(R.id.moon_atmext).isChecked)
        o.put("geocentricCorrection", findViewById<CheckBox>(R.id.moon_geocorr).isChecked)
        o.put("priority", moonLimit.getPriority())
        o.put("initialExposure", moonInitEditor.get())
        o.put("limitBright", moonLimit.getBright())
        o.put("limitDark", moonLimit.getDark())
        o.put("initialBright", moonLimit.getInitialBright())
        val r = if (editingPlanCcm) HgeNative.nativeSetPlanCcm(all.toString()) else HgeNative.nativeSetCcmDefaults(all.toString())
        Toast.makeText(this, if (r == 0) "保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
        flipper.displayedChild = if (editingPlanCcm) 0 else 2
    }

    // --- 撮影制御方法 初期値: メニュー + 方法別エディタ ---

    private fun openCcmMenu() {
        editingPlanCcm = false   // メニュー経由は初期値ccmの編集
        ccmJson = try { JSONObject(HgeNative.nativeGetCcmDefaults()) } catch (e: Exception) { null }
        flipper.displayedChild = 2
    }

    // 撮影計画画面の色別リストから「この計画の」撮影制御方法を編集する(初期値とは別)。
    private fun openPlanCcmEdit(key: String) {
        editingPlanCcm = true
        ccmJson = try { JSONObject(HgeNative.nativeGetPlanCcm()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        if (key == "moon") openMoonEdit() else openCcmEdit(key)
    }

    private val ccmTypeToKey = mapOf(1 to "night", 2 to "sunrise", 3 to "sunset", 4 to "day", 5 to "moon")
    private val ccmTypeName = mapOf(1 to "夜間撮影", 2 to "朝日撮影", 3 to "夕日撮影", 4 to "日中撮影", 5 to "月の影響")

    // 撮影計画の撮影制御方法リスト。スケジュールに入っている方法は右側(縦)に、
    // 入らなかった方法+月(常にスケジュール外)は End の下(横並び)に色別タップ可能で出す。
    private fun buildPlanCcmList(o: JSONObject) {
        val list = findViewById<LinearLayout>(R.id.plan_ccmList)
        val extra = findViewById<LinearLayout>(R.id.plan_ccmExtra)
        list.removeAllViews(); extra.removeAllViews()
        val inSched = linkedSetOf<Int>()
        o.optJSONArray("windows")?.let { arr ->
            for (i in 0 until arr.length()) {
                val t = arr.getJSONObject(i).optInt("type")
                if (t in 1..4) inSched.add(t)
            }
        }
        for (t in inSched) addPlanCcmButton(list, t, fullWidth = true)          // 右側=スケジュール内
        val notIn = (1..4).filter { it !in inSched }.toMutableList()
        notIn.add(5)                                                            // 月は常にスケジュール外
        for (t in notIn) addPlanCcmButton(extra, t, fullWidth = false)          // End の下=スケジュール外
    }

    private fun addPlanCcmButton(parent: LinearLayout, t: Int, fullWidth: Boolean) {
        val key = ccmTypeToKey[t] ?: return
        val btn = TextView(this)
        btn.text = ccmTypeName[t]; btn.textSize = 13f; btn.gravity = Gravity.CENTER
        btn.setBackgroundColor(ccmColor(t)); btn.setPadding(dp(6), dp(10), dp(6), dp(10))
        btn.isClickable = true; btn.isFocusable = true
        val lp = if (fullWidth)
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { setMargins(0, 0, 0, dp(4)) }
        else
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(2), 0, dp(2), 0) }
        btn.layoutParams = lp
        btn.setOnClickListener { openPlanCcmEdit(key) }
        parent.addView(btn)
    }

    // 太陽高度: Slider 0..14 ⇔ -19..-5°(1°刻み。分単位は将来キーボードで)
    private fun altToSeek(v: Double) = (v + 19.0).toInt().coerceIn(0, 14)
    private fun seekToAlt(p: Int) = -19.0 + p
    // ev: SeekBar 0..30 ⇔ -5.0..+5.0(1/3刻み)
    private fun evToSeek(v: Double) = ((v + 5.0) * 3.0).toInt().coerceIn(0, 30)
    private fun seekToEv(p: Int) = -5.0 + p / 3.0

    private fun altLabel(v: Double) = String.format("%.0f°", v)

    // 固定露出太陽高度の start(日没側)/end(日の出側) 時刻を天文計算で更新する(夜間のみ)。
    // start は上の行、end は固定露出太陽高度と同じ行に右寄せ表示(ラベル文言は付けない)。
    private fun updateAltTimes(deg: Int) {
        val startTv = findViewById<TextView>(R.id.edit_alt_start)
        val endTv = findViewById<TextView>(R.id.edit_alt_end)
        if (editingKey != "night") { startTv.text = ""; endTv.text = ""; return }
        Thread {
            val js = HgeNative.nativeSunAltitudeTimes(deg)
            runOnUiThread {
                try {
                    val o = JSONObject(js)
                    // "MM/dd HH:mm" → 時刻だけ表示。等幅・桁揃えで縦に整列。
                    startTv.text = "Start".padEnd(5) + " " + o.optString("start", "--").substringAfterLast(" ")
                    endTv.text = "End".padEnd(5) + " " + o.optString("end", "--").substringAfterLast(" ")
                } catch (_: Exception) { startTv.text = ""; endTv.text = "" }
            }
        }.start()
    }

    // 薄明帯ラベルと明暗バーを、スライダーのつまみ可動域(両端から trackSidePadding 内側)に
    // 合わせる。これをしないと端の度数がつまみ位置とずれる。
    private fun applyAltPadding(tsp: Int) {
        findViewById<LinearLayout>(R.id.edit_alt_bands).setPadding(tsp, 0, tsp, 0)
        val bar = findViewById<View>(R.id.edit_alt_bar)
        val lp = bar.layoutParams as FrameLayout.LayoutParams
        lp.setMargins(tsp, lp.topMargin, tsp, lp.bottomMargin)
        bar.layoutParams = lp
    }

    // 朝日/夕日の太陽高度範囲スライダー: 値 0..24 ⇔ 朝日 -18..+6 / 夕日 +6..-18(1°刻み)。
    private fun altRangeValToDeg(v: Int): Double = if (editingKey == "sunset") 6.0 - v else -18.0 + v
    private fun altRangeDegToVal(deg: Double): Int =
        (if (editingKey == "sunset") 6.0 - deg else deg + 18.0).toInt().coerceIn(0, 24)

    // 朝日/夕日: Start/End それぞれに角度と時刻(朝日=日の出側rise、夕日=日没側set)を表示する。
    private fun updateAltRangeLabels() {
        val v = findViewById<RangeSlider>(R.id.edit_alt_range).values
        if (v.size < 2) return
        val rising = editingKey == "sunrise"
        setAltThumbLabel(R.id.edit_alt_start, "Start", altRangeValToDeg(v[0].toInt()), rising)
        setAltThumbLabel(R.id.edit_alt_end, "End", altRangeValToDeg(v[1].toInt()), rising)
    }
    private val altTimeCache = HashMap<Int, String>()   // tvId -> 直近の時刻(ちらつき防止に保持)

    // 等幅・固定幅で「Start -06.0°  HH:mm」を表示。角度は符号+整数2桁+小数1桁の固定幅。
    // 時刻は直近値を保持したまま角度だけ即時更新し、計算後に時刻を差し替える(空白化させない)。
    private fun setAltThumbLabel(tvId: Int, label: String, deg: Double, rising: Boolean) {
        val tv = findViewById<TextView>(tvId)
        val lab = label.padEnd(5)                       // "Start"/"End  " で桁を揃える
        val angleStr = String.format("%+05.1f°", deg)   // 例 -06.0° / +00.0° / -18.0°
        val curTime = altTimeCache[tvId] ?: "--:--"
        tv.text = "$lab $angleStr  $curTime"
        Thread {
            val js = HgeNative.nativeSunAltitudeTimes(deg.toInt())
            runOnUiThread {
                val time = try {
                    val o = JSONObject(js)
                    (if (rising) o.optString("end", "--") else o.optString("start", "--")).substringAfterLast(" ")
                } catch (_: Exception) { "--:--" }
                altTimeCache[tvId] = time
                if (tv.text.toString().contains(angleStr)) {   // つまみがまだ同じ角度なら時刻を更新
                    tv.text = "$lab $angleStr  $time"
                }
            }
        }.start()
    }

    // 薄明帯の1区分(帯名・度幅の重み・背景色)。
    private data class Band(val label: String, val weight: Float, val color: Int)

    // 薄明帯ラベルを汎用に組む。bands=各区分、ticks=各区分の左端の度目盛り+右端(計 bands+1 個)。
    private fun buildBands(container: LinearLayout, bands: List<Band>, ticks: List<String>) {
        container.removeAllViews()
        val scale = LinearLayout(this); scale.orientation = LinearLayout.HORIZONTAL
        for (i in bands.indices) {
            val tv = TextView(this); tv.text = ticks.getOrElse(i) { "" }; tv.textSize = 9f
            tv.setTextColor(0xFF888888.toInt())
            tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, bands[i].weight)
            scale.addView(tv)
        }
        val end = TextView(this); end.text = ticks.getOrElse(bands.size) { "" }; end.textSize = 9f
        end.setTextColor(0xFF888888.toInt()); end.gravity = Gravity.END
        end.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        scale.addView(end)
        container.addView(scale)
        val bandRow = LinearLayout(this); bandRow.orientation = LinearLayout.HORIZONTAL
        for (b in bands) {
            val tv = TextView(this); tv.text = b.label; tv.textSize = 10f; tv.gravity = Gravity.CENTER
            tv.setTextColor(0xFFFFFFFF.toInt()); tv.setBackgroundColor(b.color)
            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, b.weight)
            lp.setMargins(dp(1), 0, dp(1), 0); tv.layoutParams = lp
            bandRow.addView(tv)
        }
        container.addView(bandRow)
    }

    private val colAstro = 0xFF37474F.toInt(); private val colNaut = 0xFF607D8B.toInt()
    private val colCivil = 0xFF9E9E9E.toInt(); private val colDay = 0xFFE0B96A.toInt(); private val colNight = 0xFF1A1A2E.toInt()

    // 夜間: 左=-19°暗 → 右=-5°明。-19〜-18=夜(無ラベル)/-18〜-12 天文/-12〜-6 航海/-6〜-5 市民。
    private fun buildNightBands(container: LinearLayout) = buildBands(container,
        listOf(Band("", 1f, colNight), Band("天文薄明", 6f, colAstro), Band("航海薄明", 6f, colNaut), Band("市民", 1f, colCivil)),
        listOf("-19°", "-18°", "-12°", "-6°", "-5°"))

    // 朝日: 左=-18°暗 → 右=+6°明。天文/航海/市民/地平線上(0〜+6)。
    private fun buildSunriseBands(container: LinearLayout) = buildBands(container,
        listOf(Band("天文薄明", 6f, colAstro), Band("航海薄明", 6f, colNaut), Band("市民薄明", 6f, colCivil), Band("地平線上", 6f, colDay)),
        listOf("-18°", "-12°", "-6°", "0°", "+6°"))

    // 夕日: 左=+6°明 → 右=-18°暗(朝日と逆)。地平線上/市民/航海/天文。
    private fun buildSunsetBands(container: LinearLayout) = buildBands(container,
        listOf(Band("地平線上", 6f, colDay), Band("市民薄明", 6f, colCivil), Band("航海薄明", 6f, colNaut), Band("天文薄明", 6f, colAstro)),
        listOf("+6°", "0°", "-6°", "-12°", "-18°"))

    private fun openCcmEdit(key: String) {
        val o = ccmJson?.optJSONObject(key) ?: return
        editingKey = key
        val title = mapOf("night" to "夜間撮影", "sunrise" to "朝日撮影", "sunset" to "夕日撮影", "day" to "日中撮影")[key]
        findViewById<TextView>(R.id.edit_title).text = title + (if (editingPlanCcm) "（この計画）" else "（初期値）")
        editColor = o.optInt("color", 0)
        findViewById<View>(R.id.edit_color_swatch).setBackgroundColor(0xFF000000.toInt() or editColor)

        val hasAlt = key != "day"
        val hasEv = key != "night"
        val isNight = key == "night"
        findViewById<View>(R.id.edit_alt_section).visibility = if (hasAlt) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_ev_section).visibility = if (hasEv) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_autoEdge).visibility = if (isNight) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_fixed_section).visibility = if (isNight) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_limit_section).visibility = if (isNight) View.GONE else View.VISIBLE

        if (hasAlt) {
            val isNightAlt = key == "night"
            findViewById<TextView>(R.id.edit_alt_title).text =
                if (isNightAlt) "固定露出太陽高度" else "太陽高度"
            findViewById<View>(R.id.edit_alt_seek).visibility = if (isNightAlt) View.VISIBLE else View.GONE
            findViewById<View>(R.id.edit_alt_range).visibility = if (isNightAlt) View.GONE else View.VISIBLE
            findViewById<View>(R.id.edit_alt_val).visibility = if (isNightAlt) View.VISIBLE else View.GONE
            val bar = findViewById<View>(R.id.edit_alt_bar)
            if (isNightAlt) {
                bar.setBackgroundResource(R.drawable.brightness_bar)
                buildNightBands(findViewById(R.id.edit_alt_bands))
                val sl = findViewById<Slider>(R.id.edit_alt_seek)
                sl.post { applyAltPadding(sl.trackSidePadding) }
                val p = altToSeek(o.optDouble("sunAltitude", -18.0))
                setSliderProgress(R.id.edit_alt_seek, p)
                findViewById<TextView>(R.id.edit_alt_val).text = altLabel(seekToAlt(p))
                updateAltTimes(seekToAlt(p).toInt())
            } else {
                val reversed = key == "sunset"
                bar.setBackgroundResource(if (reversed) R.drawable.brightness_bar_rev else R.drawable.brightness_bar)
                if (reversed) buildSunsetBands(findViewById(R.id.edit_alt_bands))
                else buildSunriseBands(findViewById(R.id.edit_alt_bands))
                val rg = findViewById<RangeSlider>(R.id.edit_alt_range)
                rg.post { applyAltPadding(rg.trackSidePadding) }
                val startDeg = o.optDouble("sunAltitude", if (reversed) 0.0 else -6.0)
                val endDeg = o.optDouble("sunAltitudeEnd", if (reversed) -6.0 else 0.0)
                val v1 = altRangeDegToVal(startDeg); val v2 = altRangeDegToVal(endDeg)
                rg.values = listOf(minOf(v1, v2).toFloat(), maxOf(v1, v2).toFloat())
                altTimeCache.clear()   // 別画面の時刻を引きずらない
                updateAltRangeLabels()
            }
        }
        if (hasEv) {
            val p = evToSeek(o.optDouble("ev", 0.0))
            setSliderProgress(R.id.edit_ev_seek, p)
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f ev", seekToEv(p))
        }
        if (isNight) {
            findViewById<CheckBox>(R.id.edit_autoEdge).isChecked = o.optBoolean("autoEdge", true)
            fixEditor.set(o.optJSONObject("limitBright"))
        } else {
            editLimit.set(o.optJSONObject("limitBright"), o.optJSONObject("limitDark"),
                o.optJSONArray("priority"), o.optBoolean("initialBright", true))
        }
        flipper.displayedChild = 3
    }

    private fun saveCcmEdit() {
        val all = ccmJson ?: return
        val o = all.optJSONObject(editingKey) ?: return
        o.put("color", editColor)
        when (editingKey) {
            "night" -> o.put("sunAltitude", seekToAlt(sliderProgress(R.id.edit_alt_seek)))
            "sunrise", "sunset" -> {
                val v = findViewById<RangeSlider>(R.id.edit_alt_range).values
                o.put("sunAltitude", altRangeValToDeg(v[0].toInt()))      // 撮り始め
                o.put("sunAltitudeEnd", altRangeValToDeg(v[1].toInt()))   // 終わり
            }
        }
        if (editingKey != "night") o.put("ev", seekToEv(sliderProgress(R.id.edit_ev_seek)))
        if (editingKey == "night") {
            o.put("autoEdge", findViewById<CheckBox>(R.id.edit_autoEdge).isChecked)
            val e = fixEditor.get()
            o.put("limitBright", e)
            o.put("limitDark", JSONObject(e.toString()))   // 固定露出は明暗同値
        } else {
            o.put("priority", editLimit.getPriority())
            o.put("limitBright", editLimit.getBright())
            o.put("limitDark", editLimit.getDark())
            o.put("initialBright", editLimit.getInitialBright())
        }
        val r = if (editingPlanCcm) HgeNative.nativeSetPlanCcm(all.toString()) else HgeNative.nativeSetCcmDefaults(all.toString())
        Toast.makeText(this, if (r == 0) "保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
        flipper.displayedChild = if (editingPlanCcm) 0 else 2   // 計画固有は計画画面へ、初期値はメニューへ
    }

    // SeekBar 値変更だけ拾う簡易リスナ(カラーピッカーのRGBで使用)。
    private fun seekListener(onChange: (Int) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) = onChange(p)
        override fun onStartTrackingTouch(sb: SeekBar?) {}
        override fun onStopTrackingTouch(sb: SeekBar?) {}
    }

    // 単純な値スライダー(太陽高度・ev・月補正等)を Material Slider で統一する(仕様8)。
    // 内部は従来通り 0..maxStep の整数段で扱う。gradient=true で明暗バー下地用に
    // トラックを透明化(左側の塗りつぶしを消す)。つまみは丸(●)。
    private fun setupValueSlider(id: Int, maxStep: Int, gradient: Boolean = false,
                                 thumbRes: Int = R.drawable.thumb_dot, onChange: (Int) -> Unit) {
        val s = findViewById<Slider>(id)
        s.valueFrom = 0f; s.valueTo = maxStep.toFloat(); s.stepSize = 1f
        s.isTickVisible = false; s.labelBehavior = LabelFormatter.LABEL_GONE
        s.setCustomThumbDrawable(thumbRes)
        if (gradient) { s.trackActiveTintList = transparentTint; s.trackInactiveTintList = transparentTint }
        s.addOnChangeListener { _, v, _ -> onChange(v.toInt()) }
    }
    private fun setSliderProgress(id: Int, p: Int) {
        val s = findViewById<Slider>(id); s.value = p.toFloat().coerceIn(s.valueFrom, s.valueTo)
    }
    private fun sliderProgress(id: Int): Int = findViewById<Slider>(id).value.toInt()

    // --- 露出値スライダー(iso/ss/fn 文字列配列から選択) ---

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun jsonToList(a: JSONArray?): List<String> {
        val l = mutableListOf<String>()
        if (a != null) for (i in 0 until a.length()) l.add(a.optString(i))
        return l
    }

    // 設定可能な露出値(カメラ設定値の文字列)を Entity から取得して保持する。
    private fun loadExpoValues() {
        try {
            val o = JSONObject(HgeNative.nativeGetExpoValues())
            isoValues = jsonToList(o.optJSONArray("iso"))
            ssValues = jsonToList(o.optJSONArray("ss"))
            fnValues = jsonToList(o.optJSONArray("fn"))
        } catch (_: Exception) {}
        // 表示順: 左=暗い時→右=明るい時。iso/ss は反転、fn はそのまま。
        isoDisp = isoValues.reversed()
        ssDisp = ssValues.reversed()
        fnDisp = fnValues
    }

    private fun buildExposureEditors() {
        fixEditor = ExposureEditor(findViewById(R.id.edit_fix_container))
        moonInitEditor = ExposureEditor(findViewById(R.id.moon_init_container))
        editLimit = LimitEditor(findViewById(R.id.edit_limit_container))
        moonLimit = LimitEditor(findViewById(R.id.moon_limit_container))
    }

    // 左=暗(月)→右=明(太陽) を示す明暗バー(仕様5)。スライダーの下地に敷く。
    private fun brightnessBar(): android.graphics.drawable.GradientDrawable {
        val d = android.graphics.drawable.GradientDrawable(
            android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
            intArrayOf(0xFF263238.toInt(), 0xFF90A4AE.toInt(), 0xFFFFF9C4.toInt())
        )
        d.cornerRadius = dp(5).toFloat()
        return d
    }

    // 露出スライダーの行コンテナ(薄いカード)を作る。
    private fun sliderColumn(parent: LinearLayout): LinearLayout {
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        val colLp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        colLp.setMargins(0, dp(4), 0, dp(4)); col.layoutParams = colLp
        col.setPadding(dp(4), dp(2), dp(4), dp(4))
        parent.addView(col)
        return col
    }

    // Material スライダーのトラックを透明にし、明暗バーの上に重ねた行を作る。
    // showIcons=true で両端に月/太陽を置く。端のつまみが画面端に来ないよう左右に余白(仕様8)。
    private fun sliderWithIcons(parent: LinearLayout, slider: View, showIcons: Boolean = true) {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(dp(16), 0, dp(16), 0)
        if (showIcons) {
            val moon = TextView(this); moon.text = "🌙"; moon.textSize = 16f
            moon.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            row.addView(moon)
        }
        val frame = FrameLayout(this)
        frame.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        val bar = View(this); bar.background = brightnessBar()
        val barLp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10))
        barLp.gravity = Gravity.CENTER_VERTICAL; barLp.setMargins(dp(10), 0, dp(10), 0)
        bar.layoutParams = barLp
        frame.addView(bar)
        slider.layoutParams = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        frame.addView(slider)
        row.addView(frame)
        if (showIcons) {
            val sun = TextView(this); sun.text = "☀"; sun.textSize = 16f
            sun.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            row.addView(sun)
        }
        parent.addView(row)
    }

    private val transparentTint get() = ColorStateList.valueOf(0x00000000)

    // 単一露出を選ぶ1本スライダー行。タイトルは左寄せ(仕様7)、値は右に表示。
    // 夜間固定露出・月の開始時露出に使う。
    private inner class SingleRow(parent: LinearLayout, title: String, private val vals: List<String>) {
        private val slider = Slider(this@MainActivity)
        private val valTv: TextView
        init {
            val col = sliderColumn(parent)
            // タイトル(左)と値(センター)を同じ行に。タイトルが値を表す形「ISO感度  1600」。
            val head = FrameLayout(this@MainActivity)
            head.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            head.setPadding(dp(2), 0, dp(2), 0)
            val titleTv = TextView(this@MainActivity)
            titleTv.text = title; titleTv.textSize = 14f; titleTv.setTypeface(null, Typeface.BOLD)
            titleTv.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.START or Gravity.CENTER_VERTICAL)
            valTv = TextView(this@MainActivity)
            valTv.textSize = 18f; valTv.setTypeface(null, Typeface.BOLD); valTv.gravity = Gravity.CENTER
            valTv.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER)
            head.addView(titleTv); head.addView(valTv); col.addView(head)

            slider.valueFrom = 0f
            slider.valueTo = (vals.size - 1).coerceAtLeast(1).toFloat()
            slider.stepSize = 1f
            slider.isTickVisible = false
            slider.labelBehavior = LabelFormatter.LABEL_GONE
            slider.trackActiveTintList = transparentTint
            slider.trackInactiveTintList = transparentTint
            slider.setCustomThumbDrawable(R.drawable.thumb_dot)
            slider.addOnChangeListener { _, value, _ -> valTv.text = vals.getOrElse(value.toInt()) { "" } }
            sliderWithIcons(col, slider, showIcons = false)   // 夜間固定露出・月の開始時露出は外側アイコン無し
        }
        fun set(value: String) {
            val idx = vals.indexOf(value).let { if (it < 0) 0 else it }
            slider.value = idx.toFloat().coerceIn(slider.valueFrom, slider.valueTo)
            valTv.text = vals.getOrElse(idx) { "" }
        }
        fun get(): String = vals.getOrElse(slider.value.toInt()) { "" }
    }

    // 単一露出(iso/ss/fn)を3スライダーで編集する。夜間固定露出・月の開始時露出に使う。
    private inner class ExposureEditor(container: LinearLayout) {
        private val isoRow = SingleRow(container, "ISO感度", isoDisp)
        private val ssRow = SingleRow(container, "シャッター速度", ssDisp)
        private val fnRow = SingleRow(container, "F値", fnDisp)

        fun set(o: JSONObject?) {
            isoRow.set(o?.optString("iso") ?: "")
            ssRow.set(o?.optString("ss") ?: "")
            fnRow.set(o?.optString("fn") ?: "")
        }
        fun get(): JSONObject = JSONObject()
            .put("iso", isoRow.get()).put("ss", ssRow.get()).put("fn", fnRow.get())
    }

    // 露出限界エディタ(仕様7 / §7.4.3): 項目(ISO/SS/F)ごとに「暗所限界〜明所限界の範囲スライダー」
    // を1枚のカードに置く。カードの並び順 = 優先度(上ほど先に変化)。
    // 並べ替えは左端のドラッグハンドルを上下に動かし、挿入位置バーを離した所へ移動(仕様3)。
    // priority値は exposureType(iso=0,ss=1,fn=2)。bright=limitBright=暗所限界 / dark=limitDark=明所限界。
    private inner class LimitEditor(private val container: LinearLayout) {
        private val order = mutableListOf(0, 1, 2)
        private val darkPlace = HashMap<Int, String>()    // 暗所限界 = limitBright(高ISO側・左つまみ)
        private val brightPlace = HashMap<Int, String>()  // 明所限界 = limitDark(低ISO側・右つまみ)
        private val keys = listOf("iso", "ss", "fn")
        private var initialBright = true                  // 初期値=明所限界か(仕様4d)
        private var moonMode = false                      // 月撮影時露出限界(暗所=夜間値で固定・明所のみ編集・仕様6d/e)
        private val cards = mutableListOf<View>()
        private val dividers = mutableListOf<View>()
        private val darkTvs = HashMap<Int, TextView>(); private val brightTvs = HashMap<Int, TextView>()
        private val initTvs = HashMap<Int, TextView>()
        private var dragFrom = -1

        fun set(brightObj: JSONObject?, darkObj: JSONObject?, prio: JSONArray?, initBright: Boolean,
                moonMode: Boolean = false, nightLimit: JSONObject? = null) {
            order.clear()
            if (prio != null) for (i in 0 until prio.length()) order.add(prio.optInt(i))
            if (order.sorted() != listOf(0, 1, 2)) { order.clear(); order.addAll(listOf(0, 1, 2)) }
            this.moonMode = moonMode
            for (t in 0..2) {
                // 月モードでは暗所限界=夜間撮影の設定値(固定)。通常は limitBright。
                darkPlace[t] = (if (moonMode) nightLimit?.optString(keys[t]) else brightObj?.optString(keys[t])) ?: ""
                brightPlace[t] = darkObj?.optString(keys[t]) ?: ""     // limitDark = 明所限界
            }
            initialBright = initBright
            render()
        }
        fun getBright(): JSONObject = JSONObject().put("iso", darkPlace[0]).put("ss", darkPlace[1]).put("fn", darkPlace[2])
        fun getDark(): JSONObject = JSONObject().put("iso", brightPlace[0]).put("ss", brightPlace[1]).put("fn", brightPlace[2])
        fun getPriority(): JSONArray { val a = JSONArray(); order.forEach { a.put(it) }; return a }
        fun getInitialBright(): Boolean = initialBright

        private fun valsFor(t: Int) = when (t) { 0 -> isoDisp; 1 -> ssDisp; else -> fnDisp }
        private fun nameFor(t: Int) = when (t) { 0 -> "ISO感度"; 1 -> "シャッター速度"; else -> "F値" }
        private fun updateVals(t: Int) {
            darkTvs[t]?.text = darkPlace[t]; brightTvs[t]?.text = brightPlace[t]
            initTvs[t]?.text = if (initialBright) brightPlace[t] else darkPlace[t]
        }
        private fun refreshInit() { for (t in 0..2) initTvs[t]?.text = if (initialBright) brightPlace[t] else darkPlace[t] }
        private fun makeValTv(color: Int): TextView {
            val tv = TextView(this@MainActivity); tv.textSize = 15f; tv.setTypeface(null, Typeface.BOLD)
            tv.gravity = Gravity.CENTER; tv.setTextColor(color)
            tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            return tv
        }

        // 上部に「初期値チェック」と列見出し(一か所)、続けて divider 挟みのカード(並べ替え対象)。
        private fun render() {
            container.removeAllViews()
            cards.clear(); dividers.clear(); darkTvs.clear(); brightTvs.clear(); initTvs.clear()
            if (!moonMode) {   // 月モードは初期値チェックも暗所/初期値/明所の見出しも無し(明所限界のみ)
                val cb = CheckBox(this@MainActivity)
                cb.text = "明所限界を初期値にする"; cb.isChecked = initialBright
                cb.setOnCheckedChangeListener { _, c -> initialBright = c; refreshInit() }
                container.addView(cb)
                container.addView(headerRow())
            }
            for (i in 0..order.size) {
                val div = View(this@MainActivity)
                div.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4))
                div.setBackgroundColor(0x00000000)
                dividers.add(div); container.addView(div)
                if (i < order.size) { val card = buildCard(i); cards.add(card); container.addView(card) }
            }
        }

        private fun headerRow(): View {
            val row = LinearLayout(this@MainActivity); row.orientation = LinearLayout.HORIZONTAL
            row.setPadding(dp(4), dp(2), dp(4), 0)
            fun col(text: String, weight: Float) {
                val tv = TextView(this@MainActivity); tv.text = text; tv.textSize = 11f
                tv.setTextColor(0xFF666666.toInt()); tv.gravity = Gravity.CENTER
                tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight)
                row.addView(tv)
            }
            col("", 1.5f); col("暗所限界", 1f); col("初期値", 1f); col("明所限界", 1f); col("", 1f)
            return row
        }

        private fun buildCard(i: Int): LinearLayout {
            val t = order[i]; val vals = valsFor(t)
            val card = LinearLayout(this@MainActivity); card.orientation = LinearLayout.VERTICAL
            card.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            card.setBackgroundColor(0xFFF2EEFA.toInt())
            card.setPadding(dp(4), dp(2), dp(4), dp(4))

            // 行1: 名称(左) + 数値。通常は 暗所/初期値/明所 の3値、月モードは明所限界のみ(仕様4g/6e)。
            val valRow = LinearLayout(this@MainActivity); valRow.orientation = LinearLayout.HORIZONTAL
            valRow.gravity = Gravity.CENTER_VERTICAL
            val name = TextView(this@MainActivity); name.text = nameFor(t); name.textSize = 14f; name.setTypeface(null, Typeface.BOLD)
            name.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.5f)
            val brightTv = makeValTv(0xFF222222.toInt())
            valRow.addView(name)
            if (moonMode) {
                valRow.addView(brightTv)   // 明所限界のみ(センター付近)
                val rspacer = View(this@MainActivity); rspacer.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.5f)
                valRow.addView(rspacer)
                brightTvs[t] = brightTv
            } else {
                val darkTv = makeValTv(0xFF222222.toInt()); val initTv = makeValTv(0xFF1565C0.toInt())
                val rspacer = View(this@MainActivity); rspacer.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                valRow.addView(darkTv); valRow.addView(initTv); valRow.addView(brightTv); valRow.addView(rspacer)
                darkTvs[t] = darkTv; brightTvs[t] = brightTv; initTvs[t] = initTv
            }
            card.addView(valRow)

            // 行2: ドラッグハンドル(左) + 範囲スライダー(右へ・少し短く)(仕様4f)
            val slRow = LinearLayout(this@MainActivity); slRow.orientation = LinearLayout.HORIZONTAL; slRow.gravity = Gravity.CENTER_VERTICAL
            val handle = TextView(this@MainActivity)
            handle.text = "⇅"; handle.textSize = 22f; handle.gravity = Gravity.CENTER
            handle.setBackgroundColor(0xFFD1C4E9.toInt()); handle.setPadding(dp(4), dp(2), dp(4), dp(2))
            handle.layoutParams = LinearLayout.LayoutParams(dp(40), dp(36))
            handle.setOnTouchListener(dragTouch(i))
            slRow.addView(handle)
            val frame = FrameLayout(this@MainActivity)
            val frameLp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f); frameLp.setMargins(dp(6), 0, dp(4), 0)
            frame.layoutParams = frameLp
            val bar = View(this@MainActivity); bar.setBackgroundResource(R.drawable.brightness_bar)
            val barLp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10)); barLp.gravity = Gravity.CENTER_VERTICAL; barLp.setMargins(dp(10), 0, dp(10), 0)
            bar.layoutParams = barLp; frame.addView(bar)
            val rs = RangeSlider(this@MainActivity)
            rs.layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            rs.valueFrom = 0f; rs.valueTo = (vals.size - 1).coerceAtLeast(1).toFloat(); rs.stepSize = 1f
            rs.isTickVisible = false; rs.labelBehavior = LabelFormatter.LABEL_GONE
            rs.trackActiveTintList = transparentTint; rs.trackInactiveTintList = transparentTint
            // 月モード: 左=グレー●(夜間値で固定)/右=月。通常: 左=月(暗所)/右=太陽(明所)。
            if (moonMode) rs.setCustomThumbDrawablesForValues(R.drawable.thumb_dot_gray, R.drawable.ic_moon)
            else rs.setCustomThumbDrawablesForValues(R.drawable.ic_moon, R.drawable.ic_sun)
            val di = vals.indexOf(darkPlace[t]).let { if (it < 0) 0 else it }   // 暗所限界(月モードは夜間値=固定位置)
            val bi = vals.indexOf(brightPlace[t]).let { if (it < 0) vals.size - 1 else it }
            rs.values = listOf(minOf(di, bi).toFloat(), maxOf(di, bi).toFloat())
            var lock = false
            rs.addOnChangeListener { _, _, _ ->
                if (lock) return@addOnChangeListener
                val v = rs.values
                if (moonMode) {
                    if (v[0].toInt() != di) {   // 左(暗所=夜間値)が動いたら固定位置へ戻す
                        lock = true
                        rs.values = listOf(di.toFloat(), maxOf(v[1], di.toFloat()))
                        lock = false
                    }
                    brightPlace[t] = vals.getOrElse(rs.values[1].toInt()) { "" }
                } else {
                    darkPlace[t] = vals.getOrElse(v[0].toInt()) { "" }
                    brightPlace[t] = vals.getOrElse(v[1].toInt()) { "" }
                }
                updateVals(t)
            }
            frame.addView(rs); slRow.addView(frame)
            card.addView(slRow)
            updateVals(t)
            rs.post {   // 明暗バーをつまみ可動域に合わせる
                val tsp = rs.trackSidePadding
                val lp = bar.layoutParams as FrameLayout.LayoutParams
                lp.setMargins(tsp, lp.topMargin, tsp, lp.bottomMargin); bar.layoutParams = lp
            }
            return card
        }

        // ドラッグハンドルのタッチ処理。挿入位置(divider)を色で示し、離した位置へ並べ替える。
        private fun dragTouch(index: Int) = View.OnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.parent?.requestDisallowInterceptTouchEvent(true)   // ScrollView のスクロールを抑止
                    dragFrom = index; highlightGap(ev.rawY); true
                }
                MotionEvent.ACTION_MOVE -> { highlightGap(ev.rawY); true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { dropAt(ev.rawY); true }
                else -> false
            }
        }

        // 指のY座標から挿入位置(0..order.size)を求める。
        private fun gapFor(rawY: Float): Int {
            val loc = IntArray(2); container.getLocationOnScreen(loc)
            val y = rawY - loc[1]
            var g = 0
            for (c in cards) { if (c.top + c.height / 2f < y) g++ }
            return g.coerceIn(0, order.size)
        }
        private fun highlightGap(rawY: Float) {
            val g = gapFor(rawY)
            for (k in dividers.indices) {
                dividers[k].setBackgroundColor(if (k == g) 0xFF1565C0.toInt() else 0x00000000)
            }
        }
        private fun dropAt(rawY: Float) {
            val g = gapFor(rawY)
            val from = dragFrom
            dragFrom = -1
            // タッチ処理中にツリーを作り替えると描画が壊れるので、次フレームへ遅延する。
            container.post {
                if (from in order.indices) {
                    val item = order.removeAt(from)
                    val insertAt = (if (g > from) g - 1 else g).coerceIn(0, order.size)
                    order.add(insertAt, item)
                }
                render()
            }
        }
    }

    // 簡易カラーピッカー(R/G/B スライダー + プレビュー)。
    private fun showColorPicker(initial: Int, onPick: (Int) -> Unit) {
        val ctx = this
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(ctx); root.orientation = LinearLayout.VERTICAL; root.setPadding(pad, pad, pad, pad)
        val preview = View(ctx)
        preview.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (40 * resources.displayMetrics.density).toInt())
        var r = (initial shr 16) and 0xFF; var g = (initial shr 8) and 0xFF; var b = initial and 0xFF
        fun cur() = (r shl 16) or (g shl 8) or b
        fun refresh() { preview.setBackgroundColor(0xFF000000.toInt() or cur()) }
        refresh(); root.addView(preview)
        fun bar(label: String, init: Int, set: (Int) -> Unit): SeekBar {
            val t = TextView(ctx); t.text = label; root.addView(t)
            val s = SeekBar(ctx); s.max = 255; s.progress = init
            s.setOnSeekBarChangeListener(seekListener { set(it); refresh() })
            root.addView(s); return s
        }
        bar("赤 R", r) { r = it }; bar("緑 G", g) { g = it }; bar("青 B", b) { b = it }
        androidx.appcompat.app.AlertDialog.Builder(ctx)
            .setTitle("色の設定")
            .setView(root)
            .setPositiveButton("OK") { _, _ -> onPick(cur()) }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun pickDate(cal: Calendar) {
        DatePickerDialog(this, { _, y, m, d ->
            cal.set(Calendar.YEAR, y); cal.set(Calendar.MONTH, m); cal.set(Calendar.DAY_OF_MONTH, d)
            clampPlanRange(cal === startCal); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
    }

    private fun pickTime(cal: Calendar) {
        TimePickerDialog(this, { _, h, min ->
            cal.set(Calendar.HOUR_OF_DAY, h); cal.set(Calendar.MINUTE, min); cal.set(Calendar.SECOND, 0)
            clampPlanRange(cal === startCal); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
    }

    // 開始/終了の間隔を 最小1分〜最大24時間 に収め、逆転(終了≦開始)を防ぐ。
    // startChanged=true(開始を編集) のときは終了を、false(終了を編集) のときは開始を動かす。
    private fun clampPlanRange(startChanged: Boolean) {
        val minMs = 60_000L              // 最小1分
        val maxMs = 24L * 60L * 60_000L  // 最大24時間
        val span = endCal.timeInMillis - startCal.timeInMillis
        if (span in minMs..maxMs) return
        if (startChanged) {
            // 開始を基準に終了を範囲内へ寄せる。
            val clamped = if (span < minMs) minMs else maxMs
            endCal.timeInMillis = startCal.timeInMillis + clamped
        } else {
            // 終了を基準に開始を範囲内へ寄せる。
            val clamped = if (span < minMs) minMs else maxMs
            startCal.timeInMillis = endCal.timeInMillis - clamped
        }
    }

    private fun updateTimeButtons() {
        startDate.text = fmtDate.format(startCal.time)
        startTime.text = fmtTime.format(startCal.time)
        endDate.text = fmtDate.format(endCal.time)
        endTime.text = fmtTime.format(endCal.time)
    }

    // 開始/終了時刻をEntityへ渡してスケジュールを再生成させる(結果はEV_SCHEDULEで反映)。
    private fun pushTimesToEntity() {
        val s = fmtIso.format(startCal.time)
        val e = fmtIso.format(endCal.time)
        val off = TimeZone.getDefault().getOffset(startCal.timeInMillis) / 60000
        Thread { HgeNative.nativeSetPlanTimes(s, e, off) }.start()
    }

    // 撮影方向/仰角をEntityへ渡してスケジュールを再生成させる(結果はEV_SCHEDULEで反映)。
    private fun pushDirectionToEntity(az: Float, el: Float) {
        dirText.text = "撮影方向 %.1f°   仰角 %.1f°".format(az, el)
        Thread { HgeNative.nativeSetPlanDirection(az.toDouble(), el.toDouble()) }.start()
    }

    override fun onDestroy() {
        handler.removeCallbacks(edgePoll)
        HgeNative.nativeSetListener(null)
        HgeNative.nativeCaptureStop()
        HgeNative.nativeTerm()
        super.onDestroy()
    }

    // --- Entity通知 ---
    override fun onHgeEvent(event: Int, json: String) {
        runOnUiThread {
            when (event) {
                HgeNative.EV_STATE -> {
                    val st = JSONObject(json).optInt("state", HgeNative.ST_IDLE)
                    capState.text = "state: ${HgeNative.stateName(st)}"
                    // ローカル撮影が終了/失敗したら計画画面へ戻す
                    if (!edgeCapturing && flipper.displayedChild == 1 &&
                        (st == HgeNative.ST_IDLE || st == HgeNative.ST_ERROR)) {
                        flipper.displayedChild = 0
                    }
                }
                HgeNative.EV_PROGRESS -> {
                    val o = JSONObject(json)
                    capProgress.text = "frame ${o.optInt("frame")}/${o.optInt("total")}  " +
                        "elapsed ${o.optInt("elapsedSec")}s  remain ${o.optInt("remainSec")}s"
                }
                HgeNative.EV_CAPTURED -> {
                    val o = JSONObject(json)
                    capCaptured.text = "iso ${o.optString("iso")}  ss ${o.optString("ss")}  f ${o.optString("fn")}"
                }
                HgeNative.EV_SCHEDULE -> { latestSchedule = json; updatePlanDisplay(json) }
                HgeNative.EV_DEVICE -> {}
                HgeNative.EV_ERROR -> {
                    val o = JSONObject(json)
                    capState.text = "ERROR ${o.optString("msg")}"
                }
            }
        }
    }

    // スケジュールJSON(静的フィールド+events+windows)から両画面の表示を更新する。
    private fun updatePlanDisplay(json: String) {
        try {
            val o = JSONObject(json)
            planName.text = o.optString("name")
            capName.text = o.optString("name")
            placeText.text = o.optString("place")
            latlngText.text = o.optString("latlng") + "  標高 " + o.optInt("altitude") + "m"
            cameraText.text = "カメラ: " + o.optString("camera")
            lensText.text = "レンズ: " + o.optString("lens")
            sensorText.text = "センサー %.1f×%.1fmm  焦点距離 %d mm  画角 %.0f×%.0f°".format(
                o.optDouble("sensorW"), o.optDouble("sensorH"), o.optInt("focalLength"),
                o.optDouble("fovH"), o.optDouble("fovV"))
            val land = if (o.optBoolean("landscape")) "横" else "縦"
            intervalText.text = "撮影周期: ${o.optInt("interval")}秒   $land 向き"
            // 撮影方向/仰角ウィジェットへ反映(setterは無音=コールバックを呼ばない)
            val az = o.optDouble("azimuth", 90.0).toFloat()
            val el = o.optDouble("elevation", 10.0).toFloat()
            compass.setAzimuth(az)
            compass.setFov(o.optDouble("fovH", 80.0).toFloat())
            compass.setMarkers(
                o.optDouble("sunriseAz", Double.NaN).toFloat(),
                o.optDouble("sunsetAz", Double.NaN).toFloat(),
                o.optDouble("moonriseAz", Double.NaN).toFloat(),
                o.optDouble("moonsetAz", Double.NaN).toFloat())
            elevationView.setAngle(el)
            elevationView.setFov(o.optDouble("fovV", 50.0).toFloat())
            dirText.text = "撮影方向 %.1f°   仰角 %.1f°".format(az, el)
            capGear.text = o.optString("camera") + " / " + o.optString("lens")
            capDir.text = dirText.text
            renderSchedule(planSchedule, o, false)
            renderSchedule(capSchedule, o, true)
            buildPlanCcmList(o)   // 右側の色別ccmリスト(この計画の撮影制御方法)
        } catch (_: Exception) {}
    }

    private fun ccmColor(type: Int): Int = when (type) {
        1 -> Color.parseColor("#B39DDB")   // 夜間
        2 -> Color.parseColor("#FFF59D")   // 朝日
        3 -> Color.parseColor("#FFCC80")   // 夕日
        4 -> Color.parseColor("#90CAF9")   // 日中
        5 -> Color.parseColor("#CE93D8")   // 月対処
        6 -> Color.parseColor("#A5D6A7")   // リニア移行
        else -> Color.parseColor("#EEEEEE")
    }

    private fun eventName(ev: Int): String = when (ev) {
        1 -> "Start"; 2 -> "日の入り"; 3 -> "市民薄明(夕)"; 4 -> "航海薄明(夕)"; 5 -> "天文薄明(夕)"
        6 -> "天文薄明(朝)"; 7 -> "航海薄明(朝)"; 8 -> "市民薄明(朝)"; 9 -> "日の出"
        10 -> "月の出"; 11 -> "月の入り"; 12 -> "End"; else -> "?"
    }

    // 時系列の行(イベント=灰、撮影制御方法の開始=色付き)を並べて描画する。
    private fun renderSchedule(container: LinearLayout, o: JSONObject, highlightNow: Boolean) {
        container.removeAllViews()
        data class Row(val t: Long, val time: String, val label: String, val color: Int)
        val rows = mutableListOf<Row>()
        fun parse(s: String): Long = try { fmtIso.parse(s)?.time ?: 0L } catch (_: Exception) { 0L }
        fun hm(s: String): String = if (s.length >= 16) s.substring(5, 16).replace("T", " ") else s

        // 撮影制御方法の開始マーカーは挟まず、イベント(日の出/薄明等)のみを時系列表示する。
        // 撮影制御方法は右側の色別リスト(buildPlanCcmList)に出す。
        o.optJSONArray("events")?.let { arr ->
            for (i in 0 until arr.length()) {
                val e = arr.getJSONObject(i)
                val w = e.optString("when")
                rows.add(Row(parse(w), hm(w), eventName(e.optInt("event")), ccmColor(0)))
            }
        }
        rows.sortBy { it.t }
        val now = System.currentTimeMillis()
        // 現在が含まれる行(直近で過去)を求める
        var activeIdx = -1
        if (highlightNow) { for (i in rows.indices) { if (rows[i].t <= now) activeIdx = i } }

        for ((i, r) in rows.withIndex()) {
            val tv = TextView(this)
            tv.text = "${r.time}   ${r.label}"
            tv.setBackgroundColor(r.color)
            tv.setPadding(16, 8, 16, 8)
            tv.textSize = 13f
            val lp = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            lp.setMargins(0, 1, 0, 0)
            tv.layoutParams = lp
            if (highlightNow && i == activeIdx) {
                tv.text = "▶ ${r.time}   ${r.label}  (現在)"
                tv.setTypeface(null, Typeface.BOLD)
            }
            container.addView(tv)
        }
    }

    // --- エッジ端末 ---
    private fun selectedEdge(): Edge? {
        val i = edgeSpinner.selectedItemPosition
        return if (i in 1..edges.size) edges[i - 1] else null
    }

    private fun refreshEdgeSpinner() {
        val labels = mutableListOf("無し (スマホで撮影)")
        edges.forEach { labels.add("${it.name} (${it.ip})") }
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, labels)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        edgeSpinner.adapter = adapter
    }

    private fun onEdgesFound(jsonArray: String) {
        edges.clear()
        try {
            val arr = JSONArray(jsonArray)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                edges.add(Edge(o.optString("name", "エッジ端末"), o.optString("ip"), o.optInt("port", 50506)))
            }
        } catch (_: Exception) {}
        refreshEdgeSpinner()
    }

    private val edgePoll = object : Runnable {
        override fun run() {
            val e = selectedEdge() ?: return
            Thread {
                val js = HgeNative.nativeEdgeProgress(e.ip, e.port)
                runOnUiThread {
                    if (js.isNotEmpty()) {
                        try {
                            val o = JSONObject(js)
                            capState.text = "エッジ端末: ${HgeNative.stateName(o.optInt("state"))}"
                            capProgress.text = "frame ${o.optInt("frame")}/${o.optInt("total")}  " +
                                "elapsed ${o.optInt("elapsedSec")}s  remain ${o.optInt("remainSec")}s"
                            capCaptured.text = "ccm ${o.optString("ccm")}  iso ${o.optString("iso")}  " +
                                "ss ${o.optString("ss")}  f ${o.optString("fn")}"
                        } catch (_: Exception) {}
                    }
                }
            }.start()
            if (edgeCapturing) handler.postDelayed(this, 3000)
        }
    }

    private fun startOnEdge(e: Edge) {
        // 時刻同期(C_TIME)はエッジ端末の時計を「現在時刻」に合わせるためのもの。
        // 計画開始時刻(startCal)を送るとエッジが now=開始時刻と誤認し、開始前でも即撮影してしまう。
        // 計画の start/end は別途 C_CAPTURE_PLAN(getPlanJson)で渡るので、ここは現在時刻を送る。
        val nowCal = Calendar.getInstance()
        val s = fmtIso.format(nowCal.time)
        val off = TimeZone.getDefault().getOffset(nowCal.timeInMillis) / 60000
        Thread {
            val r = HgeNative.nativeEdgeStart(e.ip, e.port, s, off)
            runOnUiThread {
                if (r == 0) {
                    edgeCapturing = true
                    flipper.displayedChild = 1
                    capState.text = "エッジ端末へ転送・撮影開始 ${e.ip}"
                    handler.postDelayed(edgePoll, 2000)
                } else {
                    capState.text = "エッジ端末 開始失敗 (code=$r)"
                }
            }
        }.start()
    }

    private fun stopOnEdge(e: Edge) {
        Thread {
            HgeNative.nativeEdgeStop(e.ip, e.port)
            runOnUiThread {
                edgeCapturing = false
                handler.removeCallbacks(edgePoll)
                flipper.displayedChild = 0
            }
        }.start()
    }
}
