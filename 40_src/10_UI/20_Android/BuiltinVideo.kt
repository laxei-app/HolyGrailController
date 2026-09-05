package app.laxei.holygrail

import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMuxer
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import java.io.File
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// 撮ったコマをそのまま動画にする(2026-09-05)。
//
// 【なぜ端末で作るか】このアプリのスマホ利用者は、1枚ずつの画像を自分で動画に仕上げる人では
//  ない。撮ったら動画が出てくるところまでが成果物なので、端末上で作りきる。
//
// 【徐々に出来上がる(2026-09-05 ユーザー要望)】MP4 は最後に閉じないと再生できない。撮影の
//  終わりに1回だけ閉じる作りだと、電池切れや異常終了でその夜の動画が丸ごと失われる。
//  そこで kSegmentMs ごとに区切り、そのたびに「そこまでの完成品」を作り直して利用者から
//  見える場所(ギャラリー)へ出す。
//
//   ① 10分ぶんの切れ端(seg)を閉じる
//   ② これまでの完成品(full) + 切れ端 → 新しい完成品   ※再符号化しない。中身を並べ替えるだけ
//   ③ 古い完成品と置き換える(新しい方を別名で作ってから差し替える。途中で落ちても古い方は無事)
//   ④ 完成品をギャラリーへ出す
//   ⑤ 切れ端を消して次の切れ端を書き始める
//
//  手元にあるのは常に「そこまでの完成品1本」と「いま書いている切れ端」の2つだけ。
//  最後に全体をつなぐ工程は無い。撮影の終わりも「最後の切れ端を足す」だけである。
//  落ちて失うのは書きかけの切れ端＝最大 kSegmentMs ぶん。
//
// 【1コマずつ渡す】撮影周期は 15〜30 秒以上あるので、コマが撮れるたびに1枚ぶんだけ
//  符号化すれば足りる。②〜④も一晩の終わり(約60MB)で数秒なので、周期のすき間に収まる。
object BuiltinVideo {

    private var appCtx: Context? = null
    fun init(ctx: Context) { appCtx = ctx.applicationContext }

    // 区切りの間隔。失うのは最後の区切りからの時間なので、コマ数ではなく時間で決める。
    //  **調整するときはここだけを変える**(2026-09-05 ユーザー判断: 10分)。
    const val kSegmentMs = 10L * 60L * 1000L

    // 【出来上がりの大きさ】撮影は 4080x3072(4:3)。そのままの画素数だと符号化器の上限に
    //  当たる端末があるので落とす。画角は切らない(空を捨てない)。
    const val kWidth  = 1920
    const val kHeight = 1440

    // ── 切れ端の符号化 ──────────────────────────────────────
    private var codec: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var track = -1
    private var started = false
    private var segFrames = 0           // いまの切れ端のコマ数
    private var segStartMs = 0L         // いまの切れ端を書き始めた時刻
    private var fps = 30
    private val info = MediaCodec.BufferInfo()

    // ── 完成品 ──────────────────────────────────────────────
    private var workDir: File? = null   // アプリの領域(切れ端と完成品の置き場)
    private var segFile: File? = null
    private var fullFile: File? = null
    private var fullFrames = 0          // 完成品に入っているコマ数(つなぐときの時刻の起点)
    private var displayName = ""        // ギャラリーでの名前 <計画名>_yyyymmddhhmmss.mp4
    private var planName = ""

    // 動画の名前に使う計画名。撮影を始める側が先に渡す(空なら "hgt")。
    @JvmStatic
    fun setPlanName(name: String) { planName = name }

    // ファイル名に使えない文字を落とす。計画名は自由に付けられるので、区切りや記号が混ざる。
    private fun safeName(s: String): String =
        s.replace(Regex("[\\\\/:*?\"<>|\\u0000-\\u001f]"), "_").trim().ifEmpty { "hgt" }
    private var publishedUri: Uri? = null   // ギャラリーに出してある完成品(API 29+)

    // 作業用。毎コマ確保し直すと 1920x1440 で 11MB を掴んでは捨てることになる。
    private var argb: IntArray? = null
    private var scaled: Bitmap? = null

    @JvmStatic
    fun isOpen(): Boolean = codec != null

