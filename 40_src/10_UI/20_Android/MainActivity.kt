package app.laxei.holygrail

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

// 開発ステップ2.1 MVP: 画面330(撮影計画設定)/400(撮影中)を1画面に最小構成で実装する。
// 固定データの撮影計画でカメラを検索し、開始/停止のみ行う。
class MainActivity : AppCompatActivity(), HgeListener {

    private lateinit var stateText: TextView
    private lateinit var progressText: TextView
    private lateinit var capturedText: TextView
    private lateinit var scheduleText: TextView
    private lateinit var startButton: Button
    private lateinit var stopButton: Button
    private lateinit var searchButton: Button
    private lateinit var ipInput: EditText
    private lateinit var connectButton: Button
    private lateinit var edgeSpinner: Spinner
    private lateinit var edgeSearchButton: Button

    // エッジ端末(検出結果)。先頭は「無し(スマホで撮影)」。
    private data class Edge(val name: String, val ip: String, val port: Int)
    private val edges = mutableListOf<Edge>()
    private val handler = Handler(Looper.getMainLooper())
    private var edgeCapturing = false

    // エッジ端末の進捗を定期取得する。
    private val edgePoll = object : Runnable {
        override fun run() {
            val e = selectedEdge() ?: return
            Thread {
                val js = HgeNative.nativeEdgeProgress(e.ip, e.port)
                runOnUiThread { if (js.isNotEmpty()) showEdgeProgress(js) }
            }.start()
            if (edgeCapturing) handler.postDelayed(this, 3000)
        }
    }

    // 選択中のエッジ端末(「無し」なら null)。
    private fun selectedEdge(): Edge? {
        val i = edgeSpinner.selectedItemPosition
        return if (i in 1..edges.size) edges[i - 1] else null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        stateText = findViewById(R.id.stateText)
        progressText = findViewById(R.id.progressText)
        capturedText = findViewById(R.id.capturedText)
        scheduleText = findViewById(R.id.scheduleText)
        startButton = findViewById(R.id.startButton)
        stopButton = findViewById(R.id.stopButton)
        searchButton = findViewById(R.id.searchButton)
        ipInput = findViewById(R.id.ipInput)
        connectButton = findViewById(R.id.connectButton)
        edgeSpinner = findViewById(R.id.edgeSpinner)
        edgeSearchButton = findViewById(R.id.edgeSearchButton)
        refreshEdgeSpinner()

        findViewById<TextView>(R.id.titleText).text = HgeNative.nativeVersion()

        // ログ保存先(アプリ外部ファイル領域 .../files)を渡してから初期化する
        HgeNative.nativeSetLogDir(getExternalFilesDir(null)?.absolutePath ?: filesDir.absolutePath)
        HgeNative.nativeInit()
        HgeNative.nativeSetListener(this)

        // 固定データのスケジュールを表示(330 相当)
        scheduleText.text = formatSchedule(HgeNative.nativeScheduleJson())
        applyState(HgeNative.nativeGetState())

        startButton.setOnClickListener {
            progressText.text = ""
            capturedText.text = ""
            val e = selectedEdge()
            if (e == null) {
                HgeNative.nativeCaptureStart()            // スマホで撮影
            } else {
                startOnEdge(e)                            // エッジ端末へ転送して撮影
            }
        }
        stopButton.setOnClickListener {
            val e = selectedEdge()
            if (e == null) {
                HgeNative.nativeCaptureStop()
            } else {
                stopOnEdge(e)
            }
        }
        // カメラ自動検索(SSDP)。エッジ端末=無しのスマホ撮影時に使う。
        searchButton.setOnClickListener {
            stateText.text = "カメラ検索中..."
            HgeNative.nativeSearchDevices()
        }
        // エッジ端末を検索する(UDPブロードキャスト)。
        edgeSearchButton.setOnClickListener {
            stateText.text = "エッジ端末検索中..."
            Thread {
                val js = HgeNative.nativeEdgeSearch(2000)
                runOnUiThread { onEdgesFound(js) }
            }.start()
        }
        // 手動接続(SSDP不使用。エミュレータ用)。HTTP通信のためバックグラウンドスレッドで実行する。
        connectButton.setOnClickListener {
            val host = ipInput.text.toString().trim()
            stateText.text = "connecting $host ..."
            Thread { HgeNative.nativeConnectManual(host) }.start()
        }
    }

