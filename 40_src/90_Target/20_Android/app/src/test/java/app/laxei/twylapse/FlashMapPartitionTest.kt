package app.laxei.twylapse

// 【区切り表の読み取り(2026-10-03 依頼)】「設定を消す」はここで求めた場所だけを消す。
//  読み違えると**別の領域を消して端末を壊す**ので、形を単体で押さえておく。
//  1件32バイト: magic(2) type(1) subtype(1) offset(4) size(4) label(16) flags(4)
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class FlashMapPartitionTest {

    private fun entry(type: Int, sub: Int, off: Int, size: Int, label: String): ByteArray {
        val b = ByteArray(32)
        b[0] = 0xAA.toByte(); b[1] = 0x50
        b[2] = type.toByte(); b[3] = sub.toByte()
        fun le(at: Int, v: Int) {
            b[at] = (v and 0xFF).toByte()
            b[at + 1] = ((v shr 8) and 0xFF).toByte()
            b[at + 2] = ((v shr 16) and 0xFF).toByte()
            b[at + 3] = ((v shr 24) and 0xFF).toByte()
        }
        le(4, off); le(8, size)
        for (i in label.indices) { b[12 + i] = label[i].code.toByte() }
        return b
    }

    /** 実機(16MB)と同じ並び。NVS は 0x9000 から 0x5000。 */
    private fun realTable(): ByteArray =
        entry(1, 2, 0x9000, 0x5000, "nvs") +
        entry(1, 0, 0xE000, 0x2000, "otadata") +
        entry(0, 0x10, 0x10000, 0x640000, "app0") +
        entry(0, 0x11, 0x650000, 0x640000, "app1") +
        entry(1, 0x82, 0xC90000, 0x360000, "spiffs") +
        ByteArray(64)   // 表の終わり(0 埋め)

    @Test
    fun 実機の並びから設定の区切りを見つける() {
        val parts = FlashMap.parsePartitions(realTable())
        assertEquals(5, parts.size)
        val nvs = FlashMap.findNvs(parts)!!
        assertEquals("nvs", nvs.label)
        assertEquals(0x9000, nvs.offset)
        assertEquals(0x5000, nvs.size)
    }

    @Test
    fun 位置が変わっても表のとおりに読む() {
        // 決め打ちしていないことの確認。区切り方を変えたらそちらへ従う。
        val t = entry(1, 0, 0x9000, 0x2000, "otadata") +
                entry(1, 2, 0xB000, 0x8000, "nvs") +
                ByteArray(32)
        val nvs = FlashMap.findNvs(FlashMap.parsePartitions(t))!!
        assertEquals(0xB000, nvs.offset)
        assertEquals(0x8000, nvs.size)
    }

    @Test
    fun 設定の区切りが無ければ何も返さない() {
        val t = entry(0, 0x10, 0x10000, 0x100000, "app0") + ByteArray(32)
        assertNull(FlashMap.findNvs(FlashMap.parsePartitions(t)))
    }

    @Test
    fun 空の表や壊れた表でも落ちない() {
        assertEquals(0, FlashMap.parsePartitions(ByteArray(0)).size)
        assertEquals(0, FlashMap.parsePartitions(ByteArray(3072)).size)   // 全部 0
        // 途中から壊れていたら、そこまでを返す(壊れた先は読まない)
        val t = entry(1, 2, 0x9000, 0x5000, "nvs") + ByteArray(32) { 0x5A }
        assertEquals(1, FlashMap.parsePartitions(t).size)
    }
}
