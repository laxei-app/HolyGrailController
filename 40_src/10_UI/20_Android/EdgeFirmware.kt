package app.laxei.holygrail

import android.content.Context
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.util.Base64

// エッジのファームを公開リポジトリから取ってきて、正しさを確かめる(2026-08-26)。
//
// 置き場は laxei-app/hgc-master の firmware/。公開なのでトークンは要らない
// (実測で確認済み)。目録(manifest.json)に版数・大きさ・SHA256 が入っている。
//
// 【必ず確かめてから焼くこと】途中で切れた中身をそのまま焼くと、端末は PC から焼き直す
//  まで動かなくなる。大きさと SHA256 の両方を見る。
//
// 【機種の選び方】どちらの機種も ESP32-S3 で、チップからは見分けられない。
//  **フラッシュ容量**で決める(8MB=StickS3 / 16MB=CoreS3)。容量はダウンロードモードで
//  1バイトも書かずに読めるので、焼く前に判別できる。

data class EdgeFirmwareEntry(
    val id: String,
    val name: String,
    val chip: String,
    val flashSize: String,      // "8MB" のような表記。機種の選別に使う
    val version: String,
    val build: String,
    val file: String,
    val offset: Int,
    val size: Int,
    val sha256: String
) {
    /** "8MB" → 8388608。読めなければ 0。 */
    fun flashSizeBytes(): Int {
        val m = Regex("^(\\d+)MB$").find(flashSize) ?: return 0
        return m.groupValues[1].toInt() * 1024 * 1024
    }
}

class EdgeFirmwareError(message: String) : Exception(message)

object EdgeFirmware {

    const val BASE = "https://raw.githubusercontent.com/laxei-app/hgc-master/main/firmware/"

    // ── 通信を伴わない部分(単体試験の対象) ──────────────────────

    fun parseManifest(json: String): List<EdgeFirmwareEntry> {
        val root = JSONObject(json)
        val arr = root.optJSONArray("edge") ?: throw EdgeFirmwareError("目録に edge がありません")
        val out = ArrayList<EdgeFirmwareEntry>(arr.length())
        for (i in 0 until arr.length()) {
            val o = arr.getJSONObject(i)
            out.add(
                EdgeFirmwareEntry(
                    id = o.optString("id"),
                    name = o.optString("name"),
                    chip = o.optString("chip"),
                    flashSize = o.optString("flashSize"),
                    version = o.optString("version"),
                    build = o.optString("build"),
                    file = o.optString("file"),
                    offset = o.optInt("offset", 0),
                    size = o.optInt("size", 0),
                    sha256 = o.optString("sha256").lowercase()
                )
            )
        }
        return out
    }

    /** 読めた容量に合うものを選ぶ。合うものが無ければ null(勝手に別機種を焼かない)。 */
    fun pickFor(flashSizeBytes: Int, list: List<EdgeFirmwareEntry>): EdgeFirmwareEntry? =
        list.firstOrNull { it.flashSizeBytes() == flashSizeBytes && flashSizeBytes > 0 }

    /** 落としてきたものが目録どおりか。大きさと SHA256 の両方を見る。 */
    fun verify(data: ByteArray, entry: EdgeFirmwareEntry): Boolean {
        if (entry.size > 0 && data.size != entry.size) return false
        if (entry.sha256.isEmpty()) return false
        val got = MessageDigest.getInstance("SHA-256").digest(data)
            .joinToString("") { "%02x".format(it) }
        return got == entry.sha256
    }

    /** 資産に入れてあるスタブローダ(esp-flasher-stub。Apache-2.0 / MIT)を読む。 */
    fun parseStub(json: String): EspStub {
        val o = JSONObject(json)
        val dec = Base64.getDecoder()
        return EspStub(
            entry = o.getLong("entry").toInt(),
            text = dec.decode(o.getString("text")),
            textStart = o.getLong("text_start").toInt(),
            data = dec.decode(o.getString("data")),
            dataStart = o.getLong("data_start").toInt()
        )
    }

    // ── 通信するところ ──────────────────────────────────────

    fun loadStub(ctx: Context): EspStub =
        ctx.assets.open("esp32s3_stub.json").use { parseStub(it.readBytes().toString(Charsets.UTF_8)) }

    /**
     * 目録を取ってくる。
     * ※ スマホがエッジの AP に繋がっているとインターネットへ出られない。呼ぶ側で
     *   「先に通常の回線へ戻してください」と案内すること。
     */
    fun fetchManifest(): List<EdgeFirmwareEntry> = parseManifest(httpGet(BASE + "manifest.json").toString(Charsets.UTF_8))

    /** 本体を落として確かめる。合わなければ [EdgeFirmwareError]。 */
    fun download(entry: EdgeFirmwareEntry, progress: ((Int, Int) -> Unit)? = null): ByteArray {
        val data = httpGet(BASE + entry.file, entry.size, progress)
        if (!verify(data, entry)) {
            throw EdgeFirmwareError("落としたファームが目録と一致しません(壊れている可能性があります)")
        }
        return data
    }

    private fun httpGet(url: String, expect: Int = 0, progress: ((Int, Int) -> Unit)? = null): ByteArray {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 15000
        c.readTimeout = 60000
        c.instanceFollowRedirects = true
        try {
            if (c.responseCode != 200) throw EdgeFirmwareError("取得に失敗しました (HTTP ${c.responseCode})")
            val total = if (expect > 0) expect else c.contentLength
            val out = ByteArrayOutputStream(if (total > 0) total else 64 * 1024)
            val buf = ByteArray(64 * 1024)
            c.inputStream.use { ins ->
                while (true) {
                    val n = ins.read(buf)
                    if (n < 0) break
                    out.write(buf, 0, n)
                    progress?.invoke(out.size(), total)
                }
            }
            return out.toByteArray()
        } finally {
            c.disconnect()
        }
    }
}
