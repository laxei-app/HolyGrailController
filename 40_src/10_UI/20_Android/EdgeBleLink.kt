package app.laxei.holygrail

// スマホ⇄エッジの ETP を BLE で運ぶ経路(エッジ側は 18_M5Common/etpBle.cpp)。
//
// 【なぜ要るか】屋外でルーターが無い運用ではエッジ自身が AP になり、カメラがそこへ繋がる。
//  スマホもその AP へ入らないと話せないので、エッジが複数台あると SSID を切り替えて回る
//  ことになる。BLE ならスマホは Wi-Fi を離れずに全台と話せる。
//
// 【プロトコルは変えない】ETP のフレームをそのまま write / notify で運ぶだけ。ETP は
//  自分でフレーム長を持っているので、分割の面倒はこちらで吸収でき、独自ヘッダは要らない。
//
// 【同期呼び出し】ネイティブ(edgeClient.cpp)の作業スレッドから exchange() が同期で呼ばれる。
//  GATT は非同期なので、ここでラッチを使って「1往復ぶん」を待ち合わせる。UIスレッドからは
//  呼ばないこと(呼ぶとフリーズする)。
//
// 【接続は張りっぱなし】TCP の永続接続と同じ考え方。毎回つなぎ直すと遅く、BLE では特に高い。
//  相手が変わったときと、切れたときだけ張り直す。

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import java.io.ByteArrayOutputStream
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock

@SuppressLint("MissingPermission")   // 権限確認は呼び出し側(MainActivity)で済ませる
object EdgeBleLink {
    // ETP 用 GATT(エッジ側 etpBle.cpp と同じ)。プロビジョニング用(...-0001-...)とは別サービス。
    private val SVC  = UUID.fromString("a1b2c3d4-0002-4a5b-8c6d-000000000001")
    private val RXC  = UUID.fromString("a1b2c3d4-0002-4a5b-8c6d-000000000002")  // write : スマホ→エッジ
    private val TXC  = UUID.fromString("a1b2c3d4-0002-4a5b-8c6d-000000000003")  // notify: エッジ→スマホ
    private val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    private const val TAG = "EdgeBleLink"

    private var appCtx: Context? = null
    fun init(ctx: Context) { appCtx = ctx.applicationContext }

    // --- 接続状態(exchange の中だけで触る。lock で直列化) ---
    private val lock = ReentrantLock()
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null   // 書き込み先
    private var txChar: BluetoothGattCharacteristic? = null   // 通知元
    private var linkedName: String = ""     // いま繋がっている端末名(HGC- を除いた名前)
    @Volatile private var mtu: Int = 23

    // --- コールバックとの待ち合わせ ---
    @Volatile private var latchConn: CountDownLatch? = null
    @Volatile private var latchMtu: CountDownLatch? = null
    @Volatile private var latchSvc: CountDownLatch? = null
    @Volatile private var latchDesc: CountDownLatch? = null
    @Volatile private var latchWrite: CountDownLatch? = null
    @Volatile private var connected = false
    private val rxBuf = ByteArrayOutputStream()
    @Volatile private var latchReply: CountDownLatch? = null

    private fun log(s: String) { android.util.Log.i(TAG, s) }

    // ETP フレームが1つ揃っているか。揃っていればその全長、まだなら 0。
    //  書式: header(2) cmd(2) method(2) length(4) data[length] terminal(4) sum(4)
    private fun frameLen(b: ByteArray): Int {
        if (b.size < 10) return 0
        if ((b[0].toInt() and 0xff) != 0x80 || (b[1].toInt() and 0xff) != 0x80) return -1  // 同期外れ
        var len = 0L
        for (i in 0..3) len = len or ((b[6 + i].toLong() and 0xff) shl (8 * i))
        val total = 18 + len
        if (total > 1 shl 20) return -1                 // 異常な長さ
        return if (b.size >= total) total.toInt() else 0
    }

