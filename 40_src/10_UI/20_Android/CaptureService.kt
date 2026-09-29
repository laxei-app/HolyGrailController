package app.laxei.twylapse

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder

// ================= 撮影中はアプリを閉じても続ける(2026-09-29 依頼) =================
// 【なぜ要るか】これまでは画面を閉じると MainActivity.onDestroy が nativeCaptureStop/nativeTerm を
//  呼んでいたので、**閉じた時点で撮影が終わって**いた。一晩置くアプリでこれは困る。
//
// 【仕組み】Android で「アプリを閉じても動き続ける」正式な方法はフォアグラウンドサービスだけ。
//  サービスが動いている間はプロセスが生かされ、撮影のスレッド(ネイティブ側)がそのまま走り続ける。
//  戻ってきたときは**同じプロセスの続き**なので、撮り直しにはならない。
//
// 【通知は消せない】これが引き換え。Android は「見えないのに動いている」ことを許す代わりに、
//  動作中を示す通知を必ず出させる。隠す手段は無く、隠そうとする作りは審査でも弾かれる。
//  **むしろ利点**として扱う — 撮影中だと分かる/タップで戻れる/中止できる/OSに殺されにくい。
//
// 【1サービス・1通知・複数行(ユーザー決定)】撮影が何本走っていても通知は1つにまとめ、本文を
//  複数行にして全部並べる。計画ごとに通知を分けると、**サービス本体の通知以外は指で消せてしまい**、
//  「消したのに動いている」という分かりにくさが出る。
//
// 【対象(第1段階)】スマホが自分で撮っているもののうち、**外部カメラ(通信で撮る)だけ**。
//  ・外部端末(エッジ)に任せた計画 … そもそも要らない。エッジが自分で撮っている
//  ・内蔵カメラ … **カメラ型のサービスが要る**(第2段階)。型が違うとカメラを使えないので、
//    いまは対象にしない。対象にしないかぎり従来どおり「閉じたら終わる」で、
//    **中途半端に生かして黙って撮れなくなる**よりは安全
//
// 【種類は connectedDevice】外部カメラを Wi-Fi で操作するので「外部機器とのやり取り」に当たる。
//  dataSync は Android 15 で**1日6時間の上限**が入っており、夕方〜明け方は超えてしまう。
//  **この上限の扱いは規定が変わりやすいので、実機で通しの確認が要る。**
class CaptureService : Service() {

