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

@SuppressLint("MissingPermission")  // 呼び出し側(MainActivity)で BLUETOOTH_SCAN/CONNECT を確認してから使う
class EdgeBle(
    private val ctx: Context,
    private val log: (String) -> Unit,
    private val result: (Boolean, String) -> Unit
) {
    companion object {
        val SVC  = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000001")
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

    // 広告名が目的の端末か。wantName が空なら誰でも可(新規登録)。
    private fun matches(r: ScanResult): Boolean {
        if (wantName.isEmpty()) return true
        val adv = try { r.scanRecord?.deviceName } catch (_: Exception) { null }
        val dev = try { r.device?.name } catch (_: SecurityException) { null }
        val want = "HGC-" + wantName
        return adv == want || dev == want
    }

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
                log("エッジへ直接接続中...")
                connect(adapter.getRemoteDevice(lastAddress))
                return
            } catch (_: Exception) { /* だめならスキャンにフォールバック */ }
        }
        scanner = adapter.bluetoothLeScanner
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        log(if (wantName.isEmpty()) "BLEスキャン中 (未設定のエッジ)..." else "BLEスキャン中 (HGC-$wantName)...")
        scanCb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                // 名前が一致するものだけ拾う(2026-08-08 UI依頼)。一致しなければスキャンを続ける。
                if (!matches(r)) return
                stopScan()
                val nm = try { r.scanRecord?.deviceName ?: r.device?.name } catch (_: SecurityException) { null }
                log("発見 ${nm ?: r.device.address}。接続中...")
                connect(r.device)
            }
            override fun onScanFailed(errorCode: Int) { finish(false, "スキャン失敗 code=$errorCode") }
        }
        scanner?.startScan(listOf(filter), settings, scanCb)
        handler.postDelayed({
            if (!done && gatt == null) {
                stopScan()
                finish(false, if (wantName.isEmpty()) "エッジが見つかりません(広告なし)"
                              else "エッジ「$wantName」が見つかりません(電源とBluetoothを確認してください)")
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
                    if (status == BluetoothGatt.GATT_SUCCESS) finish(true, "エッジにQR表示を要求しました")
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
        log("エッジ応答: $s")
        when {
            s.startsWith("ok")   -> finish(true, "設定を保存しました(エッジがWiFi再接続)")
            s.startsWith("fail") -> finish(false, "エッジ側で復号失敗(PoP不一致)")
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
        handler.postDelayed({ if (!done) finish(true, "送信完了(エッジ応答待ちタイムアウト)") }, 7000)
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
