package app.laxei.holygrail

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// 買ってきたエッジ端末へ、スマホから USB でファームを書き込む画面(2026-08-26)。
//
// 【ここが唯一の道である理由】新品には自分たちのファームが入っていない。OTA も STA での
//  自己更新も「今動いているファームが受け取って書く」仕組みなので、最初の1回には使えない。
//
// 【手順】
//  1. USB でつなぐ(Pixel なら普通の USB-C ケーブルでよい。OTG 用の特別な線は要らない)
//  2. 「調べる」… 1バイトも書かずに MAC と容量を読む。容量で機種が決まる
//  3. 「書き込む」… 公開リポジトリから落として照合し、焼いて、起動させる
//
// 【新品は手でダウンロードモードへ入れてもらう(実測)】工場出荷のファームは DTR/RTS を
//  見ていないので、こちらからは入れられない。**電源ボタンを長押し(2秒)して LED が
//  点滅したら**ダウンロードモード。私たちのファームが入った後は自動で入れられる。
class EdgeFlashActivity : AppCompatActivity() {

    private val ui = Handler(Looper.getMainLooper())
    private lateinit var stateView: TextView
    private lateinit var logView: TextView
    private lateinit var logScroll: ScrollView
    private lateinit var bar: ProgressBar
    private lateinit var probeBtn: Button
    private lateinit var writeBtn: Button

    @Volatile private var busy = false
    private var pendingAfterPermission: (() -> Unit)? = null