    companion object {
        const val ACTION_STOP_ALL = "app.laxei.twylapse.action.STOP_ALL"
        private const val CHANNEL_ID = "capture"
        private const val NOTE_ID = 4801
        private const val EXTRA_LINES = "lines"
        private const val EXTRA_CAMERA = "camera"   // 内蔵カメラで撮っている撮影が含まれるか

        // サービスが動いているか。MainActivity.onDestroy が「畳んでよいか」を判断するのに使う。
        @Volatile @JvmStatic var running = false
            private set

        // 走っている撮影の一覧を渡す。空なら止める。**状態が変わるたびに呼ぶ**(開始・終了・枚数の更新)。
        //  既に動いていれば通知の中身だけが差し替わる(startForeground を呼び直すのが作法)。
        @JvmStatic
        fun apply(ctx: Context, lines: List<String>, needCamera: Boolean = false) {
            val app = ctx.applicationContext
            if (lines.isEmpty()) {
                if (running) app.stopService(Intent(app, CaptureService::class.java))
                return
            }
            val i = Intent(app, CaptureService::class.java)
                .putStringArrayListExtra(EXTRA_LINES, ArrayList(lines))
                .putExtra(EXTRA_CAMERA, needCamera)
            // 【必ず前面として始めること】撮影の開始は画面を見ながら押すので、この時点では
            //  アプリが見えている。**見えている間に始めたサービスだけ**が、閉じたあとも
            //  カメラや機器へのアクセスを続けられる。
            try {
                if (Build.VERSION.SDK_INT >= 26) app.startForegroundService(i) else app.startService(i)
            } catch (_: Exception) {
                // 前面でない等で断られることがある。撮影自体は続くので握りつぶす(閉じたら終わるだけ)。
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    // 動き出した時刻(端末の稼働時間で測る。利用者が時計を変えてもずれない)。
    //  止まるときに「何時間動いたか」をログへ残す。**上限に当たったのか、メーカーの省電力に
    //  殺されたのか、正常に終わったのかを、後から記録だけで見分けるため**。
    private var startedAtMs = 0L

    private fun ranMinutes(): Long =
        if (startedAtMs == 0L) 0L else (android.os.SystemClock.elapsedRealtime() - startedAtMs) / 60000L


    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP_ALL) {
            // 通知の「すべて中止」。画面が無くても止められるのが要点。
            //  UI が生きていれば EV_STATE の通知で一覧のアイコンも戻る。
            Thread { runCatching { HgeNative.nativeCaptureStop() } }.start()
            running = false
            stopForegroundCompat()
            stopSelf()
            return START_NOT_STICKY
        }
        val lines = intent?.getStringArrayListExtra(EXTRA_LINES) ?: arrayListOf()
        if (lines.isEmpty()) { running = false; stopForegroundCompat(); stopSelf(); return START_NOT_STICKY }
        ensureChannel()
        goForeground(buildNote(lines), intent?.getBooleanExtra(EXTRA_CAMERA, false) == true)
        if (startedAtMs == 0L) {
            startedAtMs = android.os.SystemClock.elapsedRealtime()
            logEvent("start n=" + lines.size, false)
        }
        running = true
        // START_NOT_STICKY にする。OS に殺されて作り直されても、**撮影の状態はプロセスと一緒に
        //  失われている**ので、空のサービスだけが蘇っても意味が無い。その場合は次の起動で
        //  /asset/capturing.json からの再開(resumePhoneCapture)に任せる。
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        // 正常に止めたときも、外から殺されたときも通る(殺されると通らないこともある)。
        logEvent("stop after " + ranMinutes() + "min", false)
        running = false
        super.onDestroy()
    }

    // ================= 時間の上限に当たったとき(2026-09-29 依頼) =================
    // Android 15 から、一部の種類のフォアグラウンドサービスに**1日6時間の上限**が入った。
    // 上限に達すると「もう終わりです」とここへ知らせが来る。**数秒以内に自分で止めないと
    // ANR 扱いで落とされる**ので、行儀よく畳む。
    //
    // 【いまは当たらない見込み】上限は dataSync / mediaProcessing が対象で、こちらは
    //  connectedDevice。しかも**この手の制限は targetSdk がその版に上がってから効く**のが
    //  通例で、いまの targetSdk は 34。**それでも入れておく**のは、呼ばれたときに
    //  「黙って死んだ」ではなく**記録が残る**ようにするため。保険としては安い。
    //
    // 【2引数の版に override を付けていない理由】Android 15 の
    //  onTimeout(startId, fgsType) は compileSdk 35 でないと見えない(いまは 34。
    //  上げるには AGP も上げる必要がある)。**名前と引数が同じなら実行時には上書きされる**ので、
    //  override を付けずに同じ形で置いてある。compileSdk を 35 へ上げたときに override を付ける。
    override fun onTimeout(startId: Int) {
        handleTimeout(-1)
    }

    fun onTimeout(startId: Int, fgsType: Int) {
        handleTimeout(fgsType)
    }

    private fun handleTimeout(fgsType: Int) {
        val min = ranMinutes()
        logEvent("timeout after " + min + "min type=" + fgsType, true)
        // 撮影を先に止める。**止めてから畳むこと** — 落とされてから止まると、
        //  撮影レポートが書かれないまま終わる。
        runCatching { HgeNative.nativeCaptureStop() }
        // 前面の通知は消えるので、消せる通知で理由を残す。黙って終わったように見せない。
        runCatching {
            val nm = getSystemService(NotificationManager::class.java)
            val open = PendingIntent.getActivity(this, 2,
                Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
            val n = androidx.core.app.NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_note_capture)
                .setContentTitle("撮影を終えました")
                .setContentText("端末の制限（連続して動ける時間の上限）に達したため終了しました")
                .setStyle(androidx.core.app.NotificationCompat.BigTextStyle()
                    .bigText("端末の制限（連続して動ける時間の上限）に達したため、" +
                             (if (min > 0) "約${min / 60}時間${min % 60}分で" else "") +
                             "撮影を終了しました。アプリを開いたままにしておくと、この制限は働きません。"))
                .setContentIntent(open)
                .setAutoCancel(true)
                .build()
            nm?.notify(NOTE_ID + 1, n)
        }
        running = false
        stopForegroundCompat()
        stopSelf()
    }

    // 撮影ログへ残す(端末に残る記録。撮影レポートと突き合わせて読む)。
    private fun logEvent(msg: String, err: Boolean) {
        runCatching { HgeNative.nativeLogEvent("FGS", msg, err) }
    }

    // ================= サービスの種類(2026-09-30 依頼: 内蔵カメラ対応) =================
    // 【内蔵カメラには「カメラ」型が要る】Android は、裏に回ったアプリからのカメラ利用を
    //  塞いでいる。**カメラ型のフォアグラウンドサービスが動いている間だけ**、閉じたあとも
    //  カメラを使い続けられる。connectedDevice だけだと、プロセスは生きているのに
    //  カメラが開けず**黙って撮れなくなる**(第1段階で内蔵カメラを対象外にしていた理由)。
    //
    // 【見えている間に始めること】この決まりは「アプリが見えている間に始めたサービス」に
    //  だけ効く。撮影の開始は画面を見ながら押すので条件を満たす。
    //  **種類は最初に決まったまま変わらない** — 閉じている間に新しい撮影は始められないので、
    //  内蔵カメラが混じるかどうかが途中で変わることはない。
    //
    // 【カメラ権限が無ければカメラ型を名乗らない】Android 14 以降、権限が無いのに
    //  カメラ型を名乗ると SecurityException で落とされる。内蔵カメラの撮影は開始時に
    //  権限を求めるので通常は持っているが、保険として落とす。
    private fun goForeground(note: Notification, needCamera: Boolean) {
        var type = 0
        if (Build.VERSION.SDK_INT >= 29) {
            type = android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            val camOk = androidx.core.content.ContextCompat.checkSelfPermission(
                this, android.Manifest.permission.CAMERA) == android.content.pm.PackageManager.PERMISSION_GRANTED
            if (needCamera && camOk) {
                type = type or android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
            } else if (needCamera) {
                logEvent("camera type skipped: no CAMERA permission", true)
            }
        }
        try {
            androidx.core.app.ServiceCompat.startForeground(this, NOTE_ID, note, type)
        } catch (e: Exception) {
            // 種類を名乗れなかった。撮影を止めはしないが、閉じたら続かないので記録は残す。
            logEvent("startForeground failed: " + e, true)
            runCatching { startForeground(NOTE_ID, note) }
        }
    }

    private fun stopForegroundCompat() {
        try {
            if (Build.VERSION.SDK_INT >= 24) stopForeground(STOP_FOREGROUND_REMOVE)
            else @Suppress("DEPRECATION") stopForeground(true)
        } catch (_: Exception) {}
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < 26) return
        val nm = getSystemService(NotificationManager::class.java) ?: return
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        // 音も振動も出さない。一晩中出しっぱなしになる通知なので、鳴らすと寝られない。
        val ch = NotificationChannel(CHANNEL_ID, "撮影中", NotificationManager.IMPORTANCE_LOW)
        ch.description = "撮影を続けている間だけ出ます"
        ch.setShowBadge(false)
        ch.enableVibration(false)
        ch.setSound(null, null)
        nm.createNotificationChannel(ch)
    }

    private fun buildNote(lines: List<String>): Notification {
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val stop = PendingIntent.getService(this, 1,
            Intent(this, CaptureService::class.java).setAction(ACTION_STOP_ALL),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)

        val title = if (lines.size == 1) "撮影中" else "${lines.size}件 撮影中"
        val body = lines.joinToString("\n")
        val b = androidx.core.app.NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_note_capture)   // 小アイコンは白一色の形でないと既定の絵に化ける
            .setContentTitle(title)
            .setContentText(lines.firstOrNull() ?: "")
            .setStyle(androidx.core.app.NotificationCompat.BigTextStyle().bigText(body))
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)          // 枚数の更新のたびに鳴らさない
            .setShowWhen(false)
            .setPriority(androidx.core.app.NotificationCompat.PRIORITY_LOW)
            .addAction(0, if (lines.size == 1) "中止" else "すべて中止", stop)
        return b.build()
    }
}
