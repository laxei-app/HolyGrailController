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
// 【接続は端末ごとに張りっぱなし】TCP の永続接続と同じ考え方。接続を1本だけ持つ実装にすると、
//  常時スイープが複数台を交互に叩くたびに張り直しになり(1回 1.5〜2秒)、しかも「居ない端末」を
//  叩いた瞬間に生きている接続まで落ちる(2026-08-14 実測: 撮影中に 30 秒ごとの切断→再接続)。
//  そこで端末名ごとに接続を持ち、使う相手が変わっても他は落とさない。
//  Android の GATT クライアント数には上限があるので、古いものから閉じて MAX_LINKS 本に抑える。

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
import java.util.concurrent.ConcurrentHashMap
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
    private const val MAX_LINKS = 4     // Android の同時 GATT 接続に余裕を持たせる

    private var appCtx: Context? = null
    fun init(ctx: Context) { appCtx = ctx.applicationContext }

    private fun log(s: String) { android.util.Log.i(TAG, s) }

    // ETP フレームが1つ揃っているか。揃っていればその全長、まだなら 0、同期外れなら -1。
    //  書式: header(2) cmd(2) method(2) length(4) data[length] terminal(4) sum(4)
    private fun frameLen(b: ByteArray): Int {
        if (b.size < 10) return 0
        if ((b[0].toInt() and 0xff) != 0x80 || (b[1].toInt() and 0xff) != 0x80) return -1
        var len = 0L
        for (i in 0..3) len = len or ((b[6 + i].toLong() and 0xff) shl (8 * i))
        val total = 18 + len
        if (total > 1 shl 20) return -1                 // 異常な長さ
        return if (b.size >= total) total.toInt() else 0
    }

    // 1台ぶんの接続。コールバックが「どの端末のものか」を持てるよう、端末ごとに作る。
    private class Link(val name: String) {
        var gatt: BluetoothGatt? = null
        var rx: BluetoothGattCharacteristic? = null
        var tx: BluetoothGattCharacteristic? = null
        @Volatile var mtu: Int = 23
        @Volatile var connected = false
        var usedAt: Long = 0L                       // 最後に使った時刻(古いものから閉じる)
        val rxBuf = ByteArrayOutputStream()
        @Volatile var latchConn: CountDownLatch? = null
        @Volatile var latchMtu: CountDownLatch? = null
        @Volatile var latchSvc: CountDownLatch? = null
        @Volatile var latchDesc: CountDownLatch? = null
        @Volatile var latchWrite: CountDownLatch? = null
        @Volatile var latchReply: CountDownLatch? = null

        val cb = object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    connected = true
                    latchConn?.countDown()
                } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                    connected = false
                    // 待っている誰かを解放してから片付ける(掴んだまま固まらないように)。
                    latchConn?.countDown(); latchMtu?.countDown(); latchSvc?.countDown()
                    latchDesc?.countDown(); latchWrite?.countDown(); latchReply?.countDown()
                    android.util.Log.i(TAG, "disconnected (" + name + ")")
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

        fun ready(): Boolean = connected && rx != null && tx != null

        fun close() {
            try { gatt?.disconnect() } catch (_: Exception) {}
            try { gatt?.close() } catch (_: Exception) {}
            gatt = null; rx = null; tx = null; connected = false; mtu = 23
        }
    }

    // --- 接続状態(exchange の中だけで触る。lock で直列化) ---
    private val lock = ReentrantLock()
    // スキャン(探索)は lock を取らずに走査するので、素の HashMap では壊れる。
    private val links = ConcurrentHashMap<String, Link>()

    // 一度見つけた端末のアドレスを覚えておく。毎回スキャンすると1台につき数秒かかり、
    // 常時スイープ(30秒周期)が回らなくなるため。アドレスが分かっていれば直接つなげる。
    private val addrCache = ConcurrentHashMap<String, String>()
    // 見つからなかった端末を、しばらく探し直さないための時刻(電源が入っていない端末を
    // 毎回フルスキャンしないようにする)。
    private val missUntil = ConcurrentHashMap<String, Long>()
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
        val want = "HGC-" + name
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

    private fun dropLink(name: String) {
        links.remove(name)?.close()
    }

    // 使っていない古い接続を閉じて上限に収める(いま使う相手は残す)。
    private fun trimLinks(keep: String) {
        while (links.size > MAX_LINKS) {
            val victim = links.entries.filter { it.key != keep }.minByOrNull { it.value.usedAt } ?: return
            log("close idle link: " + victim.key)
            dropLink(victim.key)
        }
    }

    // 目的の端末へ繋がった状態にする。成功でその接続を返す。
    //  他の端末の接続には触らない。ここで落とすと、居ない端末を1回叩いただけで
    //  撮影中の端末との接続まで切れる(それが 30 秒ごとの再接続の正体だった)。
    private fun ensureLink(target: String): Link? {
        links[target]?.let { if (it.ready()) { it.usedAt = System.currentTimeMillis(); return it } }
        dropLink(target)                                   // 死んでいる接続は捨ててから張り直す

        val ctx = appCtx ?: return null
        val dev = findDevice(target, 6000) ?: run { log("not found: " + target); return null }

        val lk = Link(target)
        lk.latchConn = CountDownLatch(1)
        lk.gatt = dev.connectGatt(ctx, false, lk.cb)
        if (lk.latchConn?.await(10, TimeUnit.SECONDS) != true || !lk.connected) {
            addrCache.remove(target)   // 覚えていたアドレスが古い(繋がらない) → 次は探し直す
            lk.close(); return null
        }

        lk.latchMtu = CountDownLatch(1)
        try { lk.gatt?.requestMtu(517) } catch (_: Exception) {}
        lk.latchMtu?.await(5, TimeUnit.SECONDS)

        lk.latchSvc = CountDownLatch(1)
        try { lk.gatt?.discoverServices() } catch (_: Exception) {}
        if (lk.latchSvc?.await(10, TimeUnit.SECONDS) != true) { lk.close(); return null }

        val svc = lk.gatt?.getService(SVC) ?: run { log("no ETP service"); lk.close(); return null }
        lk.rx = svc.getCharacteristic(RXC)
        lk.tx = svc.getCharacteristic(TXC)
        if (lk.rx == null || lk.tx == null) { log("no ETP characteristics"); lk.close(); return null }

        // 通知を有効にする(CCCD への書き込みまで済ませないと notify は来ない)
        lk.gatt?.setCharacteristicNotification(lk.tx, true)
        val d = lk.tx?.getDescriptor(CCCD)
        if (d != null) {
            lk.latchDesc = CountDownLatch(1)
            if (Build.VERSION.SDK_INT >= 33) {
                lk.gatt?.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                run { d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; lk.gatt?.writeDescriptor(d) }
            }
            lk.latchDesc?.await(5, TimeUnit.SECONDS)
        }
        lk.usedAt = System.currentTimeMillis()
        links[target] = lk
        trimLinks(target)
        log("linked to " + target + " (mtu=" + lk.mtu + ")")
        return lk
    }

    private fun writeChunk(lk: Link, part: ByteArray): Boolean {
        val c = lk.rx ?: return false
        val g = lk.gatt ?: return false
        lk.latchWrite = CountDownLatch(1)
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
        return lk.latchWrite?.await(5, TimeUnit.SECONDS) == true
    }

    // ETP を 1 往復する。ネイティブから同期で呼ばれる。失敗は null。
    fun exchange(target: String, frame: ByteArray, timeoutMs: Int): ByteArray? {
        lock.lock()
        try {
            log("exchange -> " + target + " " + frame.size + "B")
            val lk = ensureLink(target) ?: run { log("no link: " + target); return null }
            synchronized(lk.rxBuf) { lk.rxBuf.reset() }
            lk.latchReply = CountDownLatch(1)

            val chunk = (lk.mtu - 3).coerceAtLeast(20)
            var off = 0
            while (off < frame.size) {
                val n = minOf(chunk, frame.size - off)
                if (!writeChunk(lk, frame.copyOfRange(off, off + n))) {
                    log("write failed at " + off); dropLink(target); return null
                }
                off += n
            }

            val deadline = System.currentTimeMillis() + timeoutMs
            while (System.currentTimeMillis() < deadline) {
                lk.latchReply?.await(200, TimeUnit.MILLISECONDS)
                val cur = synchronized(lk.rxBuf) { lk.rxBuf.toByteArray() }
                val fl = frameLen(cur)
                if (fl > 0) { log("reply " + fl + "B from " + target); return cur.copyOfRange(0, fl) }
                if (fl < 0) { synchronized(lk.rxBuf) { lk.rxBuf.reset() } }   // 同期外れ → 捨てて待ち直す
                if (!lk.connected) return null
            }
            // 何バイト届いたところで詰まったのかを残す(0=応答が全く来ていない / 途中=分割の取りこぼし)。
            val got = synchronized(lk.rxBuf) { lk.rxBuf.size() }
            log("reply timeout (" + target + ") got=" + got + "B")
            return null
        } catch (e: Exception) {
            log("exchange failed: " + e.message)
            dropLink(target)
            return null
        } finally {
            lock.unlock()
        }
    }

    // BLE モードでのエッジ探索。Wi-Fi のブロードキャスト検索の代わり。
    //  見つかった端末名(HGC- を除いたもの)を返す。
    //
    // 【繋がっている端末はスキャンに映らない】接続が張られるとエッジは広告を止めるので、
    //  スキャンだけを答えにすると「いま話せている相手が見つからない」ことになる
    //  (2026-08-14: 撮影開始が「エッジ端末が見つかりません」で弾かれた)。
    //  そこで生きている接続の相手を必ず先頭に含める。
    fun scanEdgeNames(timeoutMs: Long): List<String> {
        // ここで lock は取らない。exchange() は 1 往復ぶん(最大 15 秒)握るので、
        //  待つと探索が固まる。links は ConcurrentHashMap なので走査だけなら安全。
        val linked = links.filterValues { it.ready() }.keys.toList()
        val ctx = appCtx ?: return linked
        val mgr = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager ?: return linked
        val ad = mgr.adapter ?: return linked
        if (!ad.isEnabled) return linked
        val scanner = ad.bluetoothLeScanner ?: return linked
        val names = LinkedHashSet<String>(linked)
        val sc = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                val n = try { r.scanRecord?.deviceName } catch (_: Exception) { null }
                    ?: try { r.device?.name } catch (_: SecurityException) { null }
                if (n != null && n.startsWith("HGC-")) {
                    val short = n.removePrefix("HGC-")
                    names.add(short)
                    addrCache[short] = r.device.address     // 見えた端末は次回スキャンを省ける
                    missUntil.remove(short)                 // 居るのが分かった → 待ちを解除
                }
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

    // 通信路を切り替えるときなどに掴んでいる接続をすべて捨てる。
    fun close() {
        lock.lock()
        try {
            for (lk in links.values) lk.close()
            links.clear()
        } finally { lock.unlock() }
    }
}
