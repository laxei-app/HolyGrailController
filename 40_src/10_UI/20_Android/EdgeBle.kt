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
        val CRED = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000003")
        val STAT = UUID.fromString("a1b2c3d4-0001-4a5b-8c6d-000000000004")
        val CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }

    private val handler = Handler(Looper.getMainLooper())
    private var scanner: BluetoothLeScanner? = null
    private var scanCb: ScanCallback? = null
    private var gatt: BluetoothGatt? = null
    private var payload: ByteArray = ByteArray(0)
    private var done = false

    fun provision(pop: String, plainJson: String) {
        done = false
        payload = encrypt(pop, plainJson)
        val mgr = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        val adapter = mgr?.adapter
        if (adapter == null || !adapter.isEnabled) { finish(false, "Bluetoothが無効です"); return }
        scanner = adapter.bluetoothLeScanner
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SVC)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        log("BLEスキャン中 (HGC-Edge)...")
        scanCb = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, r: ScanResult) {
                stopScan()
                log("発見 ${r.device.address}。接続中...")
                connect(r.device)
            }
            override fun onScanFailed(errorCode: Int) { finish(false, "スキャン失敗 code=$errorCode") }
        }
        scanner?.startScan(listOf(filter), settings, scanCb)
        handler.postDelayed({ if (!done && gatt == null) { stopScan(); finish(false, "エッジが見つかりません(広告なし)") } }, 12000)
    }

    private fun stopScan() { try { scanCb?.let { scanner?.stopScan(it) } } catch (_: Exception) {}; scanCb = null }

    private fun connect(dev: BluetoothDevice) {
        gatt = dev.connectGatt(ctx, false, object : BluetoothGattCallback() {
            override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
                if (newState == BluetoothProfile.STATE_CONNECTED) { log("接続。MTU要求..."); g.requestMtu(247) }
                else if (newState == BluetoothProfile.STATE_DISCONNECTED) { if (!done) finish(false, "切断されました") }
            }
            override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) { log("MTU=$mtu。サービス探索..."); g.discoverServices() }
            override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
                val svc = g.getService(SVC) ?: run { finish(false, "サービスが見つかりません"); return }
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
                if (c.uuid == CRED) {
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
