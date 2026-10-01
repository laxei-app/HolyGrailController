package app.laxei.twylapse

// 撮ったコマを「利用者が見える場所」へ残す(2026-09-23 ユーザー指示)。
//
// 【なぜ MediaStore か】アプリ専用の領域(Android/data/…)は Android 11 以降、利用者から
//  見えない(ファイルアプリでも PC の USB 接続でも開けない)。ギャラリーに出すには
//  MediaStore へ登録するしかない。動画(BuiltinVideo)と同じ考え方。
//
// 【置き場所】Pictures/TwyLapse/<計画名>_<日時>/  … 1 回の撮影で 1 つのアルバムになる。
//  jpg は現像結果(2×2 束ね)、DNG は束ねる前のフルサイズ。名前は連番。
//
// 【DNG は fd を渡して C++ が書く】1 枚 25MB を Java の配列へ写すのは無駄なので、
//  MediaStore から貰った書き込み先(ファイル記述子)をそのまま C++ へ渡す。
import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File
import java.io.FileOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

object BuiltinStill {

    private var appCtx: Context? = null
    fun init(ctx: Context) { appCtx = ctx.applicationContext }

    private var album = ""          // Pictures の下の相対パス
    private var legacyDir: File? = null   // API 28 以下で直に書く場所
    private var seq = 0
    private var pendingUri: Uri? = null   // 書きかけの DNG(閉じるときに仕上げる)

