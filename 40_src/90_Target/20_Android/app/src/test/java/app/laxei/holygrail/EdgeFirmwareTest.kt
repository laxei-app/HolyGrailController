package app.laxei.holygrail

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest

// 公開リポジトリから取ってくるファームの目録まわりの試験(通信はしない)。
//
// ここで守りたいのは「壊れたものを焼かない」こと。途中で切れた中身を焼くと、
// 端末は PC から焼き直すまで動かなくなる。
class EdgeFirmwareTest {

    private val manifest = """
        {
          "schema": 1,
          "updated": "2026-08-26",
          "edge": [
            { "id": "stick-s3", "name": "M5StickS3", "chip": "esp32s3", "flashSize": "8MB",
              "version": "0.1.425", "build": "debug", "file": "hgc-edge-stick-s3.bin",
              "offset": 0, "size": 2685504,
              "sha256": "d18d0210aa10013a2657c851063670998fca1a8ec1d33a4f4c8ecdaa5f255fa3" },
            { "id": "core-s3", "name": "M5Stack CoreS3", "chip": "esp32s3", "flashSize": "16MB",
              "version": "0.1.425", "build": "debug", "file": "hgc-edge-core-s3.bin",
              "offset": 0, "size": 2643376, "sha256": "00" }
          ]
        }
    """.trimIndent()

    @Test
    fun 目録が読める() {
        val list = EdgeFirmware.parseManifest(manifest)
        assertEquals(2, list.size)
        assertEquals("M5StickS3", list[0].name)
        assertEquals(0, list[0].offset)
        assertEquals(2685504, list[0].size)
        assertEquals(8 * 1024 * 1024, list[0].flashSizeBytes())
        assertEquals(16 * 1024 * 1024, list[1].flashSizeBytes())
    }

    @Test
    fun 機種は容量で選ぶ() {
        val list = EdgeFirmware.parseManifest(manifest)
        assertEquals("stick-s3", EdgeFirmware.pickFor(8 * 1024 * 1024, list)?.id)
        assertEquals("core-s3", EdgeFirmware.pickFor(16 * 1024 * 1024, list)?.id)
    }

    @Test
    fun 合う機種が無ければ選ばない() {
        // 勝手に別機種を焼くくらいなら、何も選ばない方がよい
        val list = EdgeFirmware.parseManifest(manifest)
        assertNull(EdgeFirmware.pickFor(4 * 1024 * 1024, list))
        assertNull("容量が読めなかったときに選んではいけない", EdgeFirmware.pickFor(0, list))
    }

    @Test
    fun 中身が合っていれば通る() {
        val data = ByteArray(1000) { it.toByte() }
        val e = entryFor(data)
        assertTrue(EdgeFirmware.verify(data, e))
    }

    @Test
    fun 途中で切れたものは弾く() {
        val data = ByteArray(1000) { it.toByte() }
        val e = entryFor(data)
        assertFalse("大きさ違いを通してはいけない", EdgeFirmware.verify(data.copyOfRange(0, 999), e))
    }

    @Test
    fun 中身が化けたものは弾く() {
        val data = ByteArray(1000) { it.toByte() }
        val e = entryFor(data)
        val broken = data.copyOf().also { it[500] = (it[500] + 1).toByte() }
        assertFalse("大きさが同じでも中身が違えば弾く", EdgeFirmware.verify(broken, e))
    }

    @Test
    fun 照合値が空なら弾く() {
        val data = ByteArray(10)
        val e = entryFor(data).copy(sha256 = "")
        assertFalse("確かめられないものを通してはいけない", EdgeFirmware.verify(data, e))
    }

    @Test
    fun スタブローダが読める() {
        // 資産に入れてある形(base64)と同じものを組み立てて確かめる
        val text = ByteArray(64) { it.toByte() }
        val data = ByteArray(16) { (it * 3).toByte() }
        val b64 = java.util.Base64.getEncoder()
        val json = """
            {"entry":1077391268,
             "text":"${b64.encodeToString(text)}","text_start":1077379072,
             "data":"${b64.encodeToString(data)}","data_start":1070137376}
        """.trimIndent()
        val stub = EdgeFirmware.parseStub(json)
        assertEquals(1077391268, stub.entry)
        assertEquals(1077379072, stub.textStart)
        assertEquals(64, stub.text.size)
        assertEquals(16, stub.data.size)
        assertEquals(0x3F.toByte(), stub.text[63])
    }

    // ── 何をするかの決め方 ──────────────────────────────────

    private fun id(name: String, ver: String, valid: Boolean = true) = FwIdentity(name, ver, valid)
    private val ours = id("HolyGrailEdge", "0.1.427")

    @Test
    fun 同じ版数なら何も書かない() {
        val a = EdgeFirmware.decide(id("HolyGrailEdge", "0.1.427"), ours, sameBase = true)
        assertEquals(FlashAction.SKIP, a)
        assertNull("SKIP なのに書く段取りができている", EdgeFirmware.planWrite(ByteArray(0x30000), a))
    }

    @Test
    fun 版数が違えば本体だけ書く() {
        val a = EdgeFirmware.decide(id("HolyGrailEdge", "0.1.400"), ours, sameBase = true)
        assertEquals(FlashAction.APP_ONLY, a)
    }

