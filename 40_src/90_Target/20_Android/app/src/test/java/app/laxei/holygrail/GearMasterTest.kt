package app.laxei.holygrail

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.security.MessageDigest

// 機材マスタの版まわりの試験(通信はしない)。
//
// 守りたいのは2つ。
//  ・読めない構造の一覧を取り込まないこと(古いまま動くほうが安全)
//  ・壊れた一覧で上書きしないこと(機材が選べなくなる)
class GearMasterTest {

    private fun manifest(schema: Int, rev: Int, size: Int = 10, sha: String = "ab") = """
        { "schema": $schema, "revision": $rev, "updated": "2026-08-27",
          "files": [ { "name": "cameras.json", "size": $size, "sha256": "$sha", "count": 18 } ] }
    """.trimIndent()

    @Test
    fun 目録が読める() {
        val i = GearMaster.parseManifest(manifest(1, 7))
        assertEquals(1, i.schema)
        assertEquals(7, i.revision)
        assertEquals(1, i.files.size)
        assertEquals("cameras.json", i.files[0].name)
    }

    @Test
    fun 中身が新しければ取り込む() {
        assertTrue(GearMaster.shouldAdopt(GearMaster.parseManifest(manifest(1, 8)), 7))
    }

    @Test
    fun 同じか古ければ取り込まない() {
        assertFalse(GearMaster.shouldAdopt(GearMaster.parseManifest(manifest(1, 7)), 7))
        assertFalse(GearMaster.shouldAdopt(GearMaster.parseManifest(manifest(1, 6)), 7))
    }

    @Test
    fun 読めない構造は取り込まない() {
        // 版が上でも、構造が読めなければ手を出さない。古い一覧のままでも動く
        assertFalse("構造が新しすぎるのに取り込んでいる",
                    GearMaster.shouldAdopt(GearMaster.parseManifest(manifest(GearMaster.SCHEMA_MAX + 1, 99)), 0))
        assertFalse("構造の版が無い目録を取り込んでいる",
                    GearMaster.shouldAdopt(GearMaster.parseManifest(manifest(0, 99)), 0))
    }

    @Test
    fun 中身が合っていれば通る() {
        val d = ByteArray(64) { it.toByte() }
        assertTrue(GearMaster.verify(d, entryFor(d)))
    }

    @Test
    fun 途中で切れたものは弾く() {
        val d = ByteArray(64) { it.toByte() }
        assertFalse("大きさ違いを通してはいけない", GearMaster.verify(d.copyOfRange(0, 63), entryFor(d)))
    }

    @Test
    fun 中身が化けたものは弾く() {
        val d = ByteArray(64) { it.toByte() }
        val broken = d.copyOf().also { it[10] = (it[10] + 1).toByte() }
        assertFalse("大きさが同じでも中身が違えば弾く", GearMaster.verify(broken, entryFor(d)))
    }

    @Test
    fun 照合値が無いものは弾く() {
        val d = ByteArray(8)
        assertFalse("確かめられないものを通してはいけない",
                    GearMaster.verify(d, GearMaster.Entry("cameras.json", d.size, "")))
    }

    private fun entryFor(d: ByteArray) = GearMaster.Entry(
        "cameras.json", d.size,
        MessageDigest.getInstance("SHA-256").digest(d).joinToString("") { "%02x".format(it) })
}