    // 1 回の撮影の始まりにアルバム名を決める。planName は動画と同じ名前。
    @JvmStatic
    fun begin(planName: String) {
        val safe = planName.replace(Regex("[\\\\/:*?\"<>|\\u0000-\\u001f]"), "_").trim().ifEmpty { "tlp" }
        val stamp = SimpleDateFormat("yyyyMMddHHmmss", Locale.US).format(Date())
        album = "Pictures/TwyLapse/${safe}_$stamp"
        seq = 0
        legacyDir = if (Build.VERSION.SDK_INT < 29) {
            File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_PICTURES),
                 "TwyLapse/${safe}_$stamp").apply { mkdirs() }
        } else { null }
    }

    // 【保存先の空き[バイト](2026-10-01 ユーザ指示)】
    //  jpg/DNG(MediaStore 経由)も動画の作業領域(アプリの外部領域)も、同じボリュームの上にある。
    //  だから 1 か所測れば足りる。
    //
    //  **availableBytes を使う**(freeBytes ではなく)。Android は空きの一部を
    //  系統のために取ってあり、アプリが実際に書けるのは availableBytes の分だけ。
    //  freeBytes を信じると、まだあるつもりで書けなくなる。
    @JvmStatic
    fun freeBytes(): Long = try {
        val f = android.os.Environment.getExternalStorageDirectory()
        android.os.StatFs(f.absolutePath).availableBytes
    } catch (e: Exception) {
        -1L     // 測れない端末では見張らない(止める判断に使わない)
    }

    private fun nextName(ext: String): String =
        String.format(Locale.US, "tlp_%05d.%s", seq, ext)

    // 【撮った向きを jpg に入れる(2026-10-01 ユーザ指示)】
    //  RAW から現像した jpg は C++ が作っていて EXIF が無いので、ここで APP1 を先頭へ挿す。
    //  端末が JPEG で出す経路ではすでに EXIF が入っているので**触らない**
    //  (二重に入れると、読む側がどちらを見るかは決まっていない)。
    //  向きだけの最小の EXIF を自分で組む — 34 バイト。これのために書庫を
    //  足すと、ある程度大きな jpg を丸ごと読み書きし直すことになる(毎コマ 2〜8MB)。
    private fun withOrientation(src: ByteArray, ori: Int): ByteArray {
        if (ori <= 1 || src.size < 4) { return src }
        if ((src[0].toInt() and 0xFF) != 0xFF || (src[1].toInt() and 0xFF) != 0xD8) { return src }
        // すでに APP1(EXIF) があるなら何もしない。
        if (src.size > 4 && (src[2].toInt() and 0xFF) == 0xFF && (src[3].toInt() and 0xFF) == 0xE1) { return src }
        val app1 = byteArrayOf(
            0xFF.toByte(), 0xE1.toByte(), 0x00, 0x22,            // APP1, 長さ 34
            0x45, 0x78, 0x69, 0x66, 0x00, 0x00,                  // "Exif\u0000\u0000"
            0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,      // TIFF ヘッダ(リトルエンディアン)
            0x01, 0x00,                                          // IFD0 の項目数 = 1
            0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,      // タグ 0x0112 Orientation, SHORT, 1個
            ori.toByte(), 0x00, 0x00, 0x00,                      // 値
            0x00, 0x00, 0x00, 0x00)                              // 次の IFD は無い
        val out = ByteArray(2 + app1.size + (src.size - 2))
        out[0] = src[0]; out[1] = src[1]
        System.arraycopy(app1, 0, out, 2, app1.size)
        System.arraycopy(src, 2, out, 2 + app1.size, src.size - 2)
        return out
    }

    // jpg を 1 枚残す。戻り=残せたか。
    @JvmStatic
    fun saveJpeg(raw: ByteArray?): Boolean {
        val bytes = if (raw == null) null else withOrientation(raw, BuiltinCamera.exifOrientation())
        if (bytes == null || bytes.isEmpty()) { return false }
        val ctx = appCtx ?: return false
        if (album.isEmpty()) { begin("tlp") }
        val name = nextName("jpg")
        return try {
            if (Build.VERSION.SDK_INT >= 29) {
                val cr = ctx.contentResolver
                val v = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
                    put(MediaStore.Images.Media.RELATIVE_PATH, album)
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v) ?: return false
                cr.openOutputStream(uri)?.use { it.write(bytes) } ?: return false
                cr.update(uri, ContentValues().apply { put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
            } else {
                val dir = legacyDir ?: return false
                FileOutputStream(File(dir, name)).use { it.write(bytes) }
                scanLegacy(File(dir, name))
            }
            true
        } catch (e: Exception) {
            android.util.Log.w("TLP-STILL", "jpeg save failed: $e"); false
        }
    }

    // DNG の書き込み先を用意して、書き込み用のファイル記述子を返す(-1 =失敗)。
    //  **必ず closeDng を呼ぶこと**(呼ばないとギャラリーに出ない/半端なファイルが残る)。
    @JvmStatic
    fun openDng(): Int {
        val ctx = appCtx ?: return -1
        if (album.isEmpty()) { begin("tlp") }
        val name = nextName("dng")
        return try {
            if (Build.VERSION.SDK_INT >= 29) {
                val cr = ctx.contentResolver
                val v = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name)
                    put(MediaStore.Images.Media.MIME_TYPE, "image/x-adobe-dng")
                    put(MediaStore.Images.Media.RELATIVE_PATH, album)
                    put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, v) ?: return -1
                pendingUri = uri
                val pfd = cr.openFileDescriptor(uri, "w") ?: return -1
                pfd.detachFd()          // 以後は C++ が持つ。閉じるのも C++ 側(closeDng)
            } else {
                val dir = legacyDir ?: return -1
                val f = File(dir, name)
                legacyFile = f
                android.os.ParcelFileDescriptor.open(f,
                    android.os.ParcelFileDescriptor.MODE_CREATE or
                    android.os.ParcelFileDescriptor.MODE_WRITE_ONLY or
                    android.os.ParcelFileDescriptor.MODE_TRUNCATE).detachFd()
            }
        } catch (e: Exception) {
            android.util.Log.w("TLP-STILL", "dng open failed: $e"); -1
        }
    }
    private var legacyFile: File? = null

    // DNG を書き終えた。ok=書けたか(偽ならギャラリーへ出さずに消す)。
    //  ファイル記述子は書いた側(C++)が閉じてから呼ぶこと。
    @JvmStatic
    fun closeDng(ok: Boolean) {
        val ctx = appCtx
        val uri = pendingUri
        pendingUri = null
        try {
            if (Build.VERSION.SDK_INT >= 29 && ctx != null && uri != null) {
                if (ok) {
                    ctx.contentResolver.update(uri, ContentValues().apply {
                        put(MediaStore.Images.Media.IS_PENDING, 0) }, null, null)
                } else {
                    ctx.contentResolver.delete(uri, null, null)
                }
            } else {
                val f = legacyFile
                if (f != null) { if (ok) { scanLegacy(f) } else { runCatching { f.delete() } } }
            }
        } catch (e: Exception) {
            android.util.Log.w("TLP-STILL", "dng close failed: $e")
        }
        legacyFile = null
    }

    // 1 コマ終わり(jpg と DNG を同じ番号にするため、両方書き終えてから進める)。
    @JvmStatic
    fun nextFrame() { ++seq }

    private fun scanLegacy(f: File) {
        val ctx = appCtx ?: return
        runCatching {
            android.media.MediaScannerConnection.scanFile(ctx, arrayOf(f.absolutePath), null, null)
        }
    }
}