    override fun onDestroy() {
        handler.removeCallbacks(edgePoll)
        HgeNative.nativeSetListener(null)
        HgeNative.nativeCaptureStop()
        HgeNative.nativeTerm()
        super.onDestroy()
    }

    // --- エッジ端末 ---

    // スピナーを「無し」+検出済みエッジ端末で作り直す。
    private fun refreshEdgeSpinner() {
        val labels = mutableListOf("無し (スマホで撮影)")
        edges.forEach { labels.add("${it.name} (${it.ip})") }
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, labels)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        edgeSpinner.adapter = adapter
    }

    // 検索結果(edgeInfo の JSON 配列)を取り込む。
    private fun onEdgesFound(jsonArray: String) {
        edges.clear()
        try {
            val arr = JSONArray(jsonArray)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                edges.add(Edge(o.optString("name", "エッジ端末"),
                               o.optString("ip"), o.optInt("port", 50506)))
            }
        } catch (_: Exception) {}
        refreshEdgeSpinner()
        stateText.text = "エッジ端末 ${edges.size}台検出"
    }

    // 端末ローカル時刻("YYYY-MM-DDThh:mm:ss")とUTCオフセット[分]。
    private fun nowLocal(): Pair<String, Int> {
        val now = System.currentTimeMillis()
        val fmt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.US)
        val off = TimeZone.getDefault().getOffset(now) / 60000
        return Pair(fmt.format(now), off)
    }

    private fun startOnEdge(e: Edge) {
        stateText.text = "エッジ端末へ転送中... ${e.ip}"
        val (dt, off) = nowLocal()
        Thread {
            val r = HgeNative.nativeEdgeStart(e.ip, e.port, dt, off)
            runOnUiThread {
                if (r == 0) {
                    edgeCapturing = true
                    stateText.text = "エッジ端末で撮影中 ${e.ip}"
                    startButton.isEnabled = false
                    stopButton.isEnabled = true
                    handler.postDelayed(edgePoll, 2000)
                } else {
                    stateText.text = "エッジ端末 開始失敗 (code=$r)"
                }
            }
        }.start()
    }

    private fun stopOnEdge(e: Edge) {
        Thread {
            val r = HgeNative.nativeEdgeStop(e.ip, e.port)
            runOnUiThread {
                edgeCapturing = false
                handler.removeCallbacks(edgePoll)
                stateText.text = if (r == 0) "エッジ端末 停止" else "エッジ端末 停止失敗"
                startButton.isEnabled = true
                stopButton.isEnabled = false
            }
        }.start()
    }

    // エッジ端末の進捗JSONを表示に反映する。
    private fun showEdgeProgress(json: String) {
        try {
            val o = JSONObject(json)
            stateText.text = "エッジ端末: ${HgeNative.stateName(o.optInt("state"))}"
            progressText.text = "frame ${o.optInt("frame")}/${o.optInt("total")}  " +
                "elapsed ${o.optInt("elapsedSec")}s  remain ${o.optInt("remainSec")}s"
            capturedText.text = "ccm ${o.optString("ccm")}  iso ${o.optInt("iso")}  " +
                "ss ${o.optDouble("ss")}  f ${o.optDouble("fn")}"
        } catch (_: Exception) {}
    }

    // Entity からの通知(ワーカースレッド)→ UI スレッドへ post
    override fun onHgeEvent(event: Int, json: String) {
        runOnUiThread {
            when (event) {
                HgeNative.EV_STATE -> {
                    val st = JSONObject(json).optInt("state", HgeNative.ST_IDLE)
                    applyState(st)
                }
                HgeNative.EV_PROGRESS -> {
                    val o = JSONObject(json)
                    progressText.text = "frame ${o.optInt("frame")}/${o.optInt("total")}  " +
                        "elapsed ${o.optInt("elapsedSec")}s  remain ${o.optInt("remainSec")}s"
                }
                HgeNative.EV_CAPTURED -> {
                    val o = JSONObject(json)
                    capturedText.text = "captured #${o.optInt("frame")}  " +
                        "iso ${o.optInt("iso")}  ss ${o.optDouble("ss")}  f ${o.optDouble("fn")}"
                }
                HgeNative.EV_SCHEDULE -> {
                    scheduleText.text = formatSchedule(json)
                }
                HgeNative.EV_DEVICE -> {
                    stateText.text = "device: $json"
                }
                HgeNative.EV_ERROR -> {
                    val o = JSONObject(json)
                    stateText.text = "ERROR code=${o.optInt("code")} ${o.optString("msg")}"
                }
            }
        }
    }

    private fun applyState(st: Int) {
        if (edgeCapturing) return    // エッジ端末撮影中はローカル状態でボタンを上書きしない
        stateText.text = "state: ${HgeNative.stateName(st)}"
        val capturing = (st == HgeNative.ST_CAPTURING ||
                         st == HgeNative.ST_SEARCHING ||
                         st == HgeNative.ST_STOPPING)
        startButton.isEnabled = !capturing
        stopButton.isEnabled = capturing
        searchButton.isEnabled = !capturing
        connectButton.isEnabled = !capturing
    }

    private fun ccmName(type: Int): String = when (type) {
        1 -> "夜間"
        2 -> "朝日"
        3 -> "夕日"
        4 -> "日中"
        5 -> "月対処"
        6 -> "リニア移行"
        else -> "?"
    }

    // スケジュール JSON を読みやすく整形する。
    private fun formatSchedule(json: String): String {
        if (json.isEmpty()) return "(no schedule)"
        return try {
            val o = JSONObject(json)
            val sb = StringBuilder()
            sb.append("計画: ").append(o.optString("name")).append("\n")
            sb.append("周期: ").append(o.optInt("interval")).append("秒\n")
            sb.append(o.optString("start")).append(" 〜 ").append(o.optString("end")).append("\n\n")

            val ev = o.optJSONArray("events")
            if (ev != null && ev.length() > 0) {
                sb.append("[イベント]\n")
                for (i in 0 until ev.length()) {
                    val e = ev.getJSONObject(i)
                    sb.append("  ").append(eventName(e.optInt("event")))
                        .append("  ").append(e.optString("when")).append("\n")
                }
                sb.append("\n")
            }
            val win = o.optJSONArray("windows")
            if (win != null && win.length() > 0) {
                sb.append("[スケジュール]\n")
                for (i in 0 until win.length()) {
                    val w = win.getJSONObject(i)
                    sb.append("  ").append(ccmName(w.optInt("type")))
                        .append("  ").append(w.optString("start"))
                        .append(" 〜 ").append(w.optString("end")).append("\n")
                }
            }
            sb.toString()
        } catch (e: Exception) {
            json
        }
    }

    private fun eventName(ev: Int): String = when (ev) {
        1 -> "開始"
        2 -> "日の入り"
        3 -> "市民薄明(夕)"
        4 -> "航海薄明(夕)"
        5 -> "天文薄明(夕)"
        6 -> "天文薄明(朝)"
        7 -> "航海薄明(朝)"
        8 -> "市民薄明(朝)"
        9 -> "日の出"
        10 -> "月の出"
        11 -> "月の入り"
        12 -> "終了"
        else -> "?"
    }
}
