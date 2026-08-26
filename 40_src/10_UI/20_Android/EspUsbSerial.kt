package app.laxei.holygrail

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import java.io.ByteArrayOutputStream

// エッジ端末(ESP32-S3)と USB でバイトをやり取りする(2026-08-26)。
//
// 【素の USB Host API だけで足りる】ESP32-S3 が出しているのは普通の CDC-ACM なので、
//  DTR/RTS を動かす制御転送ひとつと、送受信のバルク転送だけで話せる。
//  外部のシリアルライブラリは要らない。
//
// 【端末の見え方は3通り(実測)】Android は PID が変わると別の機器として扱うので、
//  どれでも掴めるようにしておく。
//   303A:8120  買ってきたばかり(M5 の工場出荷ファーム。TinyUSB)
//   303A:1001  ROM のダウンロードモード(USB-Serial/JTAG)
//   303A:1001  私たちのファーム(同じ USB-Serial/JTAG を使っている)
//
// 【新品は自動でダウンロードモードに入れられない(実測)】工場出荷のファームは DTR/RTS を
//  見ておらず、そもそもホストからの書き込みを受け取らない。**電源ボタン長押し2秒**
//  (LED が点滅する)で手で入れてもらうしかない。私たちのファームが入った後は
//  [usbJtagResetToDownload] で自動的に入れられる。

object EspUsb {
    const val VID_ESPRESSIF = 0x303A
    const val PID_USB_JTAG = 0x1001        // ROM のダウンロードモード / 私たちのファーム
    const val PID_M5_FACTORY = 0x8120      // M5 の工場出荷ファーム

    private const val ACTION_PERMISSION = "app.laxei.holygrail.USB_PERMISSION"

    /** 繋がっている中から、焼ける相手(Espressif の USB)を探す。 */
    fun findDevice(ctx: Context): UsbDevice? {
        val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        return um.deviceList.values.firstOrNull { it.vendorId == VID_ESPRESSIF }
    }

    /** 今その機器を触ってよいか。 */
    fun hasPermission(ctx: Context, dev: UsbDevice): Boolean {
        val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        return um.hasPermission(dev)
    }

    /** 触ってよいか聞く。答えは ACTION_PERMISSION の broadcast で返る。 */
    fun requestPermission(ctx: Context, dev: UsbDevice) {
        val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
        val pi = PendingIntent.getBroadcast(
            ctx, 0, Intent(ACTION_PERMISSION).setPackage(ctx.packageName),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        )
        um.requestPermission(dev, pi)
    }

    fun permissionAction(): String = ACTION_PERMISSION

    /** その機器が今どういう顔をしているか(案内の文言を変えるのに使う)。 */
    fun describe(dev: UsbDevice): String = when (dev.productId) {
        PID_USB_JTAG -> "ダウンロードモード または 本体ファーム"
        PID_M5_FACTORY -> "工場出荷のファーム"
        else -> "不明 (PID 0x%04X)".format(dev.productId)
    }
}

/**
 * CDC-ACM の口。EspFlasher に渡して使う。
 * 使い終わったら必ず [close] すること(掴んだままだと次に開けない)。
 */
