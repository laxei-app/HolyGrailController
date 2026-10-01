package app.laxei.twylapse

import android.content.Context
import android.content.res.Configuration
import android.provider.Settings
import java.text.DateFormat
import java.text.NumberFormat
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

// 【言語(2026-10-01 依頼)】画面に出す文字は res/values(英語) と res/values-ja(日本語) から引く。
//  ここに集めてあるのは「リソースでは書けないもの」だけ:
//
//   ・言語の選び方と切り替え … スマホに合わせる / 日本語 / English
//   ・日付と時刻の並び       … 2026-09-29 19:50 か Sep 29, 2026 7:50 PM か
//   ・単位                   … 標高は メートル か フィート か
//   ・数値の桁区切り         … 1,168
//
//  **文字列の足し算で文を組まないこと。**語順が言語で変わるので、可変部は必ず
//  リソース側のプレースホルダ(%1$s)で受ける。
object Loc {

    // ---- 言語の選択 ------------------------------------------------------
    //
    // 【AppCompatDelegate.setApplicationLocales は使わない(2026-10-01 実機で踏んだ)】
    //  あれは**Activity を作り直す**。作り直すと onDestroy が走って Entity を畳むが、
    //  Entity の後始末(detectSsdpBase::watchStop)が join できないスレッドを join して
    //  **プロセスごと落ちる**(signal 6 / thread::join failed)。しかも畳んだ直後に
    //  「温かい起動」と見なされて nativeInit を飛ばすので、生き返らせることもできない。
    //
    //  そこで**自分で覚えて、プロセスごと入れ直す**。「出荷時設定に戻す」と同じ
    //  RestartActivity の道で、Entity は新しいプロセスで素直に立ち上がる。
    //  効かせるのは attachBaseContext(下の wrap)。

    const val SYSTEM = ""   // スマホに合わせる
    private const val KEY = "langTag"

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences("tlp", Context.MODE_PRIVATE)

    /** 選ばれている言語。スマホに合わせるなら "" */
    fun selected(ctx: Context): String = prefs(ctx).getString(KEY, SYSTEM) ?: SYSTEM

    /** 覚えるだけ。効かせるには**入れ直し**が要る(呼んだ側が RestartActivity へ)。 */
    fun save(ctx: Context, tag: String) {
        prefs(ctx).edit().putString(KEY, tag).commit()   // 直後にプロセスを落とすので commit
    }

    /** Activity / Service の attachBaseContext で包む。これで全部のリソースがその言語になる。 */
    fun wrap(base: Context): Context {
        val tag = selected(base)
        if (tag.isEmpty()) return base                   // スマホに合わせる = 何もしない
        val cfg = Configuration(base.resources.configuration)
        cfg.setLocale(Locale.forLanguageTag(tag))
        return base.createConfigurationContext(cfg)
    }

    /** いま実際に効いている言語(スマホに合わせるを解決したもの)。 */
    fun locale(ctx: Context): Locale {
        val cfg = ctx.resources.configuration
        return if (cfg.locales.isEmpty) Locale.US else cfg.locales.get(0)
    }

    fun isJa(ctx: Context): Boolean = locale(ctx).language == "ja"

    // ---- 日付と時刻 ------------------------------------------------------
    //
    // 保存と通信は ISO("yyyy-MM-dd HH:mm")のまま。**変えるのは見せ方だけ**。
    //
    // 【12時間制(AM/PM)をいつ使うか】端末の「24時間表示」の設定が**明示されていれば
    //  それに従う**。未設定(既定のまま)なら言語で決める — 日本語は24時間制、英語は12時間制。
    //  端末は日本語のまま表示だけ英語にして試すことがあるので、
    //  `DateFormat.is24HourFormat()` をそのまま使うと英語なのに24時間制になってしまう。

    private fun user24h(ctx: Context): Boolean? =
        try { Settings.System.getString(ctx.contentResolver, Settings.System.TIME_12_24) }
        catch (_: Exception) { null }
            ?.let { if (it == "24") true else if (it == "12") false else null }

    fun is24h(ctx: Context): Boolean = user24h(ctx) ?: isJa(ctx)

