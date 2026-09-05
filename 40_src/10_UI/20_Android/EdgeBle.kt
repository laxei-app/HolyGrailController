package app.laxei.holygrail

// エッジ端末 設定プロビジョニングの BLE セントラル(仕様 8.2.2)。
//  HGC-Edge(サービスUUID)をスキャン→接続→MTU拡張→STAT通知有効化→
//  {name,ssid,pass} を PoP由来鍵(SHA256)で AES-256-GCM 暗号化([IV12|CT|TAG16])し CRED へ write→
//  STAT 通知で ok/fail を受領。エッジ(edgeProv.cpp)と対になる。

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
import android.bluetooth.le.BluetoothLeScanner
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * BLE の検索回数の残り(2026-08-27)。
 *
 * 【なぜ数えるか】Android はアプリごとに BLE の検索回数を絞っており、**30秒に5回**を
 *  超えると検索そのものを始めてくれない(記録に
 *  `BtScan.ScanController: ... is scanning too frequently` が出る)。
 *  こちらには「見つからない」としか見えないので、原因が分からないまま何度も押して
 *  さらに深みにはまる。押す前に数えて、待つべき秒数を伝える。
 *
 * 数えるのはアプリ全体ぶん。エッジとの常時BLE通信(EdgeBleLink)も同じ枠を使うので、
 * そちらの検索も同じところへ記録する。片方だけ数えても実態と合わない。
 */
object BleScanBudget {
    private const val WINDOW_MS = 30_000L
    private const val MAX_IN_WINDOW = 5
    private val starts = ArrayDeque<Long>()

    /** 検索を始めたことを記録する。 */
    @Synchronized fun record() {
        val now = System.currentTimeMillis()
        while (starts.isNotEmpty() && now - starts.first() > WINDOW_MS) starts.removeFirst()
        starts.addLast(now)
    }

    /** 今始めたら弾かれるなら、待つべきミリ秒。0 なら始めてよい。 */
    @Synchronized fun waitMs(): Long {
        val now = System.currentTimeMillis()
        while (starts.isNotEmpty() && now - starts.first() > WINDOW_MS) starts.removeFirst()
        if (starts.size < MAX_IN_WINDOW) return 0
        return (WINDOW_MS - (now - starts.first())).coerceAtLeast(0)
    }

    /** 数えを白紙に戻す(単体試験のため。本番から呼ぶところは無い)。 */
    @Synchronized fun reset() { starts.clear() }

    /** 「あと◯秒」の文言。 */
    fun waitText(ms: Long): String = "あと %d 秒ほど".format((ms + 999) / 1000)
}

