package app.laxei.holygrail

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.zip.Inflater

// USB 書き込み(EspFlasher)の試験。**実機もスマホも使わない**。
//
// 難所は Android ではなく書き込みの手順そのものなので、そこを先に潰す。
// 偽の ROM ローダを相手に、同期 → スタブ投入 → 圧縮書き込み → MD5 照合 までを通し、
// 受け取った側で展開したものが元のイメージと一致することまで確かめる。
class EspFlasherTest {

    // ── 純粋な部品 ──────────────────────────────────────────

    @Test
    fun slipは0xC0と0xDBを逃がす() {
        val enc = EspFlasher.slipEncode(byteArrayOf(0x01, 0xC0.toByte(), 0xDB.toByte(), 0x02))
        assertArrayEquals(
            byteArrayOf(0xC0.toByte(), 0x01, 0xDB.toByte(), 0xDC.toByte(),
                        0xDB.toByte(), 0xDD.toByte(), 0x02, 0xC0.toByte()),
            enc
        )
    }

    @Test
    fun slipは包んで解くと元に戻る() {
        val body = ByteArray(300) { (it * 7).toByte() }   // 0xC0/0xDB を含む
        val dec = EspFlasher.SlipDecoder()
        dec.feed(EspFlasher.slipEncode(body))
        assertArrayEquals(body, dec.take())
        assertNull(dec.take())
    }

    @Test
    fun slipはバイトが分かれて届いても組み立てる() {
        val body = byteArrayOf(0x01, 0xC0.toByte(), 0x03)
        val enc = EspFlasher.slipEncode(body)
        val dec = EspFlasher.SlipDecoder()
        for (b in enc) dec.feed(byteArrayOf(b))          // 1バイトずつ
        assertArrayEquals(body, dec.take())
    }

    @Test
    fun slipは端末の生ログを読み飛ばす() {
        // ROM もアプリも、包みの外に普通の文字を吐く。混ざっても壊れないこと。
        val dec = EspFlasher.SlipDecoder()
        dec.feed("Guru Meditation...\r\n".toByteArray())
        dec.feed(EspFlasher.slipEncode(byteArrayOf(0x42)))
        dec.feed("waiting for download\r\n".toByteArray())
        assertArrayEquals(byteArrayOf(0x42), dec.take())
        assertNull(dec.take())
    }

    @Test
    fun slipは続けて届いた2つを別々に取り出す() {
        val dec = EspFlasher.SlipDecoder()
        val two = EspFlasher.slipEncode(byteArrayOf(1, 2)) + EspFlasher.slipEncode(byteArrayOf(3))
        dec.feed(two)
        assertArrayEquals(byteArrayOf(1, 2), dec.take())
        assertArrayEquals(byteArrayOf(3), dec.take())
    }

    @Test
    fun 検査値はROMの定義どおり() {
        // 0xEF から1バイトずつ排他的論理和
        assertEquals(0xEF, EspFlasher.checksum(ByteArray(0)))
        assertEquals(0xEF xor 0x01 xor 0x02, EspFlasher.checksum(byteArrayOf(0x01, 0x02)))
        assertEquals(0xEF xor 0xFF, EspFlasher.checksum(byteArrayOf(0xFF.toByte())))
    }

    @Test
    fun 命令の並びはリトルエンディアン() {
        val pkt = EspFlasher.buildCommand(0x09, ByteArray(0x0102), 0xAABBCCDD.toInt())
        assertEquals(0x00, pkt[0].toInt())
        assertEquals(0x09, pkt[1].toInt())
        assertEquals(0x02, pkt[2].toInt())                       // 長さ下位
        assertEquals(0x01, pkt[3].toInt())                       // 長さ上位
        assertEquals(0xDD, pkt[4].toInt() and 0xFF)              // 検査値は下位から
        assertEquals(0xAA, pkt[7].toInt() and 0xFF)
    }

    @Test
    fun le32は下位から並べる() {
        assertArrayEquals(
            byteArrayOf(0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00),
            EspFlasher.le32(0x12345678, 1)
        )
    }

    @Test
    fun 応答は0x01で始まるものだけ拾う() {
        assertNull(EspFlasher.parseResponse(byteArrayOf(0x00, 0x08, 0, 0, 0, 0, 0, 0)))
        assertNull(EspFlasher.parseResponse(byteArrayOf(0x01, 0x08)))          // 短い
        val r = EspFlasher.parseResponse(
            byteArrayOf(0x01, 0x0A, 0x02, 0x00, 0x21, 0x43, 0x65, 0x87.toByte(), 0x00, 0x00)
        )!!
        assertEquals(0x0A, r.op)
        assertEquals(0x87654321.toInt(), r.value)
        assertArrayEquals(byteArrayOf(0x00, 0x00), r.body)
    }

