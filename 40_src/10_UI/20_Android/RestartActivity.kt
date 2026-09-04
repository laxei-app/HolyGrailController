package app.laxei.holygrail

import android.app.Activity
import android.content.Intent
import android.os.Bundle

/**
 * アプリを自動で再起動するためだけの画面(2026-09-05 UI依頼)。
 *
 * 【なぜ専用の画面が要るか】「出荷時設定に戻す」は**プロセスを落とすことが必須**。
 *  Entity は読み込んだ内容をメモリに抱えており(g_placesLoaded / g_planReady …)、
 *  消しただけで動き続けると次の保存で書き戻るため。
 *  ところが自分のプロセスを落とすと、そのあと自分で起動し直すことができない。
 *  AlarmManager で後から起こす手もあるが、Android 10 以降は**止まっているアプリからの
 *  画面起動が制限される**ので確実ではない。
 *
 * 【やり方】この画面だけを `android:process=":restart"` で**別プロセス**に置く。
 *  本体プロセスを落としてもこちらは生き残り、しかも「前面にある画面」からの起動になるので
 *  制限に引っかからない。
 *
 *  MainActivity → この画面を開く(本体のpidを渡す)
 *                 → 本体プロセスを落とす
 *                 → MainActivity を新しいタスクで開き直す
 *                 → 自分は消える
 *
 * この画面は何も描かない。一瞬で終わるので、ユーザーには「落ちて戻ってきた」ように見える。
 */
class RestartActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pid = intent.getIntExtra(EXTRA_PID, 0)
        // 本体を落とす。ここから先は別プロセスなので巻き込まれない。
        if (pid > 0 && pid != android.os.Process.myPid()) {
            runCatching { android.os.Process.killProcess(pid) }
        }
        // **待たずにここで起動して finish すること。** 透過テーマでも、遅らせると
        //  「画面が出たのに finish していない」と判断されて落ちる経路がある
        //  (NoDisplay テーマでは確実に落ちる。実機で踏んだ)。
        //  本体は上で kill 済みなので、ここでの起動はもう新しいプロセスになる。
        runCatching {
            startActivity(Intent(this, MainActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK))
        }
        finish()
    }

    companion object {
        const val EXTRA_PID = "pid"

        /** 呼んだ側のプロセスを落として、アプリを開き直す。**呼んだあとは何もしないこと。** */
        fun restart(from: Activity) {
            from.startActivity(Intent(from, RestartActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                .putExtra(EXTRA_PID, android.os.Process.myPid()))
        }
    }
}
