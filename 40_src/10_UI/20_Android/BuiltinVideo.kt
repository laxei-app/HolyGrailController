package app.laxei.holygrail

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.File

// 撮ったコマをそのまま動画にする(2026-09-05)。
//
// 【なぜ端末で作るか】このアプリのスマホ利用者は、1枚ずつの画像を自分で動画に仕上げる人では
//  ない。撮ったら動画が出てくるところまでが成果物なので、端末上で作りきる。
//
// 【1コマずつ渡す】撮影周期は 15〜30 秒以上あるので、コマが撮れるたびに1枚ぶんだけ
//  符号化すれば足りる。まとめて処理する必要が無く、熱にも電池にも優しい。
//
// 【まだ「区切り」を持たない】MP4 は最後にきちんと閉じないと再生できない。いまは撮影の
//  終わりに1回閉じるだけなので、電池切れやアプリの異常終了ではその夜の動画が失われる。
//  1枚ずつの JPEG が残っているので作り直せる、というのが今の安全網である。
//  区切って書き出して後でつなぐ手当ては次の課題(設計メモ参照)。
object BuiltinVideo {

    private var codec: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var track = -1
    private var started = false
    private var frames = 0
    private var w = 0
    private var h = 0
    private var fps = 30
    private var path = ""

    // 作業用。毎コマ確保し直すと 1920x1440 で 11MB を掴んでは捨てることになる。
    private var argb: IntArray? = null
    private var scaled: Bitmap? = null

    private val info = MediaCodec.BufferInfo()

    // 【出来上がりの大きさ(2026-09-05)】撮影は 4080x3072(4:3)。そのままの画素数だと
    //  符号化器の上限に当たる端末があるので落とす。画角は切らない(空を捨てない)。
    //  16:9 で欲しい、といった要望が出たらここを変える。
    const val kWidth  = 1920
    const val kHeight = 1440

    @JvmStatic
    fun isOpen(): Boolean = codec != null

    // 書き出しを始める。"" =成功、それ以外は理由。
    @JvmStatic
    fun start(outPath: String, fpsWanted: Int): String {
        if (codec != null) { return "" }   // 既に開いている
        w = kWidth; h = kHeight
        fps = if (fpsWanted > 0) fpsWanted else 30
        path = outPath
        frames = 0; track = -1; started = false
        try {
            File(outPath).parentFile?.mkdirs()
            val fmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h)
            fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                           MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            // タイムラプスは動きが大きいので、通常の動画より多めに割り当てる。
            fmt.setInteger(MediaFormat.KEY_BIT_RATE, 16_000_000)
            fmt.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            // 1秒ごとにキーフレーム。途中から見ても崩れず、後で切り貼りしやすい。
            fmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            val c = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            c.start()
            codec = c
            muxer = MediaMuxer(outPath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        } catch (e: Exception) {
            release()
            return "video start failed: ${e.message}"
        }
        return ""
    }

    // 1コマ足す。JPEG のバイト列をそのまま渡す。
    @JvmStatic
    fun addJpeg(jpeg: ByteArray?): Boolean {
        val c = codec ?: return false
        if (jpeg == null || jpeg.isEmpty()) { return false }
        val bmp = decodeScaled(jpeg) ?: return false
        return try {
            val idx = c.dequeueInputBuffer(2_000_000)
            if (idx < 0) { return false }
            val img = c.getInputImage(idx) ?: run { c.queueInputBuffer(idx, 0, 0, 0, 0); return false }
            fillYuv(bmp, img)
            val ptsUs = frames.toLong() * 1_000_000L / fps
            c.queueInputBuffer(idx, 0, img.planes[0].buffer.capacity() * 3 / 2, ptsUs, 0)
            ++frames
            drain(false)
            true
        } catch (e: Exception) { false }
    }

    // 書き出しを閉じる。出来上がったファイルの場所を返す(失敗は "")。
    //  **閉じないと再生できない**ので、撮影の終わりに必ず呼ぶこと。
    @JvmStatic
    fun finish(): String {
        val c = codec ?: return ""
        val out = if (frames > 0) path else ""
        try {
            c.signalEndOfInputStream()
        } catch (_: Exception) {
            // ByteBuffer 入力では使えない端末がある。空のバッファに終端の印を付けて代える。
            try {
                val idx = c.dequeueInputBuffer(1_000_000)
                if (idx >= 0) { c.queueInputBuffer(idx, 0, 0,
                    frames.toLong() * 1_000_000L / fps, MediaCodec.BUFFER_FLAG_END_OF_STREAM) }
            } catch (_: Exception) {}
        }
        try { drain(true) } catch (_: Exception) {}
        release()
        return out
    }

    // ── 中身 ────────────────────────────────────────────────

    private fun release() {
        try { codec?.stop() } catch (_: Exception) {}
        try { codec?.release() } catch (_: Exception) {}
        if (started) { try { muxer?.stop() } catch (_: Exception) {} }
        try { muxer?.release() } catch (_: Exception) {}
        codec = null; muxer = null; track = -1; started = false
        argb = null
        try { scaled?.recycle() } catch (_: Exception) {}
        scaled = null
    }

    // 符号化済みのデータを取り出して MP4 へ流す。
    private fun drain(end: Boolean) {
        val c = codec ?: return
        val m = muxer ?: return
        while (true) {
            val idx = c.dequeueOutputBuffer(info, if (end) 200_000 else 0)
            if (idx == MediaCodec.INFO_TRY_AGAIN_LATER) { if (!end) return else return }
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

    // JPEG を出来上がりの大きさへ落として読む。
    //  一度に原寸で読むと 4080x3072 で 50MB になるので、まず粗く読んでから合わせる。
    private fun decodeScaled(jpeg: ByteArray): Bitmap? {
        val o = BitmapFactory.Options()
        o.inJustDecodeBounds = true
        BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, o)
        var sample = 1
        while (o.outWidth / (sample * 2) >= w) { sample *= 2 }
        val d = BitmapFactory.Options()
        d.inSampleSize = sample
        d.inPreferredConfig = Bitmap.Config.ARGB_8888
        val src = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size, d) ?: return null
        if (src.width == w && src.height == h) { return src }
        var s = scaled
        if (s == null || s.width != w || s.height != h) {
            try { s?.recycle() } catch (_: Exception) {}
            s = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
            scaled = s
        }
        val cv = android.graphics.Canvas(s)
        cv.drawBitmap(src, android.graphics.Rect(0, 0, src.width, src.height),
                      android.graphics.Rect(0, 0, w, h), null)
        src.recycle()
        return s
    }

    // ARGB を符号化器の受け口(YUV420)へ詰める。面ごとの並びは端末で違うので、
    //  rowStride / pixelStride を必ず見る(決め打ちすると色がずれる端末がある)。
    private fun fillYuv(bmp: Bitmap, img: android.media.Image) {
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