    @Test
    fun 応答の長さの欄が短くても本体を取りこぼさない() {
        // 実機のスタブは MD5(16)+成否(2)=18 バイトを送りながら、長さの欄に 2 と書いてきた。
        // 長さで切ると MD5 の先頭2バイトだけが残り、それを成否と読んで嘘の失敗になる。
        val md5 = ByteArray(16) { (0x20 + it).toByte() }
        val pkt = byteArrayOf(0x01, 0x13, 0x02, 0x00, 0, 0, 0, 0) + md5 + byteArrayOf(0, 0)
        val r = EspFlasher.parseResponse(pkt)!!
        assertEquals("長さの欄ではなく実際の中身を見ること", 18, r.body.size)
        assertArrayEquals(md5, r.body.copyOfRange(0, 16))
    }

    @Test
    fun 圧縮は展開すると元に戻る() {
        val src = ByteArray(200_000) { ((it / 977) % 251).toByte() }
        val comp = EspFlasher.deflate(src)
        assertTrue("縮んでいない", comp.size < src.size)
        assertArrayEquals(src, inflate(comp, src.size))
    }

    // ── 偽の ROM を相手に通しで ────────────────────────────────

    @Test
    fun 同期からスタブ投入と書き込みと照合まで通る() {
        val image = ByteArray(70_000) { ((it * 31) % 253).toByte() }
        val rom = FakeRom()
        val f = EspFlasher(rom)

        f.sync()
        f.runStub(EspStub(
            entry = 0x40378000, text = ByteArray(9_000) { it.toByte() }, textStart = 0x40378000,
            data = ByteArray(300) { it.toByte() }, dataStart = 0x3FCA0000
        ))
        assertTrue("スタブが走っていない", rom.stubStarted)

        val seen = mutableListOf<Pair<String, Int>>()
        f.writeFlash(0x10000, image) { phase, done, total -> seen.add(phase to done * 100 / total) }

        assertEquals("書き込み先が違う", 0x10000, rom.flashOffset)
        assertArrayEquals("展開したものが元と違う", image, inflate(rom.flashData(), image.size))
        assertTrue("進み具合が出ていない", seen.any { it.first == "write" && it.second == 100 })

        rom.flashImage = image
        assertTrue("MD5 照合が通らない", f.verify(0x10000, image))
    }

    @Test
    fun 素性はMACと容量が読める() {
        val rom = FakeRom()
        // 14:c1:9f:d5:a2:74 になるように仕込む
        rom.regs[EspFlasher.MAC_EFUSE_REG] = 0x9FD5A274.toInt()
        rom.regs[EspFlasher.MAC_EFUSE_REG + 4] = 0x000014C1
        rom.rdid = 0x17_40_C8                    // 3バイト目 0x17 = 2^23 = 8MB
        val f = EspFlasher(rom)
        f.sync()
        val info = f.probe()
        assertEquals("14:c1:9f:d5:a2:74", info.mac)
        assertEquals(8 * 1024 * 1024, info.flashSizeBytes)
    }

    @Test
    fun スタブが既に走っていれば載せ直さない() {
        // 実機で踏んだ: 前回の書き込みでスタブが載ったまま、もう一度載せようとして壊れた。
        // ROM は同期に 0 以外を返し、スタブは 0 を返す。そこで見分ける。
        val rom = FakeRom()
        val f = EspFlasher(rom)
        f.sync()
        assertFalse("最初は ROM のはず", f.isStubRunning())
        f.runStub(EspStub(0x40378000, ByteArray(100), 0x40378000, ByteArray(8), 0x3FCA0000))
        assertTrue(rom.stubStarted)

        // 繋ぎ直した想定でもう一度同期すると、今度はスタブだと分かる
        val f2 = EspFlasher(rom)
        f2.sync()
        assertTrue("スタブだと見分けられていない", f2.isStubRunning())
        rom.memBeginCount = 0
        f2.runStub(EspStub(0x40378000, ByteArray(100), 0x40378000, ByteArray(8), 0x3FCA0000))
        assertEquals("載せ直してはいけない", 0, rom.memBeginCount)
    }

    @Test
    fun 壊れた応答は失敗として扱う() {
        val rom = FakeRom().also { it.failNext = true }
        val f = EspFlasher(rom)
        try {
            f.sync(retries = 1)
            throw AssertionError("失敗すべき場面で通ってしまった")
        } catch (e: EspFlashError) {
            assertTrue(e.message!!.contains("応答"))
        }
    }

    // ── 道具 ────────────────────────────────────────────────

    private fun inflate(comp: ByteArray, hint: Int): ByteArray {
        val inf = Inflater()
        inf.setInput(comp)
        val out = ByteArrayOutputStream(hint)
        val buf = ByteArray(64 * 1024)
        while (!inf.finished()) {
            val n = inf.inflate(buf)
            if (n == 0 && (inf.needsInput() || inf.needsDictionary())) break
            out.write(buf, 0, n)
        }
        inf.end()
        return out.toByteArray()
    }

