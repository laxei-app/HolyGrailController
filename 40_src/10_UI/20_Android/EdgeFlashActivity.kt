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
// 【新品で手が要るかは機種で違う(2026-08-26 両機で実測)】
//  CoreS3 は工場出荷FWでも自動で入れられる(ハードウェアの USB-Serial/JTAG のため)。
//  StickS3 の工場出荷FWは DTR/RTS を見ていないので入れられず、**電源ボタンを長押し(2秒)
//  して LED が点滅したら**ダウンロードモード。まず自動で試し、駄目なら案内を出す作りに
//  してあるので、どちらの機種でも同じ操作で済む。
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

    /**
     * USB の抜き差しで表示を追う。ダウンロードモードへ入ると PID が変わって**別の機器**に
     * 見えるため(実測)、抜き差ししていなくても再列挙が起きる。追わないと画面が嘘をつく。
     */
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) = refreshState()
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

        val u = IntentFilter().apply {
            addAction(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(android.hardware.usb.UsbManager.ACTION_USB_DEVICE_DETACHED)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, u, Context.RECEIVER_EXPORTED)
        else registerReceiver(usbReceiver, u)

        refreshState()
    }

    override fun onResume() { super.onResume(); refreshState() }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { unregisterReceiver(permReceiver) }
        runCatching { unregisterReceiver(usbReceiver) }
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
        if (f.isStubRunning()) { log("スタブは既に載っています。"); return }
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
            // 【ここで再起動させない(2026-08-26 実機で判明)】調べ終わりに元のファームへ戻すと、
            //  ダウンロードモードから抜けてしまう。工場出荷の端末はこちらから入れ直せないので、
            //  「調べる」の直後に「書き込む」を押しても、また手で長押しする羽目になる。
            //  調べた後はそのまま焼ける状態で置いておくのが素直。
            log("ダウンロードモードのままにしてあります。続けて「書き込む」を押せます。")
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
            val want = EspFlasher.md5hex(image)
            val got = runCatching { f.flashMd5(entry.offset, image.size) }
                .getOrElse { log("照合できません: ${it.message}"); "" }
            when {
                got.isEmpty() || got != want -> {
                    // 形が読めない/合わないときは、生の応答をそのまま残す。
                    val raw = runCatching { f.flashMd5Raw(entry.offset, image.size) }
                        .getOrElse { ByteArray(0) }
                    log("応答 %d バイト: %s".format(raw.size,
                        raw.take(40).joinToString("") { "%02x".format(it) }))
                }
                else -> {}
            }
            when {
                got.isEmpty() ->
                    // 端末が MD5 を計算できない場合。書き込み自体はブロックごとに検査値を
                    //  付けて送っており、落としたファームも SHA256 で確かめてある。
                    //  照合できないことだけを残して先へ進む(黙って成功にはしない)。
                    log("※ 端末側の照合はできませんでした。書き込みは完了しています。")
                got != want -> {
                    // 頭の 4KB だけでも比べる。頭が合っていれば「後ろが違う」、
                    //  頭から違えば「そもそも別物」と切り分けられる。
                    val headWant = EspFlasher.md5hex(image.copyOfRange(0, 0x1000))
                    val headGot = runCatching { f.flashMd5(entry.offset, 0x1000) }.getOrElse { "取得失敗" }
                    log("全体 期待 $want")
                    log("全体 端末 $got")
                    log("頭4KB 期待 $headWant")
                    log("頭4KB 端末 $headGot")
                    throw EspFlashError("書き込んだ中身が一致しません。")
                }
                else -> log("一致しました。")
            }

            // DTR/RTS のリセットではダウンロードモードから抜けられない(実測)。
            f.watchdogReset()
            log("--- 完了。端末が起動します ---")
            log("設定は消えているので、BLE で端末名と Wi-Fi を入れ直してください。")
        }
    }
}