    /** USB を使ってよいかの答えはここへ返る。 */
    private val permReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) {
            if (i?.action != EspUsb.permissionAction()) return
            val ok = i.getBooleanExtra(android.hardware.usb.UsbManager.EXTRA_PERMISSION_GRANTED, false)
            val go = pendingAfterPermission
            pendingAfterPermission = null
            if (ok && go != null) go() else if (!ok) log("USB の使用が許可されませんでした。")
        }
    }

    override fun onCreate(saved: Bundle?) {
        super.onCreate(saved)
        setContentView(R.layout.activity_edge_flash)
        stateView = findViewById(R.id.fl_state)
        logView = findViewById(R.id.fl_log)
        logScroll = findViewById(R.id.fl_logScroll)
        bar = findViewById(R.id.fl_bar)
        probeBtn = findViewById(R.id.fl_probe)
        writeBtn = findViewById(R.id.fl_write)

        findViewById<ImageView>(R.id.fl_back).setOnClickListener { finish() }
        probeBtn.setOnClickListener { withDevice { dev -> runOffUi { doProbe(dev) } } }
        writeBtn.setOnClickListener { withDevice { dev -> runOffUi { doWrite(dev) } } }

        val f = IntentFilter(EspUsb.permissionAction())
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(permReceiver, f, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(permReceiver, f)

        refreshState()
    }

    override fun onResume() { super.onResume(); refreshState() }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { unregisterReceiver(permReceiver) }
    }

    // ── 画面 ────────────────────────────────────────────────

    private fun refreshState() {
        val dev = EspUsb.findDevice(this)
        stateView.text = if (dev == null) {
            "USB ケーブルでエッジ端末をつないでください。\n" +
            "(Pixel なら普通の USB-C ケーブルで大丈夫です)"
        } else {
            "つながっています: %s\n%s".format(dev.deviceName, EspUsb.describe(dev))
        }
        probeBtn.isEnabled = dev != null && !busy
        writeBtn.isEnabled = dev != null && !busy
    }

    private fun log(msg: String) = ui.post {
        val t = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        logView.append("$t  $msg\n")
        logScroll.post { logScroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun progress(done: Int, total: Int) = ui.post {
        bar.progress = if (total > 0) (done.toLong() * 100 / total).toInt() else 0
    }

    /** 端末が居て、使ってよいと言われていれば block を呼ぶ。まだなら許可を聞く。 */
    private fun withDevice(block: (UsbDevice) -> Unit) {
        val dev = EspUsb.findDevice(this)
        if (dev == null) { log("USB につながっている端末が見つかりません。"); return }
        if (EspUsb.hasPermission(this, dev)) { block(dev); return }
        log("USB の使用許可を求めています…")
        pendingAfterPermission = { block(dev) }
        EspUsb.requestPermission(this, dev)
    }

    private fun runOffUi(body: () -> Unit) {
        if (busy) return
        busy = true
        ui.post { refreshState() }
        Thread {
            try { body() }
            catch (e: Exception) { log("失敗: ${e.message}") }
            finally { busy = false; ui.post { refreshState() } }
        }.start()
    }

    // ── 手順の中身 ──────────────────────────────────────────

    /**
     * ダウンロードモードの端末と話せる状態にする。
     * 私たちのファームが入っていれば自動で入れられる。工場出荷のままなら案内を出す。
     */
    private fun connect(dev: UsbDevice): Pair<EspUsbSerial, EspFlasher> {
        val port = EspUsbSerial.open(this, dev)
        val f = EspFlasher(port)
        try {
            runCatching { f.sync(retries = 3) }.onSuccess {
                log("ダウンロードモードの端末と話せました。")
                return port to f
            }
            // 私たちのファームなら DTR/RTS で入れられる。工場出荷のままだと効かない。
            log("自動でダウンロードモードへ入れてみます…")
            port.usbJtagResetToDownload()
            runCatching { f.sync(retries = 5) }.onSuccess {
                log("ダウンロードモードに入りました。")
                return port to f
            }
            throw EspFlashError(
                "ダウンロードモードに入れません。\n" +
                "端末の電源ボタンを2秒ほど長押しして、LED が点滅したら、もう一度押してください。"
            )
        } catch (e: Exception) {
            port.close()
            throw e
        }
    }

    /** スタブを載せる。載らなくても ROM だけで焼けるので、失敗しても止めない。 */
    private fun tryStub(f: EspFlasher) {
        runCatching { f.runStub(EdgeFirmware.loadStub(this)) }
            .onSuccess { log("高速化のためのスタブを載せました。") }
            .onFailure { log("スタブを載せられませんでした。ROM だけで進めます(遅くなります)。") }
    }

    /** 1バイトも書かずに素性を読む。ここが通れば USB まわりは全部通っている。 */
    private fun doProbe(dev: UsbDevice) {
        val (port, f) = connect(dev)
        port.use {
            tryStub(f)
            val info = f.probe()
            val mb = info.flashSizeBytes / 1024 / 1024
            log("MAC        : ${info.mac}")
            log("フラッシュ : ${if (mb > 0) "${mb}MB" else "不明"} (RDID 0x%06X)".format(info.flashId))
            val model = when (info.flashSizeBytes) {
                8 * 1024 * 1024 -> "M5StickS3"
                16 * 1024 * 1024 -> "M5Stack CoreS3"
                else -> "判別できません"
            }
            log("機種       : $model")
            log("--- 何も書き込んでいません ---")
            // 調べただけなので、元のファームへ戻しておく
            runCatching { f.watchdogReset() }
        }
    }

    /** 目録を見て、機種に合うものを落として、確かめて、焼く。 */
    private fun doWrite(dev: UsbDevice) {
        log("目録を取得しています…")
        val list = try {
            EdgeFirmware.fetchManifest()
        } catch (e: Exception) {
            log("目録を取れません。エッジ端末の Wi-Fi につながっていると外に出られません。")
            throw e
        }
        log("目録: ${list.size} 機種")

        val (port, f) = connect(dev)
        port.use {
            tryStub(f)
            val info = f.probe()
            val mb = info.flashSizeBytes / 1024 / 1024
            log("端末: MAC ${info.mac} / フラッシュ ${if (mb > 0) "${mb}MB" else "不明"}")

            val entry = EdgeFirmware.pickFor(info.flashSizeBytes, list)
                ?: throw EdgeFirmwareError(
                    "この端末(フラッシュ ${if (mb > 0) "${mb}MB" else "不明"})に合うファームが目録にありません。" +
                    "別機種のものを焼くと起動しなくなるので、ここで止めます。"
                )
            log("選んだファーム: ${entry.name} 版 ${entry.version} (${entry.build})")

            log("ファームを落としています…")
            val image = EdgeFirmware.download(entry) { done, total -> progress(done, total) }
            log("照合しました (${image.size} バイト)")

            log("書き込みます。**抜かないでください**")
            f.writeFlash(entry.offset, image) { phase, done, total ->
                if (phase == "write") progress(done, total)
            }
            progress(100, 100)

            log("端末側で照合しています…")
            if (!f.verify(entry.offset, image)) {
                throw EspFlashError("書き込んだ中身が一致しません。もう一度お試しください。")
            }
            log("一致しました。")

            // DTR/RTS のリセットではダウンロードモードから抜けられない(実測)。
            f.watchdogReset()
            log("--- 完了。端末が起動します ---")
            log("設定は消えているので、BLE で端末名と Wi-Fi を入れ直してください。")
        }
    }
}