    // 書き出しを始める。戻り=ギャラリーでの名前("" =失敗)。
    @JvmStatic
    fun start(fpsWanted: Int): String {
        if (codec != null) { return displayName }
        val ctx = appCtx ?: return ""
        fps = if (fpsWanted > 0) fpsWanted else 30
        // 名前は <計画名>_yyyymmddhhmmss。同じ計画をもう一度撮っても日時で別のファイルになる。
        displayName = safeName(planName) + "_" +
                      SimpleDateFormat("yyyyMMddHHmmss", Locale.US).format(Date()) + ".mp4"
        val dir = File(ctx.getExternalFilesDir(null), "shot").apply { mkdirs() }
        workDir  = dir
        fullFile = File(dir, displayName)
        segFile  = File(dir, "seg_$displayName")
        fullFrames = 0; publishedUri = null
        runCatching { fullFile?.delete(); segFile?.delete() }
        return if (openSegment()) displayName else ""
    }

    // 1コマ足す。JPEG のバイト列をそのまま渡す。
    @JvmStatic
    fun addJpeg(jpeg: ByteArray?): Boolean {
        val c = codec ?: return false
        if (jpeg == null || jpeg.isEmpty()) { return false }
        val bmp = decodeScaled(jpeg) ?: return false
        val ok = try {
            val idx = c.dequeueInputBuffer(2_000_000)
            if (idx < 0) { return false }
            val img = c.getInputImage(idx) ?: run { c.queueInputBuffer(idx, 0, 0, 0, 0); return false }
            fillYuv(bmp, img)
            val ptsUs = segFrames.toLong() * 1_000_000L / fps
            c.queueInputBuffer(idx, 0, img.planes[0].buffer.capacity() * 3 / 2, ptsUs, 0)
            ++segFrames
            drain(false)
            true
        } catch (e: Exception) { false }
        // 区切りの時刻を過ぎていたら、ここで完成品を作り直す(周期のすき間で済む)。
        if (ok && System.currentTimeMillis() - segStartMs >= kSegmentMs) { rotate(reopen = true) }
        return ok
    }

    // 書き出しを閉じる。最後の切れ端を足して完成品にする。戻り=完成品の場所("" =失敗)。
    @JvmStatic
    fun finish(): String {
        if (codec == null) { return "" }
        rotate(reopen = false)	// 最後の切れ端を足す。次の切れ端は開かない
        val out = fullFile?.takeIf { it.exists() && fullFrames > 0 }?.absolutePath ?: ""
        argb = null
        try { scaled?.recycle() } catch (_: Exception) {}
        scaled = null
        return out
    }

    // ── 区切り ──────────────────────────────────────────────
    // いまの切れ端を閉じ、完成品へ足し、ギャラリーへ出し、(reopen なら)次の切れ端を開く。
    private fun rotate(reopen: Boolean) {
        val seg = segFile ?: return
        val full = fullFile ?: return
        val frames = segFrames
        closeSegment()
        if (frames > 0 && seg.exists()) {
            // 新しい完成品は別名で作り、できてから差し替える(途中で落ちても古い方は無事)。
            val next = File(full.parentFile, full.name + ".next")
            val merged = if (full.exists()) remux(listOf(full, seg), next) else seg.renameTo(next)
            if (merged && next.exists()) {
                runCatching { full.delete() }
                if (next.renameTo(full)) {
                    fullFrames += frames
                    runCatching { seg.delete() }
                    publish(full)
                }
            } else {
                android.util.Log.w("HGC", "video: merge failed, keeping segment " + seg.name)
            }
        }
        if (reopen) { openSegment() }
    }

