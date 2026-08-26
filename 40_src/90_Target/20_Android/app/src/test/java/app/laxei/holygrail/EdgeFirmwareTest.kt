package app.laxei.holygrail

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

    private fun entryFor(data: ByteArray): EdgeFirmwareEntry {
        val sha = MessageDigest.getInstance("SHA-256").digest(data)
            .joinToString("") { "%02x".format(it) }
        return EdgeFirmwareEntry(
            id = "t", name = "t", chip = "esp32s3", flashSize = "8MB", version = "0",
            build = "debug", file = "t.bin", offset = 0, size = data.size, sha256 = sha
        )
    }
}
