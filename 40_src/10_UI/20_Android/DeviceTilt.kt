package app.laxei.twylapse

// 【撮ったものを正しい向きで残す(2026-10-01 ユーザー指示)】端末をどう置いて撮ったかを重力で測る。
//
// 【なぜ重力か】センサーから出てくる画像の向きは**端末の置き方と関係がない**(センサーは基板に
//  固定されているので、端末を横にしようが逆さにしようが同じ向きで出てくる)。だから今までの
//  動画はどう撮っても同じ向きだった。どちらが下かを知る手立ては、端末にかかる重力しかない。
//
// 【画面の向きではだめな理由】Display の回転は「画面をどう見せているか」であって、端末の
//  置き方ではない。自動回転を切っていれば動かないし、撮影中は画面を消すので最後の値が残る。
//  三脚に固定した端末の姿勢は、重力で測るのが唯一確かな方法。
//
// 【いつ測るか】1回の撮影につき1度だけ(撮り始め)。途中で測り直すと、風で揺れたり
//  三脚を直したりしたときに動画の途中で向きが変わり、1本の中で上下が混ざって壊れる。
//
// 【戻り値の規約】OrientationEventListener と**同じ**にしてある(0=自然な向き、
//  90=左側が上、180=逆さ、270=右側が上)。Camera2 の向きの式がこの規約で書かれているため。
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.util.Log
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.roundToInt
import kotlin.math.sqrt

object DeviceTilt {

    const val UNKNOWN = -1

    // 直近に測った値と、その根拠(ログ用)。
    @Volatile private var lastWhy = ""
    fun why(): String = lastWhy

    // 端末の向きを測る。戻り = 0 / 90 / 180 / 270。測れなければ画面の向きで代える。
    //
    //  【平らに置いたときは測れない】真上(天頂)に向けると端末はほぼ水平になり、重力の
    //   水平成分が消えて「どちらが下か」が決まらない。そもそも真上を向けた絵に上下は無いので、
    //   どれを選んでも破綻しない。そのときだけ画面の向きを使う。
    fun measure(ctx: Context, waitMs: Long = 1200L): Int {
        val g = gravity(ctx, waitMs)
        if (g != null) {
            val gx = g[0]; val gy = g[1]; val gz = g[2]
            val ori = fromGravity(gx, gy, gz)
            if (ori != UNKNOWN) {
                lastWhy = String.format(java.util.Locale.US,
                    "gravity (%.2f,%.2f,%.2f) -> %d", gx, gy, gz, ori)
                Log.i("TLP-ORI", lastWhy)
                return ori
            }
            lastWhy = String.format(java.util.Locale.US,
                "gravity (%.2f,%.2f,%.2f) too flat -> display", gx, gy, gz)
        } else {
            lastWhy = "no gravity sensor -> display"
        }
        val d = fromDisplay(ctx)
        lastWhy += " = $d"
        Log.i("TLP-ORI", lastWhy)
        return d
    }

    // 重力ベクトル → 向き。測れないときは UNKNOWN。**ここだけは端末が要らない**(単体で試せる)。
    //
    //  重力は「端末の上向き」を指す(加速度センサーの約束。平らに置いて画面が上を向いていると
    //  z が +9.8)。式は AOSP の OrientationEventListener と同じ —
    //    X = -gx, Y = -gy, 向き = 90 - atan2(-Y, X)
    //  端末を立てて上端が上: g=(0,+G,0) → 0 / 左側が上: g=(-G,0,0) → 90 /
    //  逆さ: g=(0,-G,0) → 180 / 右側が上: g=(+G,0,0) → 270。
    fun fromGravity(gx: Float, gy: Float, gz: Float): Int {
        val flat = sqrt(gx * gx + gy * gy)
        // 平らに置くと水平成分が消えて「どちらが下か」が決まらない。見極めは AOSP と同じ
        //  (水平成分が |z| の半分以上 = 約27度以上傾いていれば信じる)。
        if (flat * 2f < abs(gz)) { return UNKNOWN }
        val a = Math.toDegrees(atan2(gy.toDouble(), -gx.toDouble()))
        return quantize(90.0 - a)
    }

    // 重力[m/s^2]を数回ぶん平均して取る。無ければ加速度センサーで代える(静止しているので同じ)。
    private fun gravity(ctx: Context, waitMs: Long): FloatArray? {
        val sm = ctx.getSystemService(Context.SENSOR_SERVICE) as? SensorManager ?: return null
        val sensor = sm.getDefaultSensor(Sensor.TYPE_GRAVITY)
            ?: sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            ?: return null
        val sum = FloatArray(3)
        var n = 0
        val done = CountDownLatch(1)
        val lis = object : SensorEventListener {
            override fun onSensorChanged(e: SensorEvent) {
                if (n >= kSamples) { return }
                sum[0] += e.values[0]; sum[1] += e.values[1]; sum[2] += e.values[2]
                if (++n >= kSamples) { done.countDown() }
            }
            override fun onAccuracyChanged(s: Sensor?, a: Int) {}
        }
        return try {
            sm.registerListener(lis, sensor, SensorManager.SENSOR_DELAY_NORMAL)
            done.await(waitMs, TimeUnit.MILLISECONDS)
            sm.unregisterListener(lis)
            // 1つでも取れていれば使う(待ち切れなくても、静止しているので1回で足りる)。
            if (n <= 0) { null } else { floatArrayOf(sum[0] / n, sum[1] / n, sum[2] / n) }
        } catch (e: Exception) {
            runCatching { sm.unregisterListener(lis) }
            null
        }
    }

    private const val kSamples = 5

    // 画面の向き → OrientationEventListener の規約。回り方が逆なので 360 から引く。
    private fun fromDisplay(ctx: Context): Int {
        val rot = runCatching {
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                ctx.display?.rotation
            } else {
                @Suppress("DEPRECATION")
                (ctx.getSystemService(Context.WINDOW_SERVICE) as? android.view.WindowManager)
                    ?.defaultDisplay?.rotation
            }
        }.getOrNull() ?: android.view.Surface.ROTATION_0
        val deg = when (rot) {
            android.view.Surface.ROTATION_90  -> 90
            android.view.Surface.ROTATION_180 -> 180
            android.view.Surface.ROTATION_270 -> 270
            else -> 0
        }
        return (360 - deg) % 360
    }

    private fun quantize(deg: Double): Int {
        var d = deg.roundToInt()
        d = ((d % 360) + 360) % 360
        return ((d + 45) / 90 * 90) % 360
    }
}
