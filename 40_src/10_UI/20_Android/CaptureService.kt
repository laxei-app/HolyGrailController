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

        // サービスが動いているか。MainActivity.onDestroy が「畳んでよいか」を判断するのに使う。
        @Volatile @JvmStatic var running = false
            private set

        // 走っている撮影の一覧を渡す。空なら止める。**状態が変わるたびに呼ぶ**(開始・終了・枚数の更新)。
        //  既に動いていれば通知の中身だけが差し替わる(startForeground を呼び直すのが作法)。
        @JvmStatic
        fun apply(ctx: Context, lines: List<String>) {
            val app = ctx.applicationContext
            if (lines.isEmpty()) {
                if (running) app.stopService(Intent(app, CaptureService::class.java))
                return
            }
            val i = Intent(app, CaptureService::class.java)
                .putStringArrayListExtra(EXTRA_LINES, ArrayList(lines))
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
        startForeground(NOTE_ID, buildNote(lines))
        running = true
        // START_NOT_STICKY にする。OS に殺されて作り直されても、**撮影の状態はプロセスと一緒に
        //  失われている**ので、空のサービスだけが蘇っても意味が無い。その場合は次の起動で
        //  /asset/capturing.json からの再開(resumePhoneCapture)に任せる。
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
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