    private fun openSegment(): Boolean {
        val seg = segFile ?: return false
        try {
            runCatching { seg.delete() }
            val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, kWidth, kHeight)
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                           MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, 16_000_000)   // タイムラプスは動きが大きい
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)    // 1秒ごとにキーフレーム
            val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            c.start()
            codec = c
            muxer = MediaMuxer(seg.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            track = -1; started = false; segFrames = 0
            segStartMs = System.currentTimeMillis()
            return true
        } catch (e: Exception) {
            closeSegment()
            return false
        }
    }

    private fun closeSegment() {
        val c = codec
        if (c != null) {
            try {
                val idx = c.dequeueInputBuffer(1_000_000)
                if (idx >= 0) { c.queueInputBuffer(idx, 0, 0,
                    segFrames.toLong() * 1_000_000L / fps, MediaCodec.BUFFER_FLAG_END_OF_STREAM) }
                drain(true)
            } catch (_: Exception) {}
        }
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        if (started) { try { muxer?.stop() } catch (_: Exception) {} }
        try { muxer?.release() } catch (_: Exception) {}
        codec = null; muxer = null; track = -1; started = false
    }

    // 符号化済みのデータを取り出して切れ端へ流す。
    private fun drain(end: Boolean) {
        val c = codec ?: return
        val m = muxer ?: return
        while (true) {
            val idx = c.dequeueOutputBuffer(info, if (end) 200_000 else 0)
            if (idx == MediaCodec.INFO_TRY_AGAIN_LATER) { return }
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (!started) { track = m.addTrack(c.outputFormat); m.start(); started = true }
                continue
            }
            if (idx < 0) { continue }
            val buf = c.getOutputBuffer(idx)
            if (buf != null && info.size > 0 && started &&
                (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                buf.position(info.offset); buf.limit(info.offset + info.size)
                m.writeSampleData(track, buf, info)
            }
            c.releaseOutputBuffer(idx, false)
            if ((info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) { return }
        }
    }

    // ── つなぐ(再符号化しない) ──────────────────────────────
    // 複数の MP4 を順に読み、符号化済みのコマをそのまま1本へ書く。時刻は前のぶんだけずらす。
    //  同じ設定で符号化した切れ端どうしなので、映像の形式は先頭のものをそのまま使える。
    private fun remux(inputs: List<File>, out: File): Boolean {
        var mux: MediaMuxer? = null
        var outTrack = -1
        var offsetUs = 0L
        val buf = ByteBuffer.allocateDirect(4 * 1024 * 1024)
        val bi = MediaCodec.BufferInfo()
        try {
            runCatching { out.delete() }
            mux = MediaMuxer(out.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            for (f in inputs) {
                val ex = MediaExtractor()
                FileInputStream(f).use { fis -> ex.setDataSource(fis.fd) }
                var vt = -1
                for (i in 0 until ex.trackCount) {
                    val mime = ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME) ?: ""
                    if (mime.startsWith("video/")) { vt = i; break }
                }
                if (vt < 0) { ex.release(); continue }
                ex.selectTrack(vt)
                val fmt = ex.getTrackFormat(vt)
                if (outTrack < 0) { outTrack = mux.addTrack(fmt); mux.start() }
                var lastPts = 0L
                while (true) {
                    val n = ex.readSampleData(buf, 0)
                    if (n < 0) { break }
                    bi.offset = 0; bi.size = n
                    bi.presentationTimeUs = ex.sampleTime + offsetUs
                    bi.flags = if ((ex.sampleFlags and MediaExtractor.SAMPLE_FLAG_SYNC) != 0)
                        MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                    mux.writeSampleData(outTrack, buf, bi)
                    lastPts = bi.presentationTimeUs
                    ex.advance()
                }
                ex.release()
                // 次の入力はこのぶんの後ろへ。1コマぶん足すのは最後のコマの長さのため。
                offsetUs = lastPts + 1_000_000L / fps
            }
            mux.stop(); mux.release()
            return outTrack >= 0
        } catch (e: Exception) {
            try { mux?.release() } catch (_: Exception) {}
            runCatching { out.delete() }
            return false
        }
    }

    // ── ギャラリーへ出す ────────────────────────────────────
    // Movies/HolyGrail/hgt_yymmddhhmmss.mp4。区切りのたびに置き換える。
    //  新しい方を別の項目として書き切ってから古い方を消すので、ギャラリーに見えるのは
    //  常に完全なものだけ。
    private fun publish(full: File) {
        val ctx = appCtx ?: return
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                val cr = ctx.contentResolver
                val values = ContentValues().apply {
                    put(MediaStore.Video.Media.DISPLAY_NAME, "$displayName.part")
                    put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                    put(MediaStore.Video.Media.RELATIVE_PATH, "Movies/HolyGrail")
                    put(MediaStore.Video.Media.IS_PENDING, 1)
                }
                val uri = cr.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values) ?: return
                cr.openOutputStream(uri, "w")?.use { os -> full.inputStream().use { it.copyTo(os) } }
                    ?: run { cr.delete(uri, null, null); return }
                // 古い方を消してから、新しい方を本来の名前にして見えるようにする。
                publishedUri?.let { runCatching { cr.delete(it, null, null) } }
                val done = ContentValues().apply {
                    put(MediaStore.Video.Media.DISPLAY_NAME, displayName)
                    put(MediaStore.Video.Media.IS_PENDING, 0)
                }
                cr.update(uri, done, null, null)
                publishedUri = uri
            } else {
                val dir = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES),
                               "HolyGrail").apply { mkdirs() }
                val dst = File(dir, displayName)
                val tmp = File(dir, "$displayName.part")
                full.inputStream().use { i -> tmp.outputStream().use { o -> i.copyTo(o) } }
                tmp.renameTo(dst)
                MediaScannerConnection.scanFile(ctx, arrayOf(dst.absolutePath), arrayOf("video/mp4"), null)
            }
        } catch (e: Exception) {
            android.util.Log.w("HGC", "video: publish failed: " + e)
        }
    }

    // ── 中身 ────────────────────────────────────────────────

    // JPEG を出来上がりの大きさへ落として読む。
    //  一度に原寸で読むと 4080x3072 で 50MB になるので、まず粗く読んでから合わせる。
    private fun decodeScaled(jpeg: ByteArray): Bitmap? {
        val o = BitmapFactory.Options()
        o.inJustDecodeBounds = true
        BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, o)
        var sample = 1
        while (o.outWidth / (sample * 2) >= kWidth) { sample *= 2 }
        val d = BitmapFactory.Options()
        d.inSampleSize = sample
        d.inPreferredConfig = Bitmap.Config.ARGB_8888
        val src = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, d) ?: return null
        if (src.width == kWidth && src.height == kHeight) { return src }
        var s = scaled
        if (s == null || s.width != kWidth || s.height != kHeight) {
            try { s?.recycle() } catch (_: Exception) {}
            s = Bitmap.createBitmap(kWidth, kHeight, Bitmap.Config.ARGB_8888)
            scaled = s
        }
        val cv = android.graphics.Canvas(s)
        cv.drawBitmap(src, android.graphics.Rect(0, 0, src.width, src.height),
                      android.graphics.Rect(0, 0, kWidth, kHeight), null)
        src.recycle()
        return s
    }

    // ARGB を符号化器の受け口(YUV420)へ詰める。面ごとの並びは端末で違うので、
    //  rowStride / pixelStride を必ず見る(決め打ちすると色がずれる端末がある)。
    private fun fillYuv(bmp: Bitmap, img: android.media.Image) {
        val w = kWidth; val h = kHeight
        var px = argb
        if (px == null || px.size != w * h) { px = IntArray(w * h); argb = px }
        bmp.getPixels(px, 0, w, 0, 0, w, h)

        val yP = img.planes[0]; val uP = img.planes[1]; val vP = img.planes[2]
        val yB = yP.buffer; val uB = uP.buffer; val vB = vP.buffer
        val yRow = yP.rowStride; val yPix = yP.pixelStride
        val uRow = uP.rowStride; val uPix = uP.pixelStride
        val vRow = vP.rowStride; val vPix = vP.pixelStride

        for (j in 0 until h) {
            var i = 0
            val base = j * w
            while (i < w) {
                val c = px[base + i]
                val r = (c shr 16) and 0xFF; val g = (c shr 8) and 0xFF; val b = c and 0xFF
                val y = ((66 * r + 129 * g + 25 * b + 128) shr 8) + 16
                yB.put(j * yRow + i * yPix, y.coerceIn(0, 255).toByte())
                if ((j and 1) == 0 && (i and 1) == 0) {
                    val u = ((-38 * r - 74 * g + 112 * b + 128) shr 8) + 128
                    val v = ((112 * r - 94 * g - 18 * b + 128) shr 8) + 128
                    val cj = j / 2; val ci = i / 2
                    uB.put(cj * uRow + ci * uPix, u.coerceIn(0, 255).toByte())
                    vB.put(cj * vRow + ci * vPix, v.coerceIn(0, 255).toByte())
                }
                ++i
            }
        }
    }
}
