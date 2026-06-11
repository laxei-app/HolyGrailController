package app.laxei.holygrail

import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
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
                    capCaptured.text = "iso ${o.optInt("iso")}  ss ${o.optDouble("ss")}  f ${o.optDouble("fn")}"
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
                            capCaptured.text = "ccm ${o.optString("ccm")}  iso ${o.optInt("iso")}  " +
                                "ss ${o.optDouble("ss")}  f ${o.optDouble("fn")}"
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
