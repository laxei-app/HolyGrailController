package app.laxei.twylapse

// 【レンズがそのカメラに付くか(2026-10-03 依頼)】カメラにレンズを組むとき、付かないものは
//  一覧に出さない。ここを間違えると「使えるレンズが選べない」「使えないレンズが選べる」の
//  どちらかになるので、組み合わせを単体で押さえておく。
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LensMountTest {

    @Test
    fun 同じマウントは付く() {
        assertTrue(GearMaster.lensFitsMount("RF", "RF"))
        assertTrue(GearMaster.lensFitsMount("EF-M", "EF-M"))
    }

    @Test
    fun RFボディにEFレンズはアダプタで付く() {
        assertTrue(GearMaster.lensFitsMount("RF", "EF"))
    }

    @Test
    fun 逆向きとEF_Mは付かない() {
        assertFalse(GearMaster.lensFitsMount("EF", "RF"))       // EF ボディに RF は物理的に無理
        assertFalse(GearMaster.lensFitsMount("EF-M", "RF"))
        assertFalse(GearMaster.lensFitsMount("EF-M", "EF"))     // アダプタは別物。ここでは繋がない
        assertFalse(GearMaster.lensFitsMount("RF", "EF-M"))
    }

    @Test
    fun 分からないものは出す() {
        // 【消さない】手で足したレンズや古いマスタで登録したものはマウントが空。
        //  これを消すと「登録したはずのレンズが消えた」ように見える。
        assertTrue(GearMaster.lensFitsMount("RF", ""))
        assertTrue(GearMaster.lensFitsMount("", "RF"))
        assertTrue(GearMaster.lensFitsMount("", ""))
    }

    @Test
    fun 大文字小文字は区別しない() {
        assertTrue(GearMaster.lensFitsMount("rf", "RF"))
        assertTrue(GearMaster.lensFitsMount("RF", "ef"))
    }
}