@SuppressLint("MissingPermission")  // 呼び出し側(MainActivity)で BLUETOOTH_SCAN/CONNECT を確認してから使う
class EdgeBle(
    private val ctx: Context,
    private val log: (String) -> Unit,
    private val result: (Boolean, String) -> Unit
) {
    companion object {
        val SVC  = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000001")
        // Android が返す「検索が多すぎる」。API30 で足された定数だが値は固定なので直に書く
        //  (ScanCallback.SCAN_FAILED_SCANNING_TOO_FREQUENTLY)。
        const val SCAN_TOO_FREQUENT = 6
        // 端末名が入っていないエッジが広告する名前(端末側の既定 g_devName = "NoName")。
        //  ファームを土台ごと書き直すとこの姿に戻る。
        const val UNSET_ADV_NAME = "HGC-NoName"
        val CTRL = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000002")  // write "start": エッジにQR(PoP)を表示させる
        val CRED = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000003")
        val STAT = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000004")
        val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        // 直近の startQr で "start" を送ったエッジのBLEアドレス。エッジが複数(どれも HGC-Edge で
        // 広告名が同一)でも、送信(provision)を「QRを表示させたのと同じエッジ」へ確実に向けるため。
        @Volatile var lastAddress: String? = null
    }

    private val handler = Handler(Looper.getMainLooper())
    private var scanner: BluetoothLeScanner? = null
    private var scanCb: ScanCallback? = null
    private var gatt: BluetoothGatt? = null
    private var payload: ByteArray = ByteArray(0)
    private var startOnly = false   // true=CTRL に "start" を書くだけ(QR表示要求) / false=CRED送信
    private var done = false

    // 接続したいエッジの端末名(2026-08-08 UI依頼)。空なら「最初に見つけた1台」= 従来動作。
    //
    // 【なぜ要るか】従来は全エッジが "HGC-Edge" を広告し、スマホは最初に応答した1台へ
    //  無条件で接続していた。エッジを複数台起動していると、どれに設定が飛ぶか分からず、
    //  登録済み端末の設定を更新できなかった。エッジ側は名前が決まっていれば
    //  "HGC-<端末名>" を広告するようにしたので、こちらは名前一致で選ぶ。
    //  出荷時(名前未設定)のエッジは "HGC-Edge" のままなので、新規登録では空を渡す。
    private var wantName: String = ""
    fun setTargetName(n: String) { wantName = n.trim() }

    // スキャンで見えた広告名(null=名前を広告していない)。
    private fun advName(r: ScanResult): String? =
        try { r.scanRecord?.deviceName } catch (_: Exception) { null }
            ?: try { r.device?.name } catch (_: SecurityException) { null }

    // 広告名が目的の端末か。wantName が空なら誰でも可(新規登録)。
    private fun matches(r: ScanResult): Boolean {
        if (wantName.isEmpty()) return true
        return advName(r) == "HGC-" + wantName
    }

    // 名前を広告しないエッジ(旧ファーム)を1台だけ覚えておく。名前一致が1件も無いまま
    // タイムアウトしたとき、これがあれば従来動作(最初の1台へ接続)へ落とす(2026-08-08)。
    //  ・名前つきが見えているのに一致しない → 本当に別の端末なので落とさない
    //  ・名前が一切見えない → 旧ファームなので落とす
    // こうしないと、エッジを更新するまで設定を送れず詰む。
    private var unnamedCandidate: BluetoothDevice? = null

    // 名前が未設定のエッジ(端末側は "NoName" のまま広告する)。
    //
    // 【なぜ拾うか(2026-08-27)】ファームを土台ごと書き直すと端末名が消える。すると一覧で
    //  選んだ名前では二度と見つからず、「更新したら設定画面から触れなくなった」という
    //  行き止まりになる(実機で踏んだ)。焼き直した直後はまさにこの姿なので、名前一致が
    //  無かったときの受け皿にする。
    //  ただし**1台だけのときに限る**。未設定の端末が複数居ると取り違えて、別の機体へ
    //  設定を送ってしまう。そのときは繋がずに、何が起きているかを伝えて止める。
    private val unprovisioned = LinkedHashMap<String, BluetoothDevice>()

    // エッジに "start" を送って QR(PoP)を LCD に表示させる(スキャン前に呼ぶ)。
    fun startQr() {
        done = false; startOnly = true
        beginScanConnect()
    }

    fun provision(pop: String, plainJson: String) {
        done = false; startOnly = false
        payload = encrypt(pop, plainJson)
        beginScanConnect()
    }

    private fun beginScanConnect() {
        val mgr = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val adapter = mgr?.adapter
        if (adapter == null || !adapter.isEnabled) { finish(false, "Bluetoothが無効です"); return }
        // 送信(provision)は、直前に QR を表示させたのと同じエッジへ直接接続する(複数エッジでも取り違えない)。
        if (!startOnly && lastAddress != null) {
            try {
                log("端末へ直接接続中...")
                connect(adapter.getRemoteDevice(lastAddress))
                return
            } catch (_: Exception) { /* だめならスキャンにフォールバック */ }
        }
        // 続けて押されると Android が検索を止めてしまう。始める前に見て、待つよう伝える。
        val wait = BleScanBudget.waitMs()
        if (wait > 0) {
            finish(false, "BLEの検索を続けて行ったため、Android が検索を受け付けません。" +
                          "${BleScanBudget.waitText(wait)}あけてから、もう一度お試しください")
            return
        }
        unnamedCandidate = null
        unprovisioned.clear()
        scanner = adapter.bluetoothLeScanner
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        log(if (wantName.isEmpty()) "BLEスキャン中 (未設定の端末)..." else "BLEスキャン中 (HGC-$wantName)...")
        scanCb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                // 名前が一致するものだけ拾う(2026-08-08 UI依頼)。一致しなければスキャンを続ける。
                if (!matches(r)) {
                    val n = advName(r)
                    if (n == null && unnamedCandidate == null) { unnamedCandidate = r.device }
                    if (n == UNSET_ADV_NAME) { unprovisioned[r.device.address] = r.device }
                    return
                }
                stopScan()
                log("発見 ${advName(r) ?: r.device.address}。接続中...")
                connect(r.device)
            }
            override fun onScanFailed(errorCode: Int) {
                finish(false, when (errorCode) {
                    SCAN_TOO_FREQUENT ->
                        "BLEの検索が続きすぎたため、Android が検索を止めました。" +
                        "30秒ほどあけてから、もう一度お試しください"
                    1 -> "すでに検索中です。少し待ってからお試しください"
                    2 -> "BLEの検索を始められません。Bluetooth を入れ直してみてください"
                    else -> "スキャン失敗 code=$errorCode"
                })
            }
        }
        BleScanBudget.record()
        scanner?.startScan(listOf(filter), settings, scanCb)
        handler.postDelayed({
            if (!done && gatt == null) {
                stopScan()
                val fallback = unnamedCandidate
                val unset = unprovisioned.values.toList()
                when {
                    fallback != null -> {
                        // 名前を広告しないエッジしか居ない = 旧ファーム。従来動作で接続する。
                        log("名前を広告しない端末へ接続します(端末のファームが古い可能性)")
                        connect(fallback)
                    }
                    // 焼き直した直後は名前が消えている。1台だけならそれが目当ての機体。
                    wantName.isNotEmpty() && unset.size == 1 -> {
                        log("「$wantName」は見つかりませんが、名前が未設定の端末が1台あります。" +
                            "ファームを書き直した直後はこうなります。そちらへ接続します")
                        connect(unset[0])
                    }
                    wantName.isNotEmpty() && unset.size > 1 ->
                        finish(false, "「$wantName」が見つかりません。名前が未設定の端末が${unset.size}台あるため、" +
                                      "取り違えを避けて中止しました。1台だけ電源を入れてやり直してください")
                    else ->
                        finish(false, if (wantName.isEmpty()) "端末が見つかりません(広告なし)"
                                      else "外部端末「$wantName」が見つかりません(電源とBluetoothを確認してください)")
                }
            }
        }, 12000)
    }

    private fun stopScan() { try { scanCb?.let { scanner?.stopScan(it) } } catch (_: Exception) {}; scanCb = null }

    private fun connect(dev: BluetoothDevice) {
        lastAddress = dev.address   // このエッジを覚えておき、送信時に同じ端末へ向ける
        gatt = dev.connectGatt(ctx, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED) { log("接続。MTU要求..."); g.requestMtu(247) }
                else if (newState == BluetoothProfile.STATE_DISCONNECTED) { if (!done) finish(false, "切断されました") }
            }
            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) { log("MTU=$mtu。サービス探索..."); g.discoverServices() }
            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                val svc = g.getService(SVC) ?: run { finish(false, "サービスが見つかりません"); return }
                if (startOnly) { writeCtrlStart(g); return }   // QR表示要求は STAT 通知不要で CTRL に write
                val stat = svc.getCharacteristic(STAT)
                if (stat != null) {
                    g.setCharacteristicNotification(stat, true)
                    val d = stat.getDescriptor(CCCD)
                    if (d != null) writeDescriptor(g, d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    else writeCred(g)
                } else writeCred(g)
            }
            override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) { writeCred(g) }
            override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
                if (c.uuid == CTRL) {
                    if (status == BluetoothGatt.GATT_SUCCESS) finish(true, "端末にQR表示を要求しました")
                    else finish(false, "start書込失敗 status=$status")
                } else if (c.uuid == CRED) {
                    if (status == BluetoothGatt.GATT_SUCCESS) log("認証情報を送信。応答待ち...")
                    else finish(false, "書込失敗 status=$status")
                }
            }
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
                if (c.uuid == STAT) handleStat(String(value))
            }
            @Deprecated("Deprecated in API 33")
            override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
                if (c.uuid == STAT) handleStat(String(@Suppress("DEPRECATION") (c.value ?: ByteArray(0))))
            }
        })
    }

    private fun handleStat(s: String) {
        log("端末の応答: $s")
        when {
            s.startsWith("ok")   -> finish(true, "設定を保存しました(端末がWiFi再接続)")
            s.startsWith("fail") -> finish(false, "端末側で復号失敗(PoP不一致)")
        }
    }

    private fun writeCtrlStart(g: BluetoothGatt) {
        val ctrl = g.getService(SVC)?.getCharacteristic(CTRL) ?: run { finish(false, "CTRL特性無し"); return }
        val bytes = "start".toByteArray(Charsets.UTF_8)
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ctrl, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION") ctrl.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION") ctrl.value = bytes
            @Suppress("DEPRECATION") g.writeCharacteristic(ctrl)
        }
        // 応答特性は無いので、書込コールバックが来ない実装でも完了扱いにする保険。
        handler.postDelayed({ if (!done) finish(true, "QR表示を要求(応答待ちタイムアウト)") }, 5000)
    }

    private fun writeCred(g: BluetoothGatt) {
        val cred = g.getService(SVC)?.getCharacteristic(CRED) ?: run { finish(false, "CRED特性無し"); return }
        if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(cred, payload, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
        } else {
            @Suppress("DEPRECATION") cred.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            @Suppress("DEPRECATION") cred.value = payload
            @Suppress("DEPRECATION") g.writeCharacteristic(cred)
        }
        // 応答(STAT通知)が来ない実装でも完了扱いにする保険。
        handler.postDelayed({ if (!done) finish(true, "送信完了(端末の応答待ちタイムアウト)") }, 7000)
    }

    private fun writeDescriptor(g: BluetoothGatt, d: BluetoothGattDescriptor, v: ByteArray) {
        if (Build.VERSION.SDK_INT >= 33) g.writeDescriptor(d, v)
        else { @Suppress("DEPRECATION") d.value = v; @Suppress("DEPRECATION") g.writeDescriptor(d) }
    }

    private fun finish(ok: Boolean, msg: String) {
        if (done) return
        done = true
        handler.post {
            try { gatt?.disconnect(); gatt?.close() } catch (_: Exception) {}
            gatt = null
            result(ok, msg)
        }
    }

    // 鍵=SHA256(PoP)。出力=[IV(12) | 暗号文 | GCMタグ(16)](javax の GCM は doFinal で ct||tag を返す)。
    private fun encrypt(pop: String, plain: String): ByteArray {
        val key = MessageDigest.getInstance("SHA-256").digest(pop.toByteArray(Charsets.UTF_8))
        val iv = ByteArray(12).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(128, iv))
        val ctTag = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
        return iv + ctTag
    }
}
