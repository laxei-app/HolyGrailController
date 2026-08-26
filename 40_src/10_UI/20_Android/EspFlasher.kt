package app.laxei.holygrail

import java.io.ByteArrayOutputStream
import java.security.MessageDigest
import java.util.zip.Deflater

// エッジ端末(ESP32-S3)へ USB で書き込む(2026-08-26)。
//
// 【なぜ要るか】買ってきたばかりの端末には私たちのファームが入っていない。OTA も STA での
//  自己更新も「今動いているファームが受け取って書く」仕組みなので、**最初の1回には使えない**。
//  最初の書き込みは USB で ROM のダウンロードモードへ入れて焼くしかない。
//
// 【この層の役割】バイトのやり取り(EspTransport)の上に、ROM ローダの手順だけを載せる。
//  USB でもPCのシリアルでも同じ手順が使えるよう、経路は差し替え可能にしてある。
//
// 【実機で分かっている勘所(2026-08-26 M5StickS3 で実測)】
//  ・**リセットは watchdog_reset でないと戻らない**。焼いた後に DTR/RTS のリセットを掛けても
//    ダウンロードモードから抜けず、無反応のままになる(「焼けたのに起動しない」に見える)。
//    RTC のウォッチドッグを仕掛けて自分を蹴らせるのが正解。
//  ・USB-Serial/JTAG は USB なので **ボーレートは意味を持たない**。速度変更は要らない。
//  ・**ダウンロードモードへ自動で入れるかは機種で違う**(2026-08-26 両機で実測)。
//    CoreS3 は工場出荷FWでもハードウェアの USB-Serial/JTAG(303A:1001)なので、
//    リセットをチップ側が処理してくれて**ボタン操作なしで入る**。
//    StickS3 の工場出荷FWは TinyUSB(303A:8120)で DTR/RTS を見ておらず入れない。
//    **電源ボタン長押し2秒**(LEDが点滅する)で手で入れてもらう。
//    どちらも私たちのファームが入った後は自動で入る(303A:1001 になるため)。
//  ・機種はフラッシュ容量で見分ける(8MB=StickS3 / 16MB=CoreS3)。ESP32-S3 は Chip ID を
//    持たないので MAC しか出ない。容量は1バイトも書かずに読める。

/** 相手とバイトをやり取りする経路。USB CDC でもPCのシリアルでも差し替えられるようにする。 */
interface EspTransport {
    /** そのまま送る(SLIP の包みはこの層より上で行う)。 */
    fun write(data: ByteArray)

    /** 最大 max バイト読む。読めなければ空を返してよい(締切は呼ぶ側が見る)。 */
    fun read(max: Int, timeoutMs: Int): ByteArray

    /** DTR/RTS を立てる・寝かせる。USB CDC では制御転送1つ。 */
    fun setControlLines(dtr: Boolean, rts: Boolean)

    /** 受信の取りこぼしを捨てる。 */
    fun discardInput()
}

class EspFlashError(message: String) : Exception(message)

/** ダウンロードモードで読めた端末の素性。 */
data class EspChipInfo(
    val mac: String,            // 14:c1:9f:d5:a2:74 の形
    val flashId: Int,           // RDID(0x9F)の生値
    val flashSizeBytes: Int     // 判別できなければ 0
)

/**
 * スタブローダ。ROM だけでも焼けるが遅いので、まず 7KB ほどのプログラムを RAM へ送り込んで
 * そちらに働いてもらう。中身は esptool が配っているものをそのまま資産として持つ。
 */
data class EspStub(
    val entry: Int,
    val text: ByteArray,
    val textStart: Int,
    val data: ByteArray,
    val dataStart: Int
)

/** 進み具合の通知。written/total はバイト。 */
fun interface EspProgress {
    fun onProgress(phase: String, written: Int, total: Int)
}

class EspFlasher(private val io: EspTransport) {

    private var stubRunning = false
    private val rx = SlipDecoder()

    // ── 手順 ────────────────────────────────────────────────