    @Test
    fun 検査値が合わなければ土台ごと書く() {
        // 私たちのものでないファーム(工場出荷など)は検査値を持たない
        val a = EdgeFirmware.decide(id("", "", valid = false), ours, sameBase = true)
        assertEquals("読めない相手に本体だけ書いてはいけない", FlashAction.FULL, a)
    }

    @Test
    fun 別のアプリなら土台ごと書く() {
        val a = EdgeFirmware.decide(id("SomethingElse", "0.1.427"), ours, sameBase = true)
        assertEquals(FlashAction.FULL, a)
    }

    @Test
    fun 土台が違えば版数が違っても全部書く() {
        val a = EdgeFirmware.decide(id("HolyGrailEdge", "0.1.400"), ours, sameBase = false)
        assertEquals(FlashAction.FULL, a)
    }

    @Test
    fun 焼く側に素性が無ければ飛ばさない() {
        // まだ刻んでいない公開物。版数を比べようが無いので「同じかも」で飛ばさない
        val a = EdgeFirmware.decide(id("HolyGrailEdge", "0.1.427"), id("", "", false), sameBase = true)
        assertEquals(FlashAction.APP_ONLY, a)
    }

    // ── 書く範囲 ────────────────────────────────────────────

    @Test
    fun 本体だけのときは設定を跨がない() {
        // NVS(0x9000〜0xE000)に設定が入っている。0xE000 から後ろだけ書けば残る。
        val image = ByteArray(0x30000) { (it % 251).toByte() }
        val p = EdgeFirmware.planWrite(image, FlashAction.APP_ONLY)!!
        assertTrue("設定を残す判定になっていない", p.keepsSettings)
        assertEquals(FlashMap.OTADATA, p.offset)
        assertTrue("NVS の終わりより手前から書いてはいけない", p.offset >= FlashMap.NVS_END)
        assertArrayEquals(image.copyOfRange(FlashMap.OTADATA, image.size), p.data)
    }

    @Test
    fun 全部書くときは0番地から() {
        val image = ByteArray(0x30000) { (it % 251).toByte() }
        val p = EdgeFirmware.planWrite(image, FlashAction.FULL)!!
        assertFalse("設定を残せないのに残す判定になっている", p.keepsSettings)
        assertEquals(0, p.offset)
        assertArrayEquals(image, p.data)
    }

    @Test
    fun 短すぎるイメージは本体だけ書けない() {
        val image = ByteArray(FlashMap.OTADATA) { 0 }
        val p = EdgeFirmware.planWrite(image, FlashAction.APP_ONLY)!!
        assertFalse(p.keepsSettings)
        assertEquals(0, p.offset)
    }

    @Test
    fun 区切りの番地が想定どおり() {
        // ここがずれると設定を壊すので、値そのものを固定しておく
        assertEquals(0x9000, FlashMap.NVS)
        assertEquals(0xE000, FlashMap.NVS_END)
        assertEquals(0xE000, FlashMap.OTADATA)
        assertEquals(0x10000, FlashMap.APP)
        assertEquals("素性は アプリ先頭+0x20 と決まっている", 0x10020, FlashMap.APP_DESC)
        assertEquals("消去は4KB単位なので境目に乗っていること", 0, FlashMap.OTADATA % 0x1000)
    }

    // ── 素性の読み取り ──────────────────────────────────────

    /** stamp_fw.py が作るのと同じ形の app_desc を組み立てる。 */
    private fun descOf(name: String, version: String, breakCrc: Boolean = false,
                       magic: Long = 0xABCD5432L): ByteArray {
        val d = ByteArray(FlashMap.APP_DESC_LEN)
        for (k in 0 until 4) d[k] = ((magic shr (8 * k)) and 0xFF).toByte()
        version.toByteArray(Charsets.US_ASCII).copyInto(d, 16)
        name.toByteArray(Charsets.US_ASCII).copyInto(d, 48)
        var crc = java.util.zip.CRC32().apply { update(d, 16, 64) }.value
        if (breakCrc) crc = crc xor 1L
        for (k in 0 until 4) d[176 + k] = ((crc shr (8 * k)) and 0xFF).toByte()
        return d
    }

    @Test
    fun 素性が読める() {
        val i = EdgeFirmware.parseIdentity(descOf("HolyGrailEdge", "0.1.426"))
        assertTrue(i.valid)
        assertEquals("HolyGrailEdge", i.name)
        assertEquals("0.1.426", i.version)
    }

    @Test
    fun 検査値が壊れていれば無効とする() {
        assertFalse(EdgeFirmware.parseIdentity(descOf("HolyGrailEdge", "0.1.426", breakCrc = true)).valid)
    }

    @Test
    fun 場所が違えば無効とする() {
        // 工場出荷ファームなど、そこが app_desc でない場合
        assertFalse(EdgeFirmware.parseIdentity(descOf("x", "y", magic = 0x12345678L)).valid)
        assertFalse(EdgeFirmware.parseIdentity(ByteArray(10)).valid)
    }

    private fun entryFor(data: ByteArray): EdgeFirmwareEntry {
        val sha = MessageDigest.getInstance("SHA-256").digest(data)
            .joinToString("") { "%02x".format(it) }
        return EdgeFirmwareEntry(
            id = "t", name = "t", chip = "esp32s3", flashSize = "8MB", version = "0",
            build = "debug", file = "t.bin", offset = 0, size = data.size, sha256 = sha
        )
    }
}