    // 【日付はその言語の標準の形に合わせる(2026-10-01 ユーザー決定)】
    //  決め打ちをやめ、ロケールの MEDIUM を使う。
    //   日本語 2026/09/30 / 米国 Sep 30, 2026 / 英国 30 Sep 2026
    //  **月が名前になるのが要点**。短い形式(9/30/26)は地域で月と日が
    //  ひっくり返るので、撮影開始日を読み違えるおそれがある。
    //
    //  【言語を変えると日付も変わる】Android の「アプリごとの言語」は
    //  **ロケールそのものを差し替える**仕組みで、書式も追従するのが標準。
    //  ここもそれに合わせている(端末のロケールではなく、選んだ言語で決まる)。
    //  ただし**時刻の 12/24 時間制だけは別**で、端末の設定が優先(上の is24h)。
    private fun datePat(ctx: Context): String {
        val df = DateFormat.getDateInstance(DateFormat.MEDIUM, locale(ctx))
        return (df as? SimpleDateFormat)?.toPattern()
               ?: (if (isJa(ctx)) "yyyy/MM/dd" else "MMM d, yyyy")
    }
    private fun timePat(ctx: Context) = if (is24h(ctx)) "HH:mm" else "h:mm a"
    //  曜日付き。曜日を前に置くか後ろに置くかは言語で違うので、そこだけ分ける。
    private fun dowPat(ctx: Context) =
        if (isJa(ctx)) datePat(ctx) + " (E)" else "E, " + datePat(ctx)

    fun dateFmt(ctx: Context): DateFormat = SimpleDateFormat(datePat(ctx), locale(ctx))
    fun timeFmt(ctx: Context): DateFormat = SimpleDateFormat(timePat(ctx), locale(ctx))
    fun dateTimeFmt(ctx: Context): DateFormat =
        SimpleDateFormat(datePat(ctx) + " " + timePat(ctx), locale(ctx))
    fun dowDateFmt(ctx: Context): DateFormat = SimpleDateFormat(dowPat(ctx), locale(ctx))

    fun date(ctx: Context, d: Date): String = dateFmt(ctx).format(d)
    fun time(ctx: Context, d: Date): String = timeFmt(ctx).format(d)
    fun dateTime(ctx: Context, d: Date): String = dateTimeFmt(ctx).format(d)

    /** 「9/30」のような短い日付(概要スケジュールの日付チップ)。
     *  ここもロケールに合わせる(英国なら 30/9)。幅が狭いので月は数字のまま。 */
    fun monthDay(ctx: Context, d: Date): String {
        val pat = try { android.text.format.DateFormat.getBestDateTimePattern(locale(ctx), "Md") }
                  catch (_: Exception) { "M/d" }
        return SimpleDateFormat(pat, locale(ctx)).format(d)
    }

    /** Entity が返す "yyyy-MM-dd HH:mm[:ss]" を、その言語の見せ方へ直す。 */
    private val isoDT = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US)
    fun fromIso(s: String): Date? =
        try { if (s.length >= 16) isoDT.parse(s.substring(0, 16)) else null }
        catch (_: Exception) { null }

    fun isoToDateTime(ctx: Context, s: String): String =
        fromIso(s)?.let { dateTime(ctx, it) } ?: s
    fun isoToTime(ctx: Context, s: String): String =
        fromIso(s)?.let { time(ctx, it) } ?: s

    /** Entity の "HH:mm"(日付を持たない時刻)を、その言語の見せ方へ直す。 */
    private val isoT = SimpleDateFormat("HH:mm", Locale.US)
    fun hhmm(ctx: Context, s: String): String =
        if (is24h(ctx)) s
        else try { isoT.parse(s.take(5))?.let { timeFmt(ctx).format(it) } ?: s }
             catch (_: Exception) { s }

    // ---- 数と単位 --------------------------------------------------------

    /** 桁区切り。1168 → "1,168" */
    fun num(ctx: Context, v: Long): String = NumberFormat.getInstance(locale(ctx)).format(v)
    fun num(ctx: Context, v: Int): String = num(ctx, v.toLong())

    // 【標高(2026-10-01 ユーザー決定)】持つのは常にメートル。**見せ方だけ**フィートにする。
    //  1ft = 0.3048m はちょうどの定義値なので、往復の誤差は丸めぶんだけ。
    private const val FT_PER_M = 1.0 / 0.3048

    fun useFeet(ctx: Context): Boolean = !isJa(ctx)

    /** 表示用。メートルの値を、その言語の単位の**数字だけ**にする(単位はリソース側)。 */
    fun altValue(ctx: Context, m: Int): Int =
        if (useFeet(ctx)) Math.round(m * FT_PER_M).toInt() else m

    /** 入力用。画面に打たれた値をメートルへ戻す。 */
    fun altToMeters(ctx: Context, v: Int): Int =
        if (useFeet(ctx)) Math.round(v / FT_PER_M).toInt() else v

    /** 単位の記号。"m" / "ft" */
    fun altUnit(ctx: Context): String = if (useFeet(ctx)) "ft" else "m"
}