    /**
     * ROM ローダと同期する。相手がダウンロードモードに居ることが前提。
     * 入っていなければ呼ぶ側で自動投入(DTR/RTS)を試し、それでも駄目なら案内を出すこと。
     */
    fun sync(retries: Int = 10) {
        val payload = ByteArray(36).also {
            it[0] = 0x07; it[1] = 0x07; it[2] = 0x12; it[3] = 0x20
            for (i in 4 until 36) it[i] = 0x55
        }
        var last: Exception? = null
        for (i in 0 until retries) {
            try {
                io.discardInput()
                rx.reset()
                val (value, _) = command(SYNC, payload, 0, 500)
                // 【相手がもう スタブ かどうかはここで分かる(2026-08-26 実機で判明)】
                //  ROM は 0 以外を返し、スタブは 0 を返す。前回の書き込みでスタブが載ったまま
                //  再び載せようとすると壊れて「USB へ送れません」になる。見分けて二度載せない。
                stubRunning = (value == 0)
                // ROM は同じ応答を何度か返すので、残りを吸っておく
                repeat(7) { runCatching { readPacket(100) } }
                return
            } catch (e: Exception) {
                last = e
            }
        }
        throw EspFlashError("ダウンロードモードの端末が応答しません: ${last?.message}")
    }

    /** 相手がもうスタブなら true([sync] が判定する)。 */
    fun isStubRunning(): Boolean = stubRunning

    /** RAM へスタブを送り込んで走らせる。以後の書き込みが速くなる。既に走っていれば何もしない。 */
    fun runStub(stub: EspStub) {
        if (stubRunning) { return }
        for ((data, addr) in listOf(stub.text to stub.textStart, stub.data to stub.dataStart)) {
            if (data.isEmpty()) continue
            val blocks = (data.size + RAM_BLOCK - 1) / RAM_BLOCK
            checkCommand(MEM_BEGIN, le32(data.size, blocks, RAM_BLOCK, addr), 0, 5000)
            for (seq in 0 until blocks) {
                val from = seq * RAM_BLOCK
                val chunk = data.copyOfRange(from, minOf(from + RAM_BLOCK, data.size))
                checkCommand(MEM_DATA, le32(chunk.size, seq, 0, 0) + chunk, checksum(chunk), 5000)
            }
        }
        // entrypoint を渡して RAM ダウンロードモードを抜ける = スタブが走り出す
        checkCommand(MEM_END, le32(0, stub.entry), 0, 3000)
        val hello = readPacket(3000)
        if (!hello.contentEquals("OHAI".toByteArray())) {
            throw EspFlashError("スタブが起動しませんでした")
        }
        stubRunning = true
    }

    /** SPI フラッシュの足を有効にする(ROM のときのみ引数が1つ多い)。 */
    fun spiAttach() {
        val arg = if (stubRunning) le32(0) else le32(0, 0)
        checkCommand(SPI_ATTACH, arg, 0, 3000)
    }

    /** 端末の素性を読む。**1バイトも書かない**ので、繋がりの確認に使える。 */
    fun probe(): EspChipInfo {
        val mac0 = readReg(MAC_EFUSE_REG)
        val mac1 = readReg(MAC_EFUSE_REG + 4)
        val mac = byteArrayOf(
            ((mac1 shr 8) and 0xFF).toByte(), (mac1 and 0xFF).toByte(),
            ((mac0 shr 24) and 0xFF).toByte(), ((mac0 shr 16) and 0xFF).toByte(),
            ((mac0 shr 8) and 0xFF).toByte(), (mac0 and 0xFF).toByte()
        ).joinToString(":") { "%02x".format(it) }

        spiAttach()
        val id = runSpiflashCommand(0x9F, ByteArray(0), 24)
        // RDID の3バイト目が容量の指数。0x17 なら 2^23 = 8MB。
        val sizeId = (id shr 16) and 0xFF
        val size = if (sizeId in 0x12..0x1B) (1 shl sizeId) else 0
        return EspChipInfo(mac, id, size)
    }