class EspUsbSerial private constructor(
    private val conn: UsbDeviceConnection,
    private val dataIface: UsbInterface,
    private val ctrlIfaceId: Int,
    private val epIn: UsbEndpoint,
    private val epOut: UsbEndpoint
) : EspTransport, AutoCloseable {

    // 読みは要求より多く届くことがあるので、余りをここに残して次に回す
    private var spill = ByteArray(0)

    companion object {
        private const val CDC_SET_LINE_CODING = 0x20
        private const val CDC_SET_CONTROL_LINE_STATE = 0x22
        private const val REQTYPE_HOST_TO_DEVICE_CLASS_IFACE = 0x21

        /**
         * 機器を開く。CDC のデータ用インタフェース(クラス 0x0A)を探して掴む。
         * 権限が無い・口が見つからない場合は [EspFlashError]。
         */
        fun open(ctx: Context, dev: UsbDevice): EspUsbSerial {
            val um = ctx.getSystemService(Context.USB_SERVICE) as UsbManager
            if (!um.hasPermission(dev)) throw EspFlashError("USB の使用が許可されていません")

            var data: UsbInterface? = null
            var ctrlId = 0
            for (i in 0 until dev.interfaceCount) {
                val itf = dev.getInterface(i)
                when (itf.interfaceClass) {
                    UsbConstants.USB_CLASS_CDC_DATA -> if (data == null) data = itf
                    UsbConstants.USB_CLASS_COMM -> ctrlId = itf.id
                }
            }
            // クラスで見つからない機器のために、バルクを2本持つインタフェースでも拾う
            if (data == null) {
                for (i in 0 until dev.interfaceCount) {
                    val itf = dev.getInterface(i)
                    val bulk = (0 until itf.endpointCount).map { itf.getEndpoint(it) }
                        .filter { it.type == UsbConstants.USB_ENDPOINT_XFER_BULK }
                    if (bulk.size >= 2) { data = itf; break }
                }
            }
            val di = data ?: throw EspFlashError("CDC のデータ用インタフェースが見つかりません")

            var epIn: UsbEndpoint? = null
            var epOut: UsbEndpoint? = null
            for (i in 0 until di.endpointCount) {
                val ep = di.getEndpoint(i)
                if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                if (ep.direction == UsbConstants.USB_DIR_IN) epIn = epIn ?: ep else epOut = epOut ?: ep
            }
            if (epIn == null || epOut == null) throw EspFlashError("USB の送受信の口が見つかりません")

            val conn = um.openDevice(dev) ?: throw EspFlashError("USB を開けません")
            if (!conn.claimInterface(di, true)) {
                conn.close()
                throw EspFlashError("USB のインタフェースを掴めません")
            }
            val port = EspUsbSerial(conn, di, ctrlId, epIn, epOut)
            // USB-Serial/JTAG では速度に意味は無いが、CDC の作法として一度入れておく。
            port.setLineCoding(115200)
            // 開けた時点では DTR/RTS を寝かせておく。**この2本は IO0 と EN に繋がっている**ので、
            //  不用意に動かすと相手を再起動させてしまう。
            port.setControlLines(dtr = false, rts = false)
            return port
        }
    }

    // ── EspTransport ────────────────────────────────────────

    override fun write(data: ByteArray) {
        // 【まとめて渡す】1パケットずつに割るとパケットの切れ目が不自然になり、相手の
        //  受け取り方によっては詰まる。bulkTransfer は中で勝手に分割してくれるので、
        //  こちらは丸ごと渡してよい(1回の送信は最大でもスタブの 6KB 程度)。
        var off = 0
        while (off < data.size) {
            val chunk = if (off == 0) data else data.copyOfRange(off, data.size)
            val sent = conn.bulkTransfer(epOut, chunk, chunk.size, 3000)
            if (sent < 0) throw EspFlashError("USB へ送れません")
            if (sent == 0) throw EspFlashError("USB へ送れません(0バイト)")
            off += sent
        }
    }

    override fun read(max: Int, timeoutMs: Int): ByteArray {
        if (spill.isNotEmpty()) {
            val take = minOf(max, spill.size)
            val out = spill.copyOfRange(0, take)
            spill = spill.copyOfRange(take, spill.size)
            return out
        }
        val buf = ByteArray(maxOf(epIn.maxPacketSize, minOf(max, 16384)))
        val n = conn.bulkTransfer(epIn, buf, buf.size, timeoutMs.coerceAtLeast(1))
        if (n <= 0) return ByteArray(0)          // 時間切れは空。締切は呼ぶ側が見る
        return if (n <= max) buf.copyOfRange(0, n).also { }
        else {
            spill = buf.copyOfRange(max, n)
            buf.copyOfRange(0, max)
        }
    }

    override fun setControlLines(dtr: Boolean, rts: Boolean) {
        val value = (if (dtr) 1 else 0) or (if (rts) 2 else 0)
        conn.controlTransfer(
            REQTYPE_HOST_TO_DEVICE_CLASS_IFACE, CDC_SET_CONTROL_LINE_STATE,
            value, ctrlIfaceId, null, 0, 1000
        )
    }

    override fun discardInput() {
        spill = ByteArray(0)
        val buf = ByteArray(epIn.maxPacketSize)
        // 残っているものを短い待ちで吸い出す
        while (conn.bulkTransfer(epIn, buf, buf.size, 20) > 0) { /* 捨てる */ }
    }

    // ── ここから USB ならではの部分 ───────────────────────────

    private fun setLineCoding(baud: Int) {
        val d = ByteArrayOutputStream(7)
        for (k in 0 until 4) d.write((baud shr (8 * k)) and 0xFF)
        d.write(0)      // ストップビット 1
        d.write(0)      // パリティ 無し
        d.write(8)      // データ 8bit
        val b = d.toByteArray()
        conn.controlTransfer(
            REQTYPE_HOST_TO_DEVICE_CLASS_IFACE, CDC_SET_LINE_CODING,
            0, ctrlIfaceId, b, b.size, 1000
        )
    }

    /**
     * DTR/RTS を順に動かしてダウンロードモードへ入れる(USB-Serial/JTAG 用の手順)。
     * **私たちのファームが入っている端末にだけ効く。**工場出荷のファームは DTR/RTS を
     * 見ていないので効かない(実測)。その場合は電源ボタン長押しで手で入れてもらう。
     *
     * この2本はチップの中で IO0 と EN に繋がっていて、(1,1) を経由して落とすのが作法。
     * (0,0) を通すと Windows 側で DTR が伝わらないことがある、と esptool に注釈がある。
     */
    fun usbJtagResetToDownload() {
        setControlLines(dtr = false, rts = false); Thread.sleep(100)   // 静かな状態
        setControlLines(dtr = true, rts = false); Thread.sleep(100)    // IO0 を落とす
        setControlLines(dtr = false, rts = true)                       // EN を落とす
        setControlLines(dtr = false, rts = true); Thread.sleep(100)
        setControlLines(dtr = false, rts = false)                      // 離す = 起動
        Thread.sleep(200)
        discardInput()
    }

    override fun close() {
        runCatching { conn.releaseInterface(dataIface) }
        runCatching { conn.close() }
    }
}
