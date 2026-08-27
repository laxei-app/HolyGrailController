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

/**
 * どこへ何を書くかの見立て。
 *  keepsSettings=true … 端末の設定(NVS)を残したまま、本体だけを書き換える
 *  keepsSettings=false … 0 番地からまるごと。設定は消える(新品はこちら)
 */
class WritePlan(val offset: Int, val data: ByteArray, val keepsSettings: Boolean)

/** 端末に何をするか。 */
enum class FlashAction {
    SKIP,       // 同じ版数が入っている。書かない
    APP_ONLY,   // 本体だけ入れ替える。設定は残る
    FULL        // 土台(ブートローダ・区切り)ごと全部書く。設定は消える
}

/**
 * 結合イメージの中の区切り。0 番地から順に、ブートローダ / パーティション表 /
 * NVS(設定) / otadata / アプリ本体 が並んでいる。
 *
 * 【設定を残せる理由】NVS は 0x9000〜0xE000 にある。0xE000 から後ろだけを書けば、
 *  otadata とアプリ本体は新しくなり、その手前の設定はそのまま残る。
 *  0xE000 は 4KB の境目に乗っているので、消去の単位ともぶつからない。
 */
object FlashMap {
    const val BOOTLOADER = 0x0
    const val BOOTLOADER_END = 0x8000
    const val PART_TABLE = 0x8000
    const val PART_TABLE_LEN = 0xC00
    const val NVS = 0x9000
    const val NVS_END = 0xE000
    const val OTADATA = 0xE000        // ここから後ろが「設定を残す」ときに書く範囲
    const val APP = 0x10000

    // アプリの素性は **アプリ先頭 +0x20** に必ず置かれる(ESP-IDF の esp_app_desc_t)。
    //  番地が仕様で決まっているので、こちらで場所を作らずに済む。
    const val APP_DESC = APP + 0x20
    const val APP_DESC_LEN = 256
}

/** ファームの素性。決まった番地から読み出したもの。 */
class FwIdentity(val name: String, val version: String, val valid: Boolean) {
    override fun toString(): String =
        if (valid) "$name $version" else "読めません"
}

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

    /**
     * アプリの素性(名前・版数)を読み取る。desc は APP_DESC から APP_DESC_LEN バイト。
     *
     * 見るもの:
     *  ・+0 の magic_word が 0xABCD5432 か(そもそも app_desc の場所か)
     *  ・+176 の検査値が version+name の CRC32 と合うか(私たちが刻んだものか)
     * どちらか欠ければ valid=false。**そのときは土台ごと書き直す**判断にする。
     * 私たちのものでないファーム(工場出荷など)は検査値を持たないので、ここで弾ける。
     */
    fun parseIdentity(desc: ByteArray): FwIdentity {
        if (desc.size < FlashMap.APP_DESC_LEN) return FwIdentity("", "", false)
        val magic = (0 until 4).fold(0L) { a, k -> a or ((desc[k].toLong() and 0xFF) shl (8 * k)) }
        if (magic != 0xABCD5432L) return FwIdentity("", "", false)

        fun field(off: Int): String {
            var end = off
            while (end < off + 32 && desc[end].toInt() != 0) end++
            return String(desc, off, end - off, Charsets.US_ASCII)
        }
        val version = field(16)
        val name = field(48)

        val stored = (0 until 4).fold(0L) { a, k -> a or ((desc[176 + k].toLong() and 0xFF) shl (8 * k)) }
        val crc = java.util.zip.CRC32().apply { update(desc, 16, 64) }.value
        return FwIdentity(name, version, stored == crc)
    }

    /**
     * どうするかを決める。
     *
     * dev … 端末から読み取った素性 / img … これから焼くものの素性
     * sameBase … 端末のブートローダとパーティション表が、焼くものと同じか。
     *            同じなら区切りが変わらないので本体だけ入れ替えてよい。違うとき
     *            (新品・別のファーム・区切りを変えた改修)は表ごと入れ替えないと起動しない。
     *
     * 決め方(2026-08-26 ユーザー指示):
     *  ・**検査値が合わない** → 素性が読めない/私たちのものでない → 土台ごと全部書く
     *  ・**版数が同じ**       → 書く必要が無いので何もしない
     *  ・**版数が違う**       → 書いてよい。土台が同じなら本体だけ(設定が残る)
     */
    fun decide(dev: FwIdentity, img: FwIdentity, sameBase: Boolean): FlashAction = when {
        !dev.valid -> FlashAction.FULL
        // 焼く側に素性が無い(まだ刻んでいない古い公開物)ときは版数を比べようが無いので、
        //  「同じかもしれない」で飛ばさず必ず書く。安全側に倒す。
        !img.valid -> if (sameBase) FlashAction.APP_ONLY else FlashAction.FULL
        dev.name != img.name -> FlashAction.FULL          // 別のアプリが入っている
        dev.version == img.version -> FlashAction.SKIP
        sameBase -> FlashAction.APP_ONLY
        else -> FlashAction.FULL
    }

    /** 決めた内容に沿って、どこへ何を書くかを組み立てる。SKIP なら null。 */
    fun planWrite(image: ByteArray, action: FlashAction): WritePlan? = when {
        action == FlashAction.SKIP -> null
        action == FlashAction.APP_ONLY && image.size > FlashMap.OTADATA ->
            WritePlan(FlashMap.OTADATA,
                      image.copyOfRange(FlashMap.OTADATA, image.size),
                      keepsSettings = true)
        else -> WritePlan(0, image, keepsSettings = false)
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
        // 【公開直後は古いものが返る(2026-08-26 実機で確認)】置き場は 5 分ほどキャッシュされ、
        //  しかも配信ノードごとに持ち方が違う。実際、PC からは新しい版が見えるのにスマホには
        //  古い版が返り、「公開したのに端末が更新されない」状態が数分続いた。
        //  下の細工でこちら側の持ち回しは避けられるが、**配信側の 5 分は待つしかない**。
        //  公開直後に焼くときは数分おいてからにすること。
        val fresh = url + (if (url.contains('?')) "&" else "?") + "t=" + System.currentTimeMillis()
        val c = URL(fresh).openConnection() as HttpURLConnection
        c.setRequestProperty("Cache-Control", "no-cache")
        c.useCaches = false
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
