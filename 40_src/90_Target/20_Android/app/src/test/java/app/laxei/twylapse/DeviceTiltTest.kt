package app.laxei.twylapse

// 【撮った向き(2026-10-01 依頼)】重力から端末の置き方を決める式を固める。
//  ここを間違えると動画が 90 度ずれたまま一晩ぶん出来上がる(撮り直しがきかない)ので、
//  四方向と斜め・平らの場合を単体で押さえておく。
//
//  戻り値は OrientationEventListener と同じ規約(0=自然な向き / 90=左側が上 /
//  180=逆さ / 270=右側が上)。重力ベクトルは「端末の上向き」を指す。
import org.junit.Assert.assertEquals
import org.junit.Test

class DeviceTiltTest {

    private val G = 9.81f

    @Test
    fun 四方向() {
        assertEquals(0,   DeviceTilt.fromGravity(0f, G, 0f))    // 立てて上端が上
        assertEquals(90,  DeviceTilt.fromGravity(-G, 0f, 0f))   // 左側が上
        assertEquals(180, DeviceTilt.fromGravity(0f, -G, 0f))   // 逆さ
        assertEquals(270, DeviceTilt.fromGravity(G, 0f, 0f))    // 右側が上
    }

    @Test
    fun 少し傾いていても四方向へ寄せる() {
        // 30 度ほど傾けても、近い方の向きになる(45 度で切り替わる)。
        assertEquals(0,   DeviceTilt.fromGravity(-4f, 9f, 0f))
        assertEquals(0,   DeviceTilt.fromGravity(4f, 9f, 0f))
        assertEquals(90,  DeviceTilt.fromGravity(-9f, 4f, 0f))
        assertEquals(270, DeviceTilt.fromGravity(9f, -4f, 0f))
    }

    @Test
    fun 前後に倒れていても水平成分で決まる() {
        // カメラを斜め上へ向けた姿勢(z に成分が乗る)。水平成分が残っていれば決まる。
        assertEquals(0,  DeviceTilt.fromGravity(0f, 7f, 7f))
        assertEquals(90, DeviceTilt.fromGravity(-7f, 0f, -7f))
    }

    @Test
    fun 平らに置いたら決められない() {
        // 真上(または真下)を向けると、どちらが下かは重力から決まらない。
        assertEquals(DeviceTilt.UNKNOWN, DeviceTilt.fromGravity(0f, 0f, G))
        assertEquals(DeviceTilt.UNKNOWN, DeviceTilt.fromGravity(0f, 0f, -G))
        assertEquals(DeviceTilt.UNKNOWN, DeviceTilt.fromGravity(0.5f, 0.5f, G))
    }
}
