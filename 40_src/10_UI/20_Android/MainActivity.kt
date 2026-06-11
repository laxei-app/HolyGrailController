package app.laxei.holygrail

import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import android.widget.ViewFlipper
import androidx.appcompat.app.AppCompatActivity
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
    private lateinit var dirText: TextView
    private lateinit var planSchedule: LinearLayout
    private lateinit var planStartButton: Button
    private lateinit var edgeSpinner: Spinner
    private lateinit var edgeSearchButton: Button
    private lateinit var searchButton: Button
    private lateinit var ipInput: EditText
    private lateinit var connectButton: Button

    // 撮影制御方法初期値: メニュー + 方法別エディタ
    private lateinit var planMenu: ImageView
    private var ccmJson: JSONObject? = null     // /asset/ccmDefaults.json 全体
    private var editingKey = "night"            // 編集中の方法
    private var editColor = 0                    // 編集中の色(0xRRGGBB)

    // 露出(iso/ss/fn)はカメラ設定値の文字列配列からスライダーで選択する。
    private var isoValues = listOf<String>()    // hge_getExpoValues の iso 配列
    private var ssValues = listOf<String>()     // 同 ss 配列
    private var fnValues = listOf<String>()     // 同 fn 配列(レンズf範囲)
    private lateinit var fixEditor: ExposureEditor      // 夜間 固定露出
    private lateinit var lbEditor: ExposureEditor       // 自動露出 明側上限
    private lateinit var ldEditor: ExposureEditor       // 自動露出 暗側下限
    private lateinit var moonInitEditor: ExposureEditor // 月 開始時露出
    private lateinit var moonLbEditor: ExposureEditor   // 月 明側上限
    private lateinit var moonLdEditor: ExposureEditor   // 月 暗側下限

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

        // 初期: 開始=現在、終了=2時間後
        endCal.add(Calendar.HOUR_OF_DAY, 2)
        updateTimeButtons()
        wireListeners()
        pushTimesToEntity()    // スケジュール自動生成→通知で表示
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
        dirText = findViewById(R.id.plan_dirText)
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
        findViewById<ImageView>(R.id.edit_back).setOnClickListener { flipper.displayedChild = 2 }
        findViewById<Button>(R.id.edit_save).setOnClickListener { saveCcmEdit() }
        findViewById<Button>(R.id.edit_color_btn).setOnClickListener {
            showColorPicker(editColor) { c -> editColor = c; findViewById<View>(R.id.edit_color_swatch).setBackgroundColor(0xFF000000.toInt() or c) }
        }
        // スライダーの値ラベル更新
        findViewById<SeekBar>(R.id.edit_alt_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.edit_alt_val).text = String.format("%.1f°", seekToAlt(it))
        })
        findViewById<SeekBar>(R.id.edit_ev_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f", seekToEv(it))
        })
        // 月の影響への対処
        findViewById<ImageView>(R.id.moon_back).setOnClickListener { flipper.displayedChild = 2 }
        findViewById<Button>(R.id.moon_save).setOnClickListener { saveMoonEdit() }
        findViewById<Button>(R.id.moon_color_btn).setOnClickListener {
            showColorPicker(editColor) { c -> editColor = c; findViewById<View>(R.id.moon_color_swatch).setBackgroundColor(0xFF000000.toInt() or c) }
        }
        findViewById<SeekBar>(R.id.moon_startlum_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", it * 0.1)
        })
        findViewById<SeekBar>(R.id.moon_ev_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -it * 0.1)
        })
        findViewById<SeekBar>(R.id.moon_extcoef_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + it * 0.01)
        })
        findViewById<SeekBar>(R.id.moon_skycoef_seek).setOnSeekBarChangeListener(seekListener {
            findViewById<TextView>(R.id.moon_skycoef_val).text = "$it%"
        })
        val moonModes = arrayOf("補正しない", "画角全体の明るさで自動補正", "月に露出を合わせる")
        val ma = ArrayAdapter(this, android.R.layout.simple_spinner_item, moonModes)
        ma.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        val moonSpinner = findViewById<Spinner>(R.id.moon_mode)
        moonSpinner.adapter = ma
        moonSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: android.widget.AdapterView<*>?, v: View?, pos: Int, id: Long) {
                findViewById<View>(R.id.moon_auto_section).visibility = if (pos == 1) View.VISIBLE else View.GONE
                findViewById<View>(R.id.moon_exp_section).visibility = if (pos == 2) View.VISIBLE else View.GONE
            }
            override fun onNothingSelected(p: android.widget.AdapterView<*>?) {}
        }
    }

    private fun openMoonEdit() {
        val o = ccmJson?.optJSONObject("moon") ?: return
        editColor = o.optInt("color", 0)
        findViewById<View>(R.id.moon_color_swatch).setBackgroundColor(0xFF000000.toInt() or editColor)
        findViewById<Spinner>(R.id.moon_mode).setSelection(o.optInt("mode", 0))
        val slP = (o.optDouble("startLuminance", 0.0) * 10).toInt().coerceIn(0, 30)
        findViewById<SeekBar>(R.id.moon_startlum_seek).progress = slP
        findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", slP * 0.1)
        val evP = (-o.optDouble("ev", 0.0) * 10).toInt().coerceIn(0, 100)
        findViewById<SeekBar>(R.id.moon_ev_seek).progress = evP
        findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -evP * 0.1)
        val ecP = ((o.optDouble("extinctionCoef", 0.2) - 0.1) * 100).toInt().coerceIn(0, 50)
        findViewById<SeekBar>(R.id.moon_extcoef_seek).progress = ecP
        findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + ecP * 0.01)
        val skP = o.optDouble("skyBrightnessCoef", 100.0).toInt().coerceIn(0, 100)
        findViewById<SeekBar>(R.id.moon_skycoef_seek).progress = skP
        findViewById<TextView>(R.id.moon_skycoef_val).text = "$skP%"
        findViewById<CheckBox>(R.id.moon_atmext).isChecked = o.optBoolean("atmosphericExtinction", false)
        findViewById<CheckBox>(R.id.moon_geocorr).isChecked = o.optBoolean("geocentricCorrection", false)
        moonInitEditor.set(o.optJSONObject("initialExposure"))
        moonLbEditor.set(o.optJSONObject("limitBright"))
        moonLdEditor.set(o.optJSONObject("limitDark"))
        flipper.displayedChild = 4
    }

    private fun saveMoonEdit() {
        val all = ccmJson ?: return
        val o = all.optJSONObject("moon") ?: return
        o.put("color", editColor)
        o.put("mode", findViewById<Spinner>(R.id.moon_mode).selectedItemPosition)
        o.put("startLuminance", findViewById<SeekBar>(R.id.moon_startlum_seek).progress * 0.1)
        o.put("ev", -findViewById<SeekBar>(R.id.moon_ev_seek).progress * 0.1)
        o.put("extinctionCoef", 0.1 + findViewById<SeekBar>(R.id.moon_extcoef_seek).progress * 0.01)
        o.put("skyBrightnessCoef", findViewById<SeekBar>(R.id.moon_skycoef_seek).progress.toDouble())
        o.put("atmosphericExtinction", findViewById<CheckBox>(R.id.moon_atmext).isChecked)
        o.put("geocentricCorrection", findViewById<CheckBox>(R.id.moon_geocorr).isChecked)
        o.put("initialExposure", moonInitEditor.get())
        o.put("limitBright", moonLbEditor.get())
        o.put("limitDark", moonLdEditor.get())
        val r = HgeNative.nativeSetCcmDefaults(all.toString())
        Toast.makeText(this, if (r == 0) "保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
        flipper.displayedChild = 2
    }

    // --- 撮影制御方法 初期値: メニュー + 方法別エディタ ---

    private fun openCcmMenu() {
        ccmJson = try { JSONObject(HgeNative.nativeGetCcmDefaults()) } catch (e: Exception) { null }
        flipper.displayedChild = 2
    }

    // 太陽高度: SeekBar 0..140 ⇔ -19.0..-5.0°(0.1刻み)
    private fun altToSeek(v: Double) = ((v + 19.0) * 10.0).toInt().coerceIn(0, 140)
    private fun seekToAlt(p: Int) = -19.0 + p * 0.1
    // ev: SeekBar 0..30 ⇔ -5.0..+5.0(1/3刻み)
    private fun evToSeek(v: Double) = ((v + 5.0) * 3.0).toInt().coerceIn(0, 30)
    private fun seekToEv(p: Int) = -5.0 + p / 3.0

    private fun openCcmEdit(key: String) {
        val o = ccmJson?.optJSONObject(key) ?: return
        editingKey = key
        val title = mapOf("night" to "夜間撮影", "sunrise" to "朝日撮影", "sunset" to "夕日撮影", "day" to "日中撮影")[key]
        findViewById<TextView>(R.id.edit_title).text = title
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
            val p = altToSeek(o.optDouble("sunAltitude", -18.0))
            findViewById<SeekBar>(R.id.edit_alt_seek).progress = p
            findViewById<TextView>(R.id.edit_alt_val).text = String.format("%.1f°", seekToAlt(p))
        }
        if (hasEv) {
            val p = evToSeek(o.optDouble("ev", 0.0))
            findViewById<SeekBar>(R.id.edit_ev_seek).progress = p
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f", seekToEv(p))
        }
        if (isNight) {
            findViewById<CheckBox>(R.id.edit_autoEdge).isChecked = o.optBoolean("autoEdge", true)
            fixEditor.set(o.optJSONObject("limitBright"))
        } else {
            lbEditor.set(o.optJSONObject("limitBright"))
            ldEditor.set(o.optJSONObject("limitDark"))
        }
        flipper.displayedChild = 3
    }

    private fun saveCcmEdit() {
        val all = ccmJson ?: return
        val o = all.optJSONObject(editingKey) ?: return
        o.put("color", editColor)
        if (editingKey != "day") o.put("sunAltitude", seekToAlt(findViewById<SeekBar>(R.id.edit_alt_seek).progress))
        if (editingKey != "night") o.put("ev", seekToEv(findViewById<SeekBar>(R.id.edit_ev_seek).progress))
        if (editingKey == "night") {
            o.put("autoEdge", findViewById<CheckBox>(R.id.edit_autoEdge).isChecked)
            val e = fixEditor.get()
            o.put("limitBright", e)
            o.put("limitDark", JSONObject(e.toString()))   // 固定露出は明暗同値
        } else {
            o.put("limitBright", lbEditor.get())
            o.put("limitDark", ldEditor.get())
        }
        val r = HgeNative.nativeSetCcmDefaults(all.toString())
        Toast.makeText(this, if (r == 0) "保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
        flipper.displayedChild = 2
    }

    // SeekBar 値変更だけ拾う簡易リスナ。
    private fun seekListener(onChange: (Int) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) = onChange(p)
        override fun onStartTrackingTouch(sb: SeekBar?) {}
        override fun onStopTrackingTouch(sb: SeekBar?) {}
    }

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
    }

    private fun buildExposureEditors() {
        fixEditor = ExposureEditor(findViewById(R.id.edit_fix_container))
        lbEditor = ExposureEditor(findViewById(R.id.edit_lb_container))
        ldEditor = ExposureEditor(findViewById(R.id.edit_ld_container))
        moonInitEditor = ExposureEditor(findViewById(R.id.moon_init_container))
        moonLbEditor = ExposureEditor(findViewById(R.id.moon_lb_container))
        moonLdEditor = ExposureEditor(findViewById(R.id.moon_ld_container))
    }

    // iso/ss/fn の3行スライダーをコンテナに動的生成し、文字列配列から選択させる。
    // 保存時は選択中の文字列(カメラ設定値)をそのまま JSON に書く。
    private inner class ExposureEditor(container: LinearLayout) {
        private val isoRow = Row(container, "ISO", isoValues)
        private val ssRow = Row(container, "SS", ssValues)
        private val fnRow = Row(container, "F", fnValues)

        private inner class Row(container: LinearLayout, label: String, val vals: List<String>) {
            val seek = SeekBar(this@MainActivity)
            val valTv = TextView(this@MainActivity)
            init {
                val row = LinearLayout(this@MainActivity)
                row.orientation = LinearLayout.HORIZONTAL
                row.gravity = Gravity.CENTER_VERTICAL
                val lab = TextView(this@MainActivity)
                lab.text = label
                lab.layoutParams = LinearLayout.LayoutParams(dp(40), ViewGroup.LayoutParams.WRAP_CONTENT)
                seek.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                seek.max = (vals.size - 1).coerceAtLeast(0)
                valTv.layoutParams = LinearLayout.LayoutParams(dp(64), ViewGroup.LayoutParams.WRAP_CONTENT)
                valTv.gravity = Gravity.END
                seek.setOnSeekBarChangeListener(seekListener { valTv.text = vals.getOrElse(it) { "" } })
                row.addView(lab); row.addView(seek); row.addView(valTv)
                container.addView(row)
            }
            fun set(value: String) {
                val idx = vals.indexOf(value).let { if (it < 0) 0 else it }
                seek.progress = idx
                valTv.text = vals.getOrElse(idx) { "" }   // 同値でリスナが発火しない場合に備え明示
            }
            fun get(): String = vals.getOrElse(seek.progress) { "" }
        }

        fun set(o: JSONObject?) {
            isoRow.set(o?.optString("iso") ?: "")
            ssRow.set(o?.optString("ss") ?: "")
            fnRow.set(o?.optString("fn") ?: "")
        }

        fun get(): JSONObject = JSONObject()
            .put("iso", isoRow.get()).put("ss", ssRow.get()).put("fn", fnRow.get())
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
            updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
    }

    private fun pickTime(cal: Calendar) {
        TimePickerDialog(this, { _, h, min ->
            cal.set(Calendar.HOUR_OF_DAY, h); cal.set(Calendar.MINUTE, min); cal.set(Calendar.SECOND, 0)
            updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
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
            val land = if (o.optBoolean("landscape")) "横" else "縦"
            intervalText.text = "撮影周期: ${o.optInt("interval")}秒   $land 向き"
            dirText.text = "撮影方向: ${o.optDouble("azimuth")}°   仰角: ${o.optDouble("elevation")}°"
            capGear.text = o.optString("camera") + " / " + o.optString("lens")
            capDir.text = dirText.text
            renderSchedule(planSchedule, o, false)
            renderSchedule(capSchedule, o, true)
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

        o.optJSONArray("events")?.let { arr ->
            for (i in 0 until arr.length()) {
                val e = arr.getJSONObject(i)
                val w = e.optString("when")
                rows.add(Row(parse(w), hm(w), eventName(e.optInt("event")), ccmColor(0)))
            }
        }
        o.optJSONArray("windows")?.let { arr ->
            for (i in 0 until arr.length()) {
                val w = arr.getJSONObject(i)
                val st = w.optString("start")
                rows.add(Row(parse(st), hm(st), "▼ " + w.optString("name"), ccmColor(w.optInt("type"))))
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
        val s = fmtIso.format(startCal.time)
        val off = TimeZone.getDefault().getOffset(startCal.timeInMillis) / 60000
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