    /**
     * イメージを焼く。圧縮して送るので、転送量は実測で 55% ほどになる。
     * 焼いた後の起動は [watchdogReset] を使うこと(DTR/RTS のリセットでは戻らない)。
     */
    fun writeFlash(offset: Int, image: ByteArray, progress: EspProgress? = null) {
        val comp = deflate(image)
        val blocks = (comp.size + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE
        // スタブはバイト数を、ROM は消去ブロックに丸めた値を欲しがる
        val writeSize = if (stubRunning) image.size
                        else ((image.size + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE) * FLASH_WRITE_SIZE
        progress?.onProgress("erase", 0, image.size)
        checkCommand(FLASH_DEFL_BEGIN, le32(writeSize, blocks, FLASH_WRITE_SIZE, offset), 0, 60000)

        for (seq in 0 until blocks) {
            val from = seq * FLASH_WRITE_SIZE
            val chunk = comp.copyOfRange(from, minOf(from + FLASH_WRITE_SIZE, comp.size))
            checkCommand(FLASH_DEFL_DATA, le32(chunk.size, seq, 0, 0) + chunk, checksum(chunk), 30000)
            progress?.onProgress("write", minOf((seq + 1) * image.size / blocks, image.size), image.size)
        }
        // reboot=false。起動は watchdogReset に任せる。
        checkCommand(FLASH_DEFL_END, le32(1), 0, 5000)
    }

    /**
     * 端末のフラッシュを読む。**スタブが要る**(ROM だけのときは使えない)。
     *
     * 相手は要求した長さぶんを次々と送りつけてくるので、受け取った累計を都度返して
     * 流れを止めないようにする(返さないと相手は待ち続ける)。最後に MD5 が1つ届くので、
     * 受け取った中身と突き合わせて取りこぼしが無いことを確かめる。
     */
    fun readFlash(offset: Int, length: Int): ByteArray {
        if (!stubRunning) throw EspFlashError("読み出しにはスタブが要ります")
        val sector = 0x1000
        checkCommand(READ_FLASH, le32(offset, length, sector, 64), 0, 10000)
        val out = ByteArrayOutputStream(length)
        while (out.size() < length) {
            val p = readPacket(10000)
            out.write(p)
            if (out.size() < length && p.size < sector) {
                throw EspFlashError("読み出しが途切れました(%d/%d)".format(out.size(), length))
            }
            io.write(slipEncode(le32(out.size())))      // ここまで受け取った、と返す
        }
        val digest = readPacket(10000)
        if (digest.size != 16) throw EspFlashError("読み出しの照合値が来ません")
        val data = out.toByteArray()
        val want = digest.joinToString("") { "%02x".format(it) }
        if (md5hex(data) != want) throw EspFlashError("読み出した中身が壊れています")
        return data
    }

    /** MD5 の応答本体をそのまま返す(形が読めないときの手掛かり用)。 */
    fun flashMd5Raw(offset: Int, size: Int): ByteArray =
        command(SPI_FLASH_MD5, le32(offset, size, 0, 0), 0, 120000).second

    /**
     * 端末が計算した MD5。応答の形は相手によって変わる:
     *   ROM  … 16進32文字 + 末尾2バイトの成否
     *   スタブ … 生16バイト + 末尾2バイトの成否
     */
    fun flashMd5(offset: Int, size: Int): String {
        val body = checkCommand(SPI_FLASH_MD5, le32(offset, size, 0, 0), 0, 120000)
        return when {
            body.size >= 32 -> String(body, 0, 32).lowercase()
            body.size >= 16 -> body.copyOfRange(0, 16).joinToString("") { "%02x".format(it) }
            else -> ""
        }
    }

    /**
     * 端末の [offset] から [expect] と同じ中身が載っているか。読み出さず、端末に MD5 を
     * 計算させて突き合わせる(2.6MB でも数秒)。
     * 「今入っているのは同じ土台か」を焼く前に確かめるのに使う。
     */
    fun regionMatches(offset: Int, expect: ByteArray): Boolean {
        val got = flashMd5(offset, expect.size)
        return got.isNotEmpty() && got.equals(md5hex(expect), ignoreCase = true)
    }

    /** 焼いたものが本当に載っているかを端末側の MD5 で確かめる。 */
    fun verify(offset: Int, image: ByteArray): Boolean {
        val want = md5hex(image)
        val got = flashMd5(offset, image.size)
        return got.isNotEmpty() && got.equals(want, ignoreCase = true)
    }

    /**
     * RTC のウォッチドッグで自分を蹴らせて起動する。
     * **DTR/RTS のリセットではダウンロードモードから抜けられない**(実機で確認)。
     */
    fun watchdogReset() {
        runCatching { writeReg(RTC_OPTION1, 0, FORCE_DOWNLOAD_BOOT) }   // 強制DLモードの札を消す
        writeReg(RTC_WDTWPROTECT, RTC_WDT_WKEY)                          // 鍵を開ける
        writeReg(RTC_WDTCONFIG1, 2000)                                   // 時間切れまで
        writeReg(RTC_WDTCONFIG0, (1 shl 31) or (5 shl 28) or (1 shl 8) or 2)
        writeReg(RTC_WDTWPROTECT, 0)                                     // 鍵を閉じる
        stubRunning = false
    }

    // ── 部品 ────────────────────────────────────────────────

    fun readReg(addr: Int): Int {
        val (value, data) = command(READ_REG, le32(addr), 0, 3000)
        if (data.isNotEmpty() && data[0].toInt() != 0) {
            throw EspFlashError("番地 0x%08x が読めません".format(addr))
        }
        return value
    }

    fun writeReg(addr: Int, value: Int, mask: Int = -1, delayUs: Int = 0) {
        checkCommand(WRITE_REG, le32(addr, value, mask, delayUs), 0, 3000)
    }

    /**
     * SPI フラッシュへ生の命令を送る(容量を知るための RDID など)。
     * SPI 周辺回路の「ユーザ命令」機能を叩く。触ったレジスタは元へ戻す。
     */
    private fun runSpiflashCommand(cmd: Int, data: ByteArray, readBits: Int): Int {
        val usrReg = SPI_REG_BASE + SPI_USR_OFFS
        val usr2Reg = SPI_REG_BASE + SPI_USR2_OFFS
        val w0Reg = SPI_REG_BASE + SPI_W0_OFFS
        val cmdReg = SPI_REG_BASE + 0x00

        val oldUsr = readReg(usrReg)
        val oldUsr2 = readReg(usr2Reg)
        val dataBits = data.size * 8

        var flags = 1 shl 31                       // USR_COMMAND
        if (readBits > 0) flags = flags or (1 shl 28)   // USR_MISO
        if (dataBits > 0) flags = flags or (1 shl 27)   // USR_MOSI
        if (dataBits > 0) writeReg(SPI_REG_BASE + SPI_MOSI_DLEN_OFFS, dataBits - 1)
        if (readBits > 0) writeReg(SPI_REG_BASE + SPI_MISO_DLEN_OFFS, readBits - 1)
        writeReg(usrReg, flags)
        writeReg(usr2Reg, (7 shl 28) or cmd)
        if (dataBits == 0) {
            writeReg(w0Reg, 0)
        } else {
            val padded = data.copyOf((data.size + 3) / 4 * 4)
            for (i in padded.indices step 4) {
                var w = 0
                for (k in 0 until 4) w = w or ((padded[i + k].toInt() and 0xFF) shl (8 * k))
                writeReg(w0Reg + i, w)
            }
        }
        writeReg(cmdReg, 1 shl 18)                 // CMD_USR

        var done = false
        for (i in 0 until 10) {
            if (readReg(cmdReg) and (1 shl 18) == 0) { done = true; break }
        }
        if (!done) throw EspFlashError("SPI の命令が終わりません")

        val status = readReg(w0Reg)
        writeReg(usrReg, oldUsr)
        writeReg(usr2Reg, oldUsr2)
        return status
    }

    /** 応答の末尾2バイトを見て成否を判定する。成功なら本体(あれば)を返す。 */
    private fun checkCommand(op: Int, data: ByteArray, chk: Int, timeoutMs: Int): ByteArray {
        val (_, body) = command(op, data, chk, timeoutMs)
        if (body.size < STATUS_BYTES) {
            throw EspFlashError("命令 0x%02x の応答が短すぎます(%d バイト)".format(op, body.size))
        }
        val status = body[body.size - STATUS_BYTES].toInt() and 0xFF
        if (status != 0) {
            val reason = body[body.size - STATUS_BYTES + 1].toInt() and 0xFF
            throw EspFlashError("命令 0x%02x が失敗(status=%d reason=0x%02x)".format(op, status, reason))
        }
        return body.copyOfRange(0, body.size - STATUS_BYTES)
    }

    /** 1つ送って、同じ命令の応答が返るまで読む。戻りは (ヘッダのvalue, 本体)。 */
    private fun command(op: Int, data: ByteArray, chk: Int, timeoutMs: Int): Pair<Int, ByteArray> {
        io.write(slipEncode(buildCommand(op, data, chk)))
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            val pkt = readPacket((deadline - System.currentTimeMillis()).toInt().coerceAtLeast(1))
            val r = parseResponse(pkt) ?: continue
            if (r.op == op) return r.value to r.body
        }
        throw EspFlashError("命令 0x%02x の応答がありません".format(op))
    }

    private fun readPacket(timeoutMs: Int): ByteArray {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (true) {
            rx.take()?.let { return it }
            if (System.currentTimeMillis() >= deadline) throw EspFlashError("応答待ちが時間切れ")
            val got = io.read(4096, (deadline - System.currentTimeMillis()).toInt().coerceAtLeast(1))
            if (got.isNotEmpty()) rx.feed(got)
        }
    }

    /** 届いたバイトを溜めて、包みが1つ揃うたびに取り出せるようにする。 */
    class SlipDecoder {
        private val packets = ArrayDeque<ByteArray>()
        private var partial: ByteArrayOutputStream? = null
        private var escaped = false

        fun feed(bytes: ByteArray) {
            for (raw in bytes) {
                val b = raw.toInt() and 0xFF
                val p = partial
                if (p == null) {
                    // 包みの外。0xC0 が来るまでは端末の生ログなので読み飛ばす。
                    if (b == 0xC0) partial = ByteArrayOutputStream(64)
                    continue
                }
                when {
                    escaped -> {
                        escaped = false
                        when (b) {
                            0xDC -> p.write(0xC0)
                            0xDD -> p.write(0xDB)
                            else -> { partial = null }      // 壊れた包みは捨てる
                        }
                    }
                    b == 0xDB -> escaped = true
                    b == 0xC0 -> {
                        // 中身が空なら、直前の終端と続けて来た開始とみなして仕切り直す
                        if (p.size() == 0) partial = ByteArrayOutputStream(64)
                        else { packets.addLast(p.toByteArray()); partial = null }
                    }
                    else -> p.write(b)
                }
            }
        }

        fun take(): ByteArray? = packets.removeFirstOrNull()

        fun reset() { packets.clear(); partial = null; escaped = false }
    }

    // ── ここから下は経路に依らない純粋な部分(単体試験の対象) ──────────
    companion object Wire {

        // ROM ローダの命令
        const val FLASH_BEGIN = 0x02
        const val FLASH_DATA = 0x03
        const val FLASH_END = 0x04
        const val MEM_BEGIN = 0x05
        const val MEM_END = 0x06
        const val MEM_DATA = 0x07
        const val SYNC = 0x08
        const val WRITE_REG = 0x09
        const val READ_REG = 0x0A
        const val SPI_ATTACH = 0x0D
        const val FLASH_DEFL_BEGIN = 0x10
        const val FLASH_DEFL_DATA = 0x11
        const val FLASH_DEFL_END = 0x12
        const val SPI_FLASH_MD5 = 0x13
        const val READ_FLASH = 0xD2         // スタブだけが持つ(ROM には無い)

        const val CHECKSUM_MAGIC = 0xEF
        const val STATUS_BYTES = 2          // 応答の末尾2バイトが成否
        const val RAM_BLOCK = 0x1800        // RAM へ送るときの1回分
        const val FLASH_WRITE_SIZE = 0x400  // フラッシュへ送るときの1回分

        // ESP32-S3 の番地(esptool の targets/esp32s3.py と同じ)
        const val EFUSE_BASE = 0x60007000
        const val MAC_EFUSE_REG = EFUSE_BASE + 0x044
        const val SPI_REG_BASE = 0x60002000
        const val SPI_USR_OFFS = 0x18
        const val SPI_USR1_OFFS = 0x1C
        const val SPI_USR2_OFFS = 0x20
        const val SPI_MOSI_DLEN_OFFS = 0x24
        const val SPI_MISO_DLEN_OFFS = 0x28
        const val SPI_W0_OFFS = 0x58
        const val RTCCNTL_BASE = 0x60008000
        const val RTC_WDTCONFIG0 = RTCCNTL_BASE + 0x0098
        const val RTC_WDTCONFIG1 = RTCCNTL_BASE + 0x009C
        const val RTC_WDTWPROTECT = RTCCNTL_BASE + 0x00B0
        const val RTC_WDT_WKEY = 0x50D83AA1
        const val RTC_OPTION1 = 0x6000812C
        const val FORCE_DOWNLOAD_BOOT = 0x1

        /** MD5 を16進文字列で。端末が言う値と突き合わせるため。 */
        fun md5hex(data: ByteArray): String =
            MessageDigest.getInstance("MD5").digest(data).joinToString("") { "%02x".format(it) }

        /** ROM の検査値。1バイトずつ排他的論理和を取るだけ。 */
        fun checksum(data: ByteArray): Int {
            var s = CHECKSUM_MAGIC
            for (b in data) s = s xor (b.toInt() and 0xFF)
            return s
        }

        /** 命令の包み: 0x00, 命令, 長さ(16bit), 検査値(32bit), 本体。すべてリトルエンディアン。 */
        fun buildCommand(op: Int, data: ByteArray, chk: Int): ByteArray {
            val out = ByteArray(8 + data.size)
            out[0] = 0x00
            out[1] = op.toByte()
            out[2] = (data.size and 0xFF).toByte()
            out[3] = ((data.size shr 8) and 0xFF).toByte()
            out[4] = (chk and 0xFF).toByte()
            out[5] = ((chk shr 8) and 0xFF).toByte()
            out[6] = ((chk shr 16) and 0xFF).toByte()
            out[7] = ((chk shr 24) and 0xFF).toByte()
            data.copyInto(out, 8)
            return out
        }

        class Response(val op: Int, val value: Int, val body: ByteArray)

        /**
         * 応答の先頭は 0x01。違うもの・短いものは黙って捨てる(ROM は雑音も混ぜてくる)。
         *
         * 【長さの欄を信用しないこと(2026-08-26 実機で判明)】ヘッダには長さが入っているが、
         *  中身より短い値を入れてくる相手が居る。実機のスタブは MD5(16バイト)+成否(2バイト)
         *  =18バイトを送りながら、長さの欄には 2 と書いてきた。長さで切ると MD5 の先頭2バイト
         *  だけが残り、それを成否と読んで「書き込んだ中身が一致しません」という嘘の失敗になる。
         *  esptool も長さの欄は使わず、頭8バイトの後ろを丸ごと本体として扱っている。
         */
        fun parseResponse(pkt: ByteArray): Response? {
            if (pkt.size < 8) return null
            if (pkt[0].toInt() != 0x01) return null
            val op = pkt[1].toInt() and 0xFF
            var value = 0
            for (k in 0 until 4) value = value or ((pkt[4 + k].toInt() and 0xFF) shl (8 * k))
            return Response(op, value, pkt.copyOfRange(8, pkt.size))
        }

        /** SLIP の包み。0xC0 で挟み、中の 0xC0 と 0xDB を逃がす。 */
        fun slipEncode(packet: ByteArray): ByteArray {
            val out = ByteArrayOutputStream(packet.size + 16)
            out.write(0xC0)
            for (b in packet) {
                when (b.toInt() and 0xFF) {
                    0xDB -> { out.write(0xDB); out.write(0xDD) }
                    0xC0 -> { out.write(0xDB); out.write(0xDC) }
                    else -> out.write(b.toInt())
                }
            }
            out.write(0xC0)
            return out.toByteArray()
        }

        /** 32bit をリトルエンディアンで並べる。 */
        fun le32(vararg values: Int): ByteArray {
            val out = ByteArray(values.size * 4)
            for (i in values.indices) {
                val v = values[i]
                for (k in 0 until 4) out[i * 4 + k] = ((v shr (8 * k)) and 0xFF).toByte()
            }
            return out
        }

        /** ROM が展開できる形(zlib)に縮める。実測で 2.6MB が 1.5MB になる。 */
        fun deflate(data: ByteArray): ByteArray {
            val d = Deflater(Deflater.BEST_COMPRESSION)
            d.setInput(data)
            d.finish()
            val out = ByteArrayOutputStream(data.size / 2)
            val buf = ByteArray(64 * 1024)
            while (!d.finished()) {
                val n = d.deflate(buf)
                if (n > 0) out.write(buf, 0, n)
            }
            d.end()
            return out.toByteArray()
        }
    }
}