    private val cb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                connected = true
                latchConn?.countDown()
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connected = false
                // 待っている誰かを解放してから片付ける(掴んだまま固まらないように)。
                latchConn?.countDown(); latchMtu?.countDown(); latchSvc?.countDown()
                latchDesc?.countDown(); latchWrite?.countDown(); latchReply?.countDown()
                log("disconnected")
            }
        }
        override fun onMtuChanged(g: BluetoothGatt, m: Int, status: Int) {
            mtu = if (m > 23) m else 23
            latchMtu?.countDown()
        }
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) { latchSvc?.countDown() }
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) { latchDesc?.countDown() }
        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (c.uuid == RXC) latchWrite?.countDown()
        }
        // API 33+ / それ以前の両方を受ける
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            if (c.uuid == TXC) onNotify(value)
        }
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            @Suppress("DEPRECATION")
            if (c.uuid == TXC) onNotify(c.value ?: ByteArray(0))
        }
    }

    private fun onNotify(v: ByteArray) {
        synchronized(rxBuf) {
            rxBuf.write(v)
            // 1フレーム揃ったら待っている exchange を起こす
            if (frameLen(rxBuf.toByteArray()) != 0) latchReply?.countDown()
        }
    }

    // 一度見つけた端末のアドレスを覚えておく。毎回スキャンすると1台につき数秒かかり、
    // 常時スイープ(30秒周期)が回らなくなるため。アドレスが分かっていれば直接つなげる。
    private val addrCache = HashMap<String, String>()
    // 見つからなかった端末を、しばらく探し直さないための時刻(電源が入っていない端末を
    // 毎回フルスキャンしないようにする)。
    private val missUntil = HashMap<String, Long>()
    private const val MISS_BACKOFF_MS = 60_000L

    // 名前でエッジを探す。見つかった BluetoothDevice を返す(タイムアウトで null)。
    private fun findDevice(name: String, timeoutMs: Long): BluetoothDevice? {
        val ctx = appCtx ?: return null
        val mgr = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return null
        val ad = mgr.adapter ?: return null
        if (!ad.isEnabled) return null
        // 覚えているアドレスがあればスキャンを飛ばす(繋がらなければ後段で捨てて再スキャンになる)。
        addrCache[name]?.let { a ->
            try { return ad.getRemoteDevice(a) } catch (_: Exception) { addrCache.remove(name) }
        }
        val now = System.currentTimeMillis()
        (missUntil[name] ?: 0L).let { if (now < it) return null }   // 直近で見つからなかった端末は待つ
        val scanner = ad.bluetoothLeScanner ?: return null
        val want = "HGC-$name"
        var found: BluetoothDevice? = null
        val latch = CountDownLatch(1)
        val sc = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val n = try { r.scanRecord?.deviceName } catch (_: Exception) { null }
                    ?: try { r.device?.name } catch (_: SecurityException) { null }
                // 見えた端末はすべて覚える(次回そのぶんスキャンを省ける)
                if (n != null && n.startsWith("HGC-")) addrCache[n.removePrefix("HGC-")] = r.device.address
                if (n == want) { found = r.device; latch.countDown() }
            }
        }
        // ETP サービスで絞る(プロビジョニング専用の旧ファームは拾わない)。
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try {
            scanner.startScan(listOf(filter), settings, sc)
            latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        } catch (_: Exception) {
        } finally {
            try { scanner.stopScan(sc) } catch (_: Exception) {}
        }
        if (found == null) { missUntil[name] = System.currentTimeMillis() + MISS_BACKOFF_MS }
        return found
    }

    private fun closeLink() {
        try { gatt?.disconnect() } catch (_: Exception) {}
        try { gatt?.close() } catch (_: Exception) {}
        gatt = null; rxChar = null; txChar = null; linkedName = ""; connected = false; mtu = 23
    }

    // 目的の端末へ繋がった状態にする。成功で true。
    private fun ensureLink(target: String): Boolean {
        if (connected && linkedName == target && rxChar != null && txChar != null) return true
        closeLink()
        val ctx = appCtx ?: return false
        val dev = findDevice(target, 6000) ?: run { log("not found: $target"); return false }

        latchConn = CountDownLatch(1)
        gatt = dev.connectGatt(ctx, false, cb)
        if (latchConn?.await(10, TimeUnit.SECONDS) != true || !connected) {
            addrCache.remove(target)   // 覚えていたアドレスが古い(繋がらない) → 次は探し直す
            closeLink(); return false
        }

        latchMtu = CountDownLatch(1)
        try { gatt?.requestMtu(517) } catch (_: Exception) {}
        latchMtu?.await(5, TimeUnit.SECONDS)

        latchSvc = CountDownLatch(1)
        try { gatt?.discoverServices() } catch (_: Exception) {}
        if (latchSvc?.await(10, TimeUnit.SECONDS) != true) { closeLink(); return false }

        val svc = gatt?.getService(SVC) ?: run { log("no ETP service"); closeLink(); return false }
        rxChar = svc.getCharacteristic(RXC)
        txChar = svc.getCharacteristic(TXC)
        if (rxChar == null || txChar == null) { log("no ETP characteristics"); closeLink(); return false }

        // 通知を有効にする(CCCD への書き込みまで済ませないと notify は来ない)
        gatt?.setCharacteristicNotification(txChar, true)
        val d = txChar?.getDescriptor(CCCD)
        if (d != null) {
            latchDesc = CountDownLatch(1)
            if (Build.VERSION.SDK_INT >= 33) {
                gatt?.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                run { d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; gatt?.writeDescriptor(d) }
            }
            latchDesc?.await(5, TimeUnit.SECONDS)
        }
        linkedName = target
        log("linked to $target (mtu=$mtu)")
        return true
    }

    private fun writeChunk(c: BluetoothGattCharacteristic, part: ByteArray): Boolean {
        latchWrite = CountDownLatch(1)
        val g = gatt ?: return false
        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(c, part, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                c.value = part
                g.writeCharacteristic(c)
            }
        }
        if (!ok) return false
        return latchWrite?.await(5, TimeUnit.SECONDS) == true
    }

    // ETP を 1 往復する。ネイティブから同期で呼ばれる。失敗は null。
    fun exchange(target: String, frame: ByteArray, timeoutMs: Int): ByteArray? {
        lock.lock()
        try {
            log("exchange -> $target ${frame.size}B")
            if (!ensureLink(target)) { log("no link: $target"); return null }
            val c = rxChar ?: return null
            synchronized(rxBuf) { rxBuf.reset() }
            latchReply = CountDownLatch(1)

            val chunk = (mtu - 3).coerceAtLeast(20)
            var off = 0
            while (off < frame.size) {
                val n = minOf(chunk, frame.size - off)
                if (!writeChunk(c, frame.copyOfRange(off, off + n))) { log("write failed at $off"); closeLink(); return null }
                off += n
            }

            val deadline = System.currentTimeMillis() + timeoutMs
            while (System.currentTimeMillis() < deadline) {
                latchReply?.await(200, TimeUnit.MILLISECONDS)
                val cur = synchronized(rxBuf) { rxBuf.toByteArray() }
                val fl = frameLen(cur)
                if (fl > 0) { log("reply ${fl}B from $target"); return cur.copyOfRange(0, fl) }
                if (fl < 0) { synchronized(rxBuf) { rxBuf.reset() } }   // 同期外れ → 捨てて待ち直す
                if (!connected) return null
            }
            log("reply timeout ($target)")
            return null
        } catch (e: Exception) {
            log("exchange failed: ${e.message}")
            closeLink()
            return null
        } finally {
            lock.unlock()
        }
    }

    // BLE モードでのエッジ探索。Wi-Fi のブロードキャスト検索の代わり。
    //  見つかった端末名(HGC- を除いたもの)を返す。
    fun scanEdgeNames(timeoutMs: Long): List<String> {
        val ctx = appCtx ?: return emptyList()
        val mgr = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return emptyList()
        val ad = mgr.adapter ?: return emptyList()
        if (!ad.isEnabled) return emptyList()
        val scanner = ad.bluetoothLeScanner ?: return emptyList()
        val names = LinkedHashSet<String>()
        val sc = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val n = try { r.scanRecord?.deviceName } catch (_: Exception) { null }
                    ?: try { r.device?.name } catch (_: SecurityException) { null }
                if (n != null && n.startsWith("HGC-")) names.add(n.removePrefix("HGC-"))
            }
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        try {
            scanner.startScan(listOf(filter), settings, sc)
            Thread.sleep(timeoutMs)
        } catch (_: Exception) {
        } finally {
            try { scanner.stopScan(sc) } catch (_: Exception) {}
        }
        return names.toList()
    }

    // 通信路を切り替えるときなどに掴んでいる接続を捨てる。
    fun close() { lock.lock(); try { closeLink() } finally { lock.unlock() } }
}
