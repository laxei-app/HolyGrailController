package app.laxei.holygrail

import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject

// 開発ステップ2.1 MVP: 画面330(撮影計画設定)/400(撮影中)を1画面に最小構成で実装する。
// 固定データの撮影計画でカメラを検索し、開始/停止のみ行う。
class MainActivity : AppCompatActivity(), HgeListener {

    private lateinit var stateText: TextView
    private lateinit var progressText: TextView
    private lateinit var capturedText: TextView
    private lateinit var scheduleText: TextView
    private lateinit var startButton: Button
    private lateinit var stopButton: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        stateText = findViewById(R.id.stateText)
        progressText = findViewById(R.id.progressText)
        capturedText = findViewById(R.id.capturedText)
        scheduleText = findViewById(R.id.scheduleText)
        startButton = findViewById(R.id.startButton)
        stopButton = findViewById(R.id.stopButton)

        findViewById<TextView>(R.id.titleText).text = HgeNative.nativeVersion()

        HgeNative.nativeInit()
        HgeNative.nativeSetListener(this)

        // 固定データのスケジュールを表示(330 相当)
        scheduleText.text = formatSchedule(HgeNative.nativeScheduleJson())
        applyState(HgeNative.nativeGetState())

        startButton.setOnClickListener {
            progressText.text = ""
            capturedText.text = ""
            HgeNative.nativeCaptureStart()
        }
        stopButton.setOnClickListener {
            HgeNative.nativeCaptureStop()
        }
    }

    override fun onDestroy() {
        HgeNative.nativeSetListener(null)
        HgeNative.nativeCaptureStop()
        HgeNative.nativeTerm()
        super.onDestroy()
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
        stateText.text = "state: ${HgeNative.stateName(st)}"
        val capturing = (st == HgeNative.ST_CAPTURING ||
                         st == HgeNative.ST_SEARCHING ||
                         st == HgeNative.ST_STOPPING)
        startButton.isEnabled = !capturing
        stopButton.isEnabled = capturing
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
