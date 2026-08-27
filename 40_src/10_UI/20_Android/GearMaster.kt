package app.laxei.holygrail

import android.content.Context
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

// 機材マスタ(カメラ/レンズの一覧)を公開リポジトリから取ってくる(2026-08-27)。
//
// 【版は2つある】
//  schema   … 構造の版。項目を増やす・意味を変えたときに上がる。
//             自分が読める版を超えていたら**取り込まない**。古い一覧のままでも動くが、
//             知らない構造を読んで壊れるよりよい。
//  revision … 中身の版。機種を足すたびに1つ進む。手元より大きければ取り込む。
//             機種が1台増えるたびにアプリの更新を強いないために、構造とは分けてある。
//
// 【3段構え】屋外・初回起動・エッジのAPに繋いでいる間は外に出られない。
//  1. 取り込んだもの(/master)
//  2. アプリ同梱(assets/master)
//  3. それも無ければ Entity の出荷時フォールバック(EOS R10 + SIGMA 12mm)
//
// 【同梱で上書きしてよいのは「同梱のほうが新しいとき」だけ】
//  以前は起動のたびに同梱アセットで無条件に上書きしていた。そのままネットから新しい
//  一覧を取ってきても**次の起動で古いものに戻る**ので、版を見て決めるようにした。
object GearMaster {

    const val BASE = "https://raw.githubusercontent.com/laxei-app/hgc-master/main/master/"
    const val MANIFEST = "manifest.json"

    /** このアプリが読める構造の版。公開側がこれを超えていたら手を出さない。 */
    const val SCHEMA_MAX = 1

    private const val PREF = "gearMaster"
    private const val KEY_LAST_CHECK = "lastCheckDay"

    class Info(val schema: Int, val revision: Int, val files: List<Entry>)
    class Entry(val name: String, val size: Int, val sha256: String)

    fun parseManifest(json: String): Info {
        val o = JSONObject(json)
        val arr = o.optJSONArray("files")
        val list = ArrayList<Entry>()
        for (i in 0 until (arr?.length() ?: 0)) {
            val f = arr!!.getJSONObject(i)
            list.add(Entry(f.optString("name"), f.optInt("size", 0),
                           f.optString("sha256").lowercase()))
        }
        return Info(o.optInt("schema", 0), o.optInt("revision", 0), list)
    }

    /** 落としたものが目録どおりか。大きさと SHA256 の両方を見る。 */
    fun verify(data: ByteArray, e: Entry): Boolean {
        if (e.size > 0 && data.size != e.size) return false
        if (e.sha256.isEmpty()) return false
        return MessageDigest.getInstance("SHA-256").digest(data)
            .joinToString("") { "%02x".format(it) } == e.sha256
    }

    /**
     * 取り込むべきか。
     *  ・構造が読めない → 取り込まない
     *  ・中身が手元より新しい → 取り込む
     */
    fun shouldAdopt(remote: Info, localRevision: Int): Boolean =
        remote.schema in 1..SCHEMA_MAX && remote.revision > localRevision

    // ── 手元の版 ────────────────────────────────────────────

    /** /master に入っている版。まだ何も無ければ 0。 */
    fun installedRevision(baseDir: File): Int =
        runCatching { parseManifest(File(File(baseDir, "master"), MANIFEST).readText()).revision }
            .getOrDefault(0)

    /** アプリ同梱の版。 */
    fun bundledRevision(ctx: Context): Int =
        runCatching { parseManifest(ctx.assets.open("master/$MANIFEST").use {
            it.readBytes().toString(Charsets.UTF_8) }).revision }.getOrDefault(0)

    // ── 同梱から /master へ ─────────────────────────────────

    /**
     * 同梱アセットを /master へ置く。**同梱のほうが新しいときだけ**上書きする。
     * 取り込み済みのものを消さないための判断で、ここが以前の作りとの一番の違い。
     */
    fun installBundledIfNewer(ctx: Context, baseDir: File) {
        try {
            val dir = File(baseDir, "master")
            if (!dir.exists()) dir.mkdirs()
            val have = installedRevision(baseDir)
            val bundled = bundledRevision(ctx)
            val names = ctx.assets.list("master")?.toList() ?: return
            // 何も入っていなければ版に関わらず置く(初回起動)
            if (have > 0 && bundled <= have) return
            for (n in names) {
                ctx.assets.open("master/$n").use { ins ->
                    File(dir, n).outputStream().use { os -> ins.copyTo(os) }
                }
            }
        } catch (_: Exception) {
            // 置けなくても Entity の出荷時フォールバックで最低限は動く
        }
    }

    // ── 公開リポジトリから ──────────────────────────────────

    /** 今日はもう見に行ったか(1日1回にする)。 */
    fun checkedToday(ctx: Context): Boolean {
        val p = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)
        return p.getString(KEY_LAST_CHECK, "") == today()
    }

    fun markChecked(ctx: Context) {
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)
            .edit().putString(KEY_LAST_CHECK, today()).apply()
    }

    private fun today(): String =
        java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).format(java.util.Date())

    /**
     * 新しければ取り込む。戻り: 取り込んだ版(何もしなければ null)。
     * **通信するので必ず別スレッドから呼ぶこと。**
     * 失敗は握りつぶす(圏外・エッジのAPに繋いでいる等。手元の一覧で動き続ける)。
     */
    fun fetchIfNewer(ctx: Context, baseDir: File, log: ((String) -> Unit)? = null): Int? {
        return try {
            val remote = parseManifest(httpGet(BASE + MANIFEST).toString(Charsets.UTF_8))
            val have = installedRevision(baseDir)
            log?.invoke("機材マスタ: 手元=版 $have / 公開=版 ${remote.revision} (schema ${remote.schema})")
            if (!shouldAdopt(remote, have)) {
                if (remote.schema > SCHEMA_MAX) {
                    log?.invoke("機材マスタの構造が新しすぎます(schema ${remote.schema})。アプリを更新してください")
                }
                return null
            }
            // 全部そろって照合できてから置き換える(片方だけ新しい状態を作らない)
            val got = LinkedHashMap<String, ByteArray>()
            for (f in remote.files) {
                val d = httpGet(BASE + f.name)
                if (!verify(d, f)) { log?.invoke("機材マスタ ${f.name} が壊れています"); return null }
                got[f.name] = d
            }
            val dir = File(baseDir, "master")
            if (!dir.exists()) dir.mkdirs()
            for ((n, d) in got) File(dir, n).writeBytes(d)
            File(dir, MANIFEST).writeText(
                """{"schema":${remote.schema},"revision":${remote.revision}}""")
            log?.invoke("機材マスタを版 ${remote.revision} に更新しました")
            remote.revision
        } catch (e: Exception) {
            log?.invoke("機材マスタの確認に失敗: ${e.message}")
            null
        }
    }

    private fun httpGet(url: String): ByteArray {
        // 置き場は数分キャッシュされる。公開直後でも新しいものを見に行けるようにする。
        val fresh = url + (if (url.contains('?')) "&" else "?") + "t=" + System.currentTimeMillis()
        val c = URL(fresh).openConnection() as HttpURLConnection
        c.setRequestProperty("Cache-Control", "no-cache")
        c.useCaches = false
        c.connectTimeout = 10000
        c.readTimeout = 30000
        try {
            if (c.responseCode != 200) throw Exception("HTTP ${c.responseCode}")
            val out = ByteArrayOutputStream()
            c.inputStream.use { it.copyTo(out) }
            return out.toByteArray()
        } finally {
            c.disconnect()
        }
    }
}