    /**
     * 偽の ROM ローダ。こちらが送った包みを解いて、本物と同じ形の応答を返す。
     * 受け取ったフラッシュ書き込みは溜めておき、試験側で展開して突き合わせる。
     */
    private class FakeRom : EspTransport {
        val regs = HashMap<Int, Int>()
        var rdid = 0x17_40_C8
        var stubStarted = false
        var flashOffset = -1
        var failNext = false
        var flashImage: ByteArray = ByteArray(0)
        var memBeginCount = 0

        private val flash = ByteArrayOutputStream()
        private val outbox = ByteArrayOutputStream()
        private val dec = EspFlasher.SlipDecoder()
        private var spiCmdPending = 0

        fun flashData(): ByteArray = flash.toByteArray()

        override fun write(data: ByteArray) {
            dec.feed(data)
            while (true) {
                val pkt = dec.take() ?: break
                handle(pkt)
            }
        }

        override fun read(max: Int, timeoutMs: Int): ByteArray {
            val b = outbox.toByteArray()
            outbox.reset()
            return if (b.size <= max) b else b.copyOfRange(0, max)
        }

        override fun setControlLines(dtr: Boolean, rts: Boolean) {}
        override fun discardInput() { outbox.reset() }

        private fun reply(op: Int, value: Int, extra: ByteArray = ByteArray(0), declaredLen: Int = -1) {
            val body = extra + byteArrayOf(0, 0)          // 末尾2バイト=成功
            val pkt = ByteArray(8 + body.size)
            val dl = if (declaredLen >= 0) declaredLen else body.size
            pkt[0] = 0x01; pkt[1] = op.toByte()
            pkt[2] = (dl and 0xFF).toByte()
            pkt[3] = ((dl shr 8) and 0xFF).toByte()
            for (k in 0 until 4) pkt[4 + k] = ((value shr (8 * k)) and 0xFF).toByte()
            body.copyInto(pkt, 8)
            outbox.write(EspFlasher.slipEncode(pkt))
        }

        private fun le(p: ByteArray, i: Int): Int {
            var v = 0
            for (k in 0 until 4) v = v or ((p[i + k].toInt() and 0xFF) shl (8 * k))
            return v
        }

        private fun handle(pkt: ByteArray) {
            if (failNext) return                          // 黙って落とす=時間切れになる
            val op = pkt[1].toInt() and 0xFF
            val chk = le(pkt, 4)
            val body = pkt.copyOfRange(8, pkt.size)
            when (op) {
                // 本物と同じにする: ROM は 0 以外、スタブが走っていれば 0 を返す
                EspFlasher.SYNC -> reply(op, if (stubStarted) 0 else 0x07)
                EspFlasher.READ_REG -> {
                    val addr = le(body, 0)
                    val v = when (addr) {
                        EspFlasher.SPI_REG_BASE -> 0                       // CMD_USR は即終わる
                        EspFlasher.SPI_REG_BASE + EspFlasher.SPI_W0_OFFS -> spiCmdPending
                        else -> regs[addr] ?: 0
                    }
                    reply(op, v)
                }
                EspFlasher.WRITE_REG -> {
                    val addr = le(body, 0)
                    val v = le(body, 4)
                    regs[addr] = v
                    // SPI の「ユーザ命令」を起こしたら、RDID の答えを W0 に置いたことにする
                    if (addr == EspFlasher.SPI_REG_BASE && v == (1 shl 18)) spiCmdPending = rdid
                    reply(op, 0)
                }
                EspFlasher.SPI_ATTACH -> reply(op, 0)
                EspFlasher.MEM_BEGIN -> { memBeginCount++; reply(op, 0) }
                EspFlasher.MEM_DATA -> reply(op, 0)
                EspFlasher.MEM_END -> {
                    reply(op, 0)
                    outbox.write(EspFlasher.slipEncode("OHAI".toByteArray()))
                    stubStarted = true
                }
                EspFlasher.FLASH_DEFL_BEGIN -> { flashOffset = le(body, 12); reply(op, 0) }
                EspFlasher.FLASH_DEFL_DATA -> {
                    val n = le(body, 0)
                    val chunk = body.copyOfRange(16, 16 + n)
                    if (EspFlasher.checksum(chunk) != chk) return   // 検査値違いは無応答=失敗
                    flash.write(chunk)
                    reply(op, 0)
                }
                EspFlasher.FLASH_DEFL_END -> reply(op, 0)
                EspFlasher.SPI_FLASH_MD5 -> {
                    // 実機のスタブと同じ意地悪をする: 長さの欄には成否ぶんの 2 しか書かない
                    val md5 = MessageDigest.getInstance("MD5").digest(flashImage)
                    reply(op, 0, md5, declaredLen = 2)
                }
                else -> reply(op, 0)
            }
        }
    }
}
