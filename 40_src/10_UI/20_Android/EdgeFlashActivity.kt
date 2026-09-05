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
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
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
//  1. USB でつなぐ(端子が同じならケーブル1本。合わないときだけ変換が要る)
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

        // 【書き込み中は閉じさせない(2026-09-05 UI依頼)】書き込みは別スレッドで走っており、
        //  画面を閉じても止まらない。閉じると経過が見えないまま裏で続き、失敗しても気づけない。
        //  デバッグログの取得中と同じ扱いにする。端末の戻るキーも同じ(下の onBackPressed)。
        findViewById<ImageView>(R.id.fl_home).setOnClickListener { leaveTo(true) }
        findViewById<ImageView>(R.id.fl_menu).setOnClickListener { leaveTo(false) }
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

        applyOrientation()
        refreshState()
    }

    // 横向きは 左=書き込み中の状況(進み具合とログ) / 右=説明とボタン に分ける(2026-08-30 UI依頼)。
    //  横へ広げただけだと、ボタンや説明が上へ積まれてログが画面外へ押し出され、
    //  書き込み中の様子が見えなくなるため。
    // レイアウトXMLを縦横で分けないのは、この画面も configChanges で回転を受けていて
    //  作り直しが起きないから(横向きの形が縦向きに残ってしまう)。付け替えで対応する。
    private var flSplit: LinearLayout? = null
    private val flLp = HashMap<View, ViewGroup.LayoutParams>()   // 縦向きへ戻すときの元の指定

    private fun dpi(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun applyOrientation() {
        val root = findViewById<LinearLayout>(R.id.fl_root) ?: return
        val land = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
        val state = findViewById<View>(R.id.fl_state) ?: return
        val btns = findViewById<View>(R.id.fl_buttons) ?: return
        val note = findViewById<View>(R.id.fl_note) ?: return
        val barV = findViewById<View>(R.id.fl_bar) ?: return
        val logSc = findViewById<View>(R.id.fl_logScroll) ?: return
        val mp = ViewGroup.LayoutParams.MATCH_PARENT
        val wc = ViewGroup.LayoutParams.WRAP_CONTENT
        val parts = listOf(state, btns, note, barV, logSc)
        if (land && flSplit == null) {
            for (v in parts) { flLp[v] = v.layoutParams; root.removeView(v) }
            val left = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            val right = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            left.addView(barV, LinearLayout.LayoutParams(mp, wc))
            left.addView(logSc, LinearLayout.LayoutParams(mp, 0, 1f))
            right.addView(state, LinearLayout.LayoutParams(mp, wc))
            right.addView(btns, LinearLayout.LayoutParams(mp, wc))
            right.addView(note, LinearLayout.LayoutParams(mp, wc))
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.addView(left, LinearLayout.LayoutParams(0, mp, 1f))
            row.addView(View(this).apply { setBackgroundColor(0xFF000000.toInt()) },
                        LinearLayout.LayoutParams(dpi(1), mp))
            row.addView(right, LinearLayout.LayoutParams(0, mp, 1f))
            root.addView(row, LinearLayout.LayoutParams(mp, 0, 1f))
            flSplit = row
        } else if (!land && flSplit != null) {
            val row = flSplit!!
            (row.getChildAt(0) as LinearLayout).removeAllViews()
            (row.getChildAt(2) as LinearLayout).removeAllViews()
            root.removeView(row)
            var i = 1                                   // 0 はヘッダ
            for (v in parts) { root.addView(v, i, flLp[v]); i++ }
            flSplit = null
        }
    }

    // 回転は configChanges で受けている(書き込み中の経過を消さないため)。並べ替えだけする。
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        applyOrientation()
    }

    override fun onResume() { super.onResume(); refreshState() }

    // ホーム/メニューへ戻る。書き込み中は断る。
    //  この画面は MainActivity とは別の画面部品なので、閉じると呼び出し元(メニュー)へ戻る。
    //  ホームのときだけ、MainActivity に「撮影計画を出す」と伝えてから閉じる。
    private fun leaveTo(home: Boolean) {
        if (busy) {
            android.widget.Toast.makeText(this, "書き込み中です。終わるまでお待ちください",
                                          android.widget.Toast.LENGTH_SHORT).show()
            return
        }
        if (home) { MainActivity.goHomeOnResume = true }
        finish()
    }

    @Deprecated("端末の戻るキー。書き込み中だけ止める")
    override fun onBackPressed() {
        if (busy) {
            android.widget.Toast.makeText(this, "書き込み中です。終わるまでお待ちください",
                                          android.widget.Toast.LENGTH_SHORT).show()
            return
        }
        @Suppress("DEPRECATION")
        super.onBackPressed()
    }

    override fun onDestroy() {
        super.onDestroy()
        runCatching { unregisterReceiver(permReceiver) }
        runCatching { unregisterReceiver(usbReceiver) }
    }

    // ── 画面 ────────────────────────────────────────────────

    private fun refreshState() {
        val dev = EspUsb.findDevice(this)
        stateView.text = if (dev == null) {
            "USB ケーブルで登録端末をつないでください。\n" +
            "登録端末側は USB-C です。スマートフォンの端子が違うときは変換ケーブル\n" +
            "(または OTG アダプタ)を使ってください。充電専用のケーブルでは通信できません。"
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

    /**
     * 書き込みの途中で人に問う。**書き込みの手順は別スレッドで動いている**ので、
     * ダイアログは画面のスレッドへ出し、答えが来るまでこちらは待つ。
     * 画面が既に無いなど出せないときは「やめる」とみなす(勝手に書かない)。
     */
    private fun confirm(title: String, msg: String): Boolean {
        val answer = java.util.concurrent.ArrayBlockingQueue<Boolean>(1)
        ui.post {
            runCatching {
                androidx.appcompat.app.AlertDialog.Builder(this)
                    .setTitle(title)
                    .setMessage(msg)
                    .setCancelable(false)
                    .setPositiveButton("書き込む") { _, _ -> answer.offer(true) }
                    .setNegativeButton("やめる") { _, _ -> answer.offer(false) }
                    .show()
            }.onFailure { answer.offer(false) }
        }
        return answer.take()
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
            log("目録を取れません。登録端末の Wi-Fi につながっていると外に出られません。")
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

            // 【端末に今入っているものを見てから決める】
            //  ・アプリの素性(名前・版数)は決まった番地にある。読み取って版数を比べる
            //  ・ブートローダと区切りが同じなら、本体だけ入れ替えても起動する。その場合
            //    NVS(0x9000〜0xE000)を跨がないので設定がそのまま残る
            //  確かめられないときは安全側(まるごと書く)へ倒す。
            val imgId = EdgeFirmware.parseIdentity(
                image.copyOfRange(FlashMap.APP_DESC, FlashMap.APP_DESC + FlashMap.APP_DESC_LEN))
            val devId = runCatching {
                EdgeFirmware.parseIdentity(f.readFlash(FlashMap.APP_DESC, FlashMap.APP_DESC_LEN))
            }.getOrElse { FwIdentity("", "", false) }
            log("端末のファーム: $devId")
            log("焼くファーム  : $imgId")

            val sameBase = runCatching {
                f.regionMatches(FlashMap.BOOTLOADER,
                                image.copyOfRange(FlashMap.BOOTLOADER, FlashMap.BOOTLOADER_END)) &&
                f.regionMatches(FlashMap.PART_TABLE,
                                image.copyOfRange(FlashMap.PART_TABLE,
                                                  FlashMap.PART_TABLE + FlashMap.PART_TABLE_LEN))
            }.getOrElse { false }

            // 【同じ版数・古い版へ戻すときは人に聞く(2026-09-05 ユーザー依頼)】
            //  以前は同じ版数だと黙って飛ばしていた。焼き直したいときに手立てが無く、
            //  古い版へ戻すときも黙って書いていたので、どちらも確かめてから進める。
            val ask = EdgeFirmware.askBefore(devId, imgId)
            if (ask != FlashAsk.NONE) {
                val body = if (ask == FlashAsk.SAME_VERSION)
                    "端末には同じ版数 ${devId.version} が入っています。\n書き込みますか?"
                else
                    "端末の版数 ${devId.version} より古い ${imgId.version} を書きます。\n古い版へ戻しますか?"
                val extra = if (sameBase) "\n\n設定は残ります。"
                            else "\n\n土台が違うのでまるごと書きます。設定は消えます。"
                if (!confirm(if (ask == FlashAsk.SAME_VERSION) "同じ版数です" else "古い版へ戻します",
                             body + extra)) {
                    log("取りやめました。")
                    log("--- 何もせずに終わります ---")
                    f.watchdogReset()
                    return@use
                }
            }

            val action = EdgeFirmware.decide(devId, imgId, sameBase,
                                             approvedSame = (ask == FlashAsk.SAME_VERSION))
            val plan = EdgeFirmware.planWrite(image, action)
            if (plan == null) {
                log("同じ版数が入っています。書き込む必要はありません。")
                log("--- 何もせずに終わります ---")
                f.watchdogReset()
                return@use
            }
            if (plan.keepsSettings) {
                log("同じ土台です。**設定を残して**本体だけ書き換えます。")
            } else if (!devId.valid) {
                log("端末の素性が読めません。土台ごと全部書きます。**設定は消えます**。")
            } else {
                log("土台が違います(区切りの変更など)。まるごと書きます。**設定は消えます**。")
            }

            // 設定が本当に残ったかを、書き込みの前後で見比べられるようにしておく。
            //  「残すつもりだったが実は消していた」を後から気づけないのが一番まずい。
            val nvsBefore = if (plan.keepsSettings) {
                runCatching { f.flashMd5(FlashMap.NVS, FlashMap.NVS_END - FlashMap.NVS) }
                    .getOrElse { "" }
            } else ""

            log("書き込みます。**抜かないでください**")
            f.writeFlash(plan.offset, plan.data) { phase, done, total ->
                if (phase == "write") progress(done, total)
            }
            progress(100, 100)

            log("端末側で照合しています…")
            val want = EspFlasher.md5hex(plan.data)
            val got = runCatching { f.flashMd5(plan.offset, plan.data.size) }
                .getOrElse { log("照合できません: ${it.message}"); "" }
            when {
                got.isEmpty() ->
                    // 端末が MD5 を計算できない場合。書き込み自体はブロックごとに検査値を
                    //  付けて送っており、落としたファームも SHA256 で確かめてある。
                    //  照合できないことだけを残して先へ進む(黙って成功にはしない)。
                    log("※ 端末側の照合はできませんでした。書き込みは完了しています。")
                got != want -> {
                    val raw = runCatching { f.flashMd5Raw(plan.offset, plan.data.size) }
                        .getOrElse { ByteArray(0) }
                    log("期待 $want")
                    log("端末 $got")
                    log("応答 %d バイト: %s".format(raw.size,
                        raw.take(40).joinToString("") { "%02x".format(it) }))
                    throw EspFlashError("書き込んだ中身が一致しません。")
                }
                else -> log("一致しました。")
            }

            if (plan.keepsSettings && nvsBefore.isNotEmpty()) {
                val nvsAfter = runCatching { f.flashMd5(FlashMap.NVS, FlashMap.NVS_END - FlashMap.NVS) }
                    .getOrElse { "" }
                if (nvsAfter == nvsBefore) log("設定の領域は手つかずです(照合値 ${nvsBefore.take(8)}…)")
                else log("※ 設定の領域が変わっています。BLE で入れ直してください。")
            }

            // DTR/RTS のリセットではダウンロードモードから抜けられない(実測)。
            f.watchdogReset()
            log("--- 完了。端末が起動します ---")
            if (plan.keepsSettings) {
                log("設定は残してあります。そのまま使えます。")
            } else {
                // 【入れ直し方まで書くこと(2026-08-27 実機で詰まった)】名前が消えると、
                //  エッジ設定の一覧で以前の名前を選んだままでは BLE で見つからない。
                //  「＋ 新規エッジ端末」を選べば名前を問わず拾う。
                log("設定は消えています。登録端末設定の「＋ 新規端末」を選んでから")
                log("QR表示を要求し、端末名と Wi-Fi を入れ直してください。")
            }
        }
    }
}
