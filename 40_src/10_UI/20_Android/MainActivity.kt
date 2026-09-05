package app.laxei.holygrail

import android.Manifest
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiInfo
import android.net.wifi.WifiManager
import android.provider.MediaStore
import android.os.Build
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.PopupMenu
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.CompoundButton
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import android.widget.ViewFlipper
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import com.google.android.material.slider.LabelFormatter
import com.google.android.material.slider.RangeSlider
import com.google.android.material.slider.Slider
import org.json.JSONArray
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Locale
import java.util.TimeZone

// 330撮影計画設定 / 430撮影中(仕様書10 §7.3)。ViewFlipperで切替。
// 開始/終了時刻を編集するとスケジュールを自動生成して表示し、撮影開始できる。
class MainActivity : AppCompatActivity(), HgeListener {

    private lateinit var flipper: ViewFlipper

    // 330
    private lateinit var startDate: Button
    private lateinit var startTime: Button
    private lateinit var endDate: Button
    private lateinit var endTime: Button
    private lateinit var resetButton: Button
    // センサー/焦点距離/画角の文言(2026-08-08 UI依頼で撮影シミュレーション画面へ移設)。
    // 計画JSONの更新時に作り、SimPage が生成済みならそのまま渡す(未生成なら次のbindで反映)。
    private var simGearText: String = ""
    private lateinit var placeText: TextView
    private lateinit var latlngText: TextView
    private lateinit var cameraText: TextView
    private lateinit var lensText: TextView
    private lateinit var intervalText: TextView
    private lateinit var npfText: TextView
    private lateinit var landscapeCheck: android.widget.Switch	// スライドSW(2026-08-24 UI依頼でチェックボックスから変更)
    // 同期撮影(2026-08-25)。主カメラで測光した露出を追加カメラへも配って全台で撮る。
    private lateinit var syncShotCheck: android.widget.Switch
    private lateinit var subCamRow: android.view.View
    private lateinit var subCamText: TextView
    // 表示更新でのプログラム的セットが native を呼び返すのを防ぐ(landscape と同じ理由)。
    private var suppressSyncShot = false
    // 現在の追加カメラの名前(選択ダイアログの初期チェック状態に使う)。
    private var planSubCamNames: MutableList<String> = mutableListOf()
    private var suppressLandscape = false   // updatePlanDisplay でのチェック設定が native を呼ばないように
    // 項目11: 計画1ページ目の撮影方向/仰角ウィジェット(compass/elevationView/dirText)は廃止。
    // 方向・仰角は撮影シミュレーション画面で設定する(cs の azimuth/elevation は保持)。
    private lateinit var planOverview: LinearLayout      // 概要スケジュール(先頭ページ・表示専用)
    // 概要スケジュールの字下げ(dp)。screen_plan.xml の見出し TextView の幅と合わせると
    //  内容の左端が撮影場所/エッジ端末/カメラ…と揃う。
    private val OVERVIEW_INDENT_DP = 96
    private lateinit var planPager: PlanPager            // 横スライドのページャ(先頭+薄明ページ)
    private lateinit var planFormScroll: ScrollView      // 先頭ページのフォーム縦スクロール
    private lateinit var planListScroll: ScrollView      // 先頭ページの計画リスト(分割バー上)
    private lateinit var planListContainer: LinearLayout
    // 【撮影計画ひな形(2026-09-04 UI依頼)】ひな形は撮影計画とまったく同じ形なので、
    //  画面も撮影計画画面をそのまま使う。上の一覧の中身と、できることだけを切り替える。
    //  ユーザーには別画面に見えるが、中身は同じページ(ViewFlipper は増えない)。
    //  **モードを変えるのは画面に入るときと出るときだけ**。ポーリングの途中で変わると
    //  選択が化ける(以前 restoreViewingSelection で起きた不具合と同じ形になる)。
    private var tplMode = false
    private var planIdBeforeTpl = ""      // ひな形画面へ入る前に選んでいた計画(戻すため)

    // 編集対象の計画 id。切替(選択/新規/複製/起動時)のたびに「変更の取り消し」用のベースラインを取り直す。
    private var currentPlanId: String = ""
        set(value) {
            val changed = field != value; field = value
            if (changed) {
                capturePlanBaseline()
                // 項目6: 進捗ステータスは「選択中の計画」に紐付く。計画を切り替えたら即座に選択計画の状態へ更新する
                //  (前の計画の表示が残らないように・別カメラの計画を選べばその進捗を出す)。
                if (::captureStatus.isInitialized) runOnUiThread { refreshCaptureStatusForCurrent() }
            }
        }
    private var planBaseline = ""           // 変更の取り消し用: 計画/画面に入った時点の cs JSON(nativeGetPlanJson)
    // 変更あり/なしを見るための「比べる用」の形。カメラのパスワードは暗号化されて載っており、
    //  **同じ内容でも呼ぶたびに違う暗号文**になる(毎回ちがう乱数を混ぜるため)。そのまま比べると
    //  何も触っていなくても常に「変更あり」になり、取り消しボタンが最初から赤いままになる
    //  (2026-08-30 実機で確認。他の画面は入力欄の値で見ているのでこの問題は出ない)。
    //  比較のときだけ伏せる。取り消しで書き戻すのは生の planBaseline なので中身は変わらない。
    private fun planSig(j: String) = j.replace(Regex("\"authPass\":\"[^\"]*\""), "\"authPass\":\"\"")
    // 撮影計画画面の「変更の取り消し」ボタンの dirty 連動(現在の計画 != ベースライン なら有効)。
    // 計画画面表示中のみ、planExec 上で現在値を取り比較する(編集と直列化して安全)。
    private val planDirtyWatch = object : Runnable {
        override fun run() {
            if (::flipper.isInitialized && flipper.displayedChild == 0 && ::resetButton.isInitialized) {
                planExec.execute {
                    val cur = try { HgeNative.nativeGetPlanJson() } catch (e: Exception) { "" }
                    runOnUiThread {
                        if (flipper.displayedChild == 0)
                            setCancelEnabled(resetButton, !planReadOnly && planBaseline.isNotEmpty() &&
                                             planSig(cur) != planSig(planBaseline))
                    }
                }
            }
            handler.postDelayed(this, 500)
        }
    }
    // 計画の選択・改名・各種編集(g_plan/g_editIdを触る操作)は単一スレッドで直列化し競合を防ぐ
    // (例: 改名と別計画選択が並走すると選択がファイルから古い名前を読み戻して改名が無効化される)。
    private val planExec = java.util.concurrent.Executors.newSingleThreadExecutor()
    // 項目5: エッジへの計画送信(数秒のネットワークI/O)専用の単一スレッド。planExec と分離することで、
    //  送信中でも計画選択(planExec)がブロックされず UI が固まらない。送る計画JSONは送信前に取得して渡す。
    private val edgeExec = java.util.concurrent.Executors.newSingleThreadExecutor()
    private val capturingPlans = mutableSetOf<String>()  // 実撮影中(撮影窓内)の計画 id 群=カメラ点滅
    private val disconnectedPlans = mutableSetOf<String>() // カメラ未検出(NOCAMERA/旧DISCONNECTED)の計画 id 群=✖点灯
    private val waitingPlans = mutableSetOf<String>()    // 撮影要求済・撮影窓前で待機中(カメラOK)の計画 id 群=カメラ点灯
    private val nocamDialogShown = mutableSetOf<String>() // カメラ未検出ポップアップを表示済みの計画 id(多重表示抑止。Phase3)
    // 計画ごとの最新の進捗。ポーリング/イベントが来るたびに、その計画を表示中かどうかに関わらず必ず入れる。
    //  こうしておくと、詳細を別の計画へ切り替えた瞬間に「次のポーリングを待たずに」今の枚数と残り時間を
    //  出せる。従来は受信したその場で「表示中の計画なら」書いていたので、切り替え直後は枚数の無い
    //  「● 撮影中」だけになり、数秒経つまで実態が分からなかった。
    private data class CapProgress(val frame: Int, val total: Int, val remainSec: Int)
    private val planProgress = mutableMapOf<String, CapProgress>()
    // 進捗JSON({"frame","total","remainSec"})を計画ごとに控える。総数が入っていないものは捨てる
    //  (0/0枚 と出すくらいなら、枚数なしの表示に留めるほうが誤解が無い)。
    private fun rememberProgress(planId: String, o: JSONObject) {
        if (planId.isEmpty()) return
        val total = o.optInt("total")
        if (total <= 0) return
        planProgress[planId] = CapProgress(o.optInt("frame"), total, o.optInt("remainSec"))
    }
    // 項目11: 撮影開始前(待機中)に未検出ポップアップを出した計画 id。待機中は1回だけ出すために使う。
    //  clearNoCam(状態復帰)では消さない — 消すと NOCAMERA↔SEARCHING の往復で毎回出てしまう。
    //  中止/エッジから削除で消し、次に開始要求したときは再び1回出るようにする。
    private val nocamShownWaiting = mutableSetOf<String>()
    private val nocamDialogs = mutableMapOf<String, androidx.appcompat.app.AlertDialog>() // 表示中のNOCAMERAダイアログ(状態が復帰/停止したら閉じるための参照)
    private val edgeAppliedSerials = mutableSetOf<String>() // ①エッジ書き戻し済みのカメラserial(重複適用の抑止。1serial=1回)
    private val promptingCamSerials = mutableSetOf<String>() // ②登録プロンプト表示中のカメラserial(重複ダイアログ抑止)
    private val declinedCamSerials by lazy { loadDeclinedCamSerials() } // ②「いいえ」で自動プロンプトを抑止するserial(永続。手動登録は別途可能)
    private val stoppingPlans = mutableSetOf<String>()   // 「中止」操作済みで停止(IDLE)確定待ちの計画 id。確定までNOCAMERAダイアログを抑止し無限再表示を防ぐ
    private val startingPlans = mutableSetOf<String>()   // 開始操作中(タップ〜開始要求の結果確定まで)の計画 id。この間のポーリングIDLE(=まだ届いていないだけ)で
                                                          // 集合から外して“開始前アイコン+ポーリング対象外”に落ちるレースを防ぐ(2台順次開始は直列化で到達が遅れる)
    // 項目6: エッジが実際に保有(ロスターに存在)する計画。エッジ名→保有計画id集合。応答したエッジの分だけ
    //  更新し、未応答/オフラインのエッジ分は前回値を保持(スイープ取りこぼしでロックが揺れないように)。
    //  ロック判定はこれ(=エッジが持っている)＋開始操作の過渡(下 isPlanOnEdge)で行う。エッジ選択(スピナー=
    //  pe_ 割り当て)だけでは「保有」にはならないので、選んだだけの未開始計画はロックしない。
    private val edgeHeldByEdge = HashMap<String, MutableSet<String>>()
    // 計画ごとの「認証で弾かれている理由」(hgc::notice)。エッジの進捗で運ばれてくる。
    //  カメラは見つかっているので、「見つかりません」ではなく理由を出すために使う。
    private val planAuthNotice = HashMap<String, Int>()
    // 【ロックの解除は遅らせる(2026-08-28 仕様確定)】
    //  エッジへ送った計画で使っているカメラは、スマホ側で変更も削除もできない。エッジは
    //  受け取った計画をそのまま使い続け、**スマホ抜きでも単独で開始できる**ので、手元だけ
    //  書き換えると「画面の値」と「実際に撮る値」が食い違う。
    //  解除の道は3本。**いずれも早すぎてはいけない**(早いと、まだエッジが持っているのに
    //  変更できてしまう)。安全側は必ず「遅らせる」方。
    //   1. エッジが「もう持っていない」と言った  … reconcileEdgeRoster。これが一番確か
    //   2. 窓の終了から猶予を過ぎた            … エッジが見えないとき用の保険。下の猶予
    //   3. エッジ登録そのものを消した          … 壊れた/失くした端末で詰まないための逃げ道
    //  また、この台帳は**アプリを終了しても消してはいけない**。消えるとロックが外れ、
    //  次のスイープまで無防備になる。prefs へ残す。
    //
    //  猶予は「計画の終了時刻」から数える。**撮影中は猶予に関係なくロックされる**
    //  (撮っている間は必ず 今<終了時刻 なので下の判定が true)。猶予が効くのは
    //  「終了時刻を過ぎた後、エッジが見えない間」だけで、その時点でエッジが持っている
    //  複製は窓が過去=二度と走らないので、守る相手が既に無害になっている。
    //  よって長く取る意味は無く、次の計画として直したいときに邪魔になるだけ
    //  (2026-08-29 に1時間から短縮)。
    //
    //  長さは**その計画の撮影周期の2倍**。エッジ側の撮影期間はそもそも「窓の終了 + 1フレーム」
    //  まで伸びる(holyGrailEntity.cpp の PRE_MARGIN_SEC のコメント参照)ので、最低でも周期1本は
    //  必要で、残り1本をスマホ/エッジの時計ずれと後片付けの余白に充てる。固定の分数にすると
    //  周期を長くしたときに足りなくなる。下の値は**周期が読めなかったときだけ**使う既定値。
    private val kLockGraceFallbackMs = 60L * 1000L
    // この計画は「エッジに送信済み(=どこかのエッジが保有)」か。ロック(編集/削除不可)の判定に使う。
    //  開始操作の過渡(startingPlans)や実行中集合も、ロスター反映前の一瞬をロックするため含める。
    private fun isPlanOnEdge(id: String): Boolean =
        (edgeHeldByEdge.values.any { it.contains(id) } && planLockActive(id)) || isPlanUsingCamera(id)
    // 項目3: この計画は「今カメラを実際に使っている」か(開始要求中/待機/撮影中/未検出)。
    //  エッジが保有しているだけ(中止後も常駐=項目4)はカメラを使っていないので含めない。
    //  編集ロック(isPlanOnEdge)とは別物。予約重複の開始可否はこちらで判定する
    //  (isPlanOnEdge で判定すると、停止済みでエッジ常駐の計画が相手を永久にブロックしてしまう)。
    private fun isPlanUsingCamera(id: String): Boolean =
        startingPlans.contains(id) || capturingPlans.contains(id) ||
        waitingPlans.contains(id) || disconnectedPlans.contains(id)
    private val schedulePages = mutableListOf<ScheduleView>()   // §7.3.2 薄明ページの ScheduleView(読取専用切替に使う)
    private val twilightPages = mutableListOf<View>()           // 薄明ページのラッパ(計画名ヘッダ+ScheduleView)。ページャ追加/削除用
    private var simPage: SimPage? = null                        // 撮影シミュレーション(§7.3 画面360)。ページャ最終ページ(永続1インスタンス)
    private var starsLoadStarted = false                        // fixed_star.json の読み込みを開始済み
    // シミュレーションのネイティブ投影計算・恒星読み込み用(planExecを塞がないよう専用の単一スレッド)。
    private val simExec = java.util.concurrent.Executors.newSingleThreadExecutor()
    private val twilightBoxViews = mutableListOf<TextView>()    // 概要の薄明移動ボックス(幅/高さ揃え用)
    private lateinit var captureStatus: TextView     // 撮影中ステータス(plan画面内)
    private var planReadOnly = false                 // 撮影中の計画を表示中=編集不可(item7)
    private var blinkOn = true              // 撮影中カメラアイコンの点滅状態
    private lateinit var edgeSpinner: Spinner

    // 撮影制御方法初期値: メニュー + 方法別エディタ
    private lateinit var planMenu: ImageView
    private var ccmJson: JSONObject? = null     // 編集中のccm全体(初期値 or 計画固有)
    private var editingKey = "night"            // 編集中の方法
    private var editingPlanCcm = false          // true=計画固有ccmを編集 / false=初期値ccm
    // ロック(撮影中/エッジ保有)の計画の撮影制御方法は**見るだけ**にする(2026-09-01 UI依頼)。
    //  以前は編集ボタンも薄明の帯も無効で、何が設定されているのか確かめようが無かった。
    private var ccmReadOnly = false
    private var editColor = 0                    // (旧)per-ccm色。現在は未使用(色はシステム共通へ移行)
    // システム共通の色(型ごとの文字色/背景色。0xRRGGBB)。nativeGetColors から読み込む。
    private val ccmBgMap = HashMap<Int, Int>()
    private val ccmTextMap = HashMap<Int, Int>()
    // 色の設定画面の状態
    private var colorType = "night"
    private var gearColorsOpen = false   // メニューの「色の設定」を展開しているか(既定=畳む)
    private var colorTextPicker: com.jaredrummler.android.colorpicker.ColorPickerView? = null
    private var colorBgPicker: com.jaredrummler.android.colorpicker.ColorPickerView? = null
    // 撮影制御方法の初期値プリセット(型ごとに複数)
    private var presetType = "day"
    private val presetCcms = ArrayList<JSONObject>()
    private var selPresetName: String? = null
    private var presetNameField: EditText? = null
    private var presetPreferCheck: CheckBox? = null

    // 露出(iso/ss/fn)はカメラ設定値の文字列配列からスライダーで選択する。
    private var isoValues = listOf<String>()    // hge_getExpoValues の iso 配列(real昇順)
    private var ssValues = listOf<String>()     // 同 ss 配列(real昇順)
    private var fnValues = listOf<String>()     // 同 fn 配列(レンズf範囲, 昇順)
    // 表示順は「左=暗い時の設定 → 右=明るい時の設定」で統一する(仕様4の方針)。
    //  iso: 左=高感度→右=低感度 / ss: 左=長秒→右=短秒 / fn: 左=開放→右=絞る。
    private var isoDisp = listOf<String>()
    private var ssDisp = listOf<String>()
    private var fnDisp = listOf<String>()
    private lateinit var fixEditor: ExposureEditor      // 夜間 固定露出(単一)
    private lateinit var editLimit: LimitEditor         // 自動露出 露出限界(優先度+明暗を一体化)

    // 430
    private lateinit var capName: TextView
    private lateinit var capGear: TextView
    private lateinit var capDir: TextView
    private lateinit var capState: TextView
    private lateinit var capProgress: TextView
    private lateinit var capCaptured: TextView
    private lateinit var capSchedule: LinearLayout
    private lateinit var capStopButton: Button

    private val startCal = Calendar.getInstance()
    private val endCal = Calendar.getInstance()
    private var latestSchedule = ""

    // エッジ端末
    private data class Edge(val name: String, val ip: String, val port: Int)
    // ネイティブへ渡す「相手の指定」。Wi-Fi なら IP、BLE なら端末名。
    //  BLE にはブロードキャストが無く、探索も接続も名前で行うため(edgeClient.cpp と対)。
    private fun Edge.addr(): String = if (edgeUseBle()) name else ip
    // その端末に話しかけられる見込みがあるか。Wi-Fi は IP 未解決だと無理だが、BLE は名前だけで足りる。
    private fun Edge.reachable(): Boolean = if (edgeUseBle()) name.isNotEmpty() else ip.isNotEmpty()
    private val edges = mutableListOf<Edge>()   // 登録済みエッジ端末(設定で追加、prefsに永続化、オフラインでも選択可)
    // 常時スイープ(edgeSweep)によるエッジ生存状態。true=オンライン/false=オフライン/未登録=不明(起動直後)。
    private val edgeOnline = mutableMapOf<String, Boolean>()
    private val edgeMiss = mutableMapOf<String, Int>()   // スイープUDPの連続無応答回数(閾値超えでTCP生存確認→オフライン判定)
    // エッジ選択スピナーの保存制御。Spinner の onItemSelected は setAdapter/setSelection の中では呼ばれず
    //  レイアウト後に“非同期”で届く。そのため「設定の間だけ true にするフラグ」では抑止できず(false に戻った
    //  後にコールバックが来る)、しかも書き込み先を currentPlanId にすると計画を切り替えた瞬間に
    //  前の計画のエッジが新しい計画へ漏れる。→ ユーザーが触った時だけ保存し、保存先はスピナーが表示中の計画に固定する。
    private var edgeSpinnerUserTouched = false   // ユーザーがスピナーを操作した(=この後の選択通知は本人の意思)
    private var edgeSpinnerPlanId: String = ""   // スピナーが今どの計画の選択を表示しているか(保存先。currentPlanId ではない)
    private var edgeSpinnerLabels: List<String> = emptyList()   // 表示が変わらなければ adapter を作り直さない(余計な通知を出さない)
    private val handler = Handler(Looper.getMainLooper())

    // エッジ設定(QR+PoP §8.2.2)。スキャンしたPoP/端末名を保持。
    private var scannedPop = ""
    private var scannedName = ""
    private var pendingBleAction: (() -> Unit)? = null
    private val BLE_PERM_REQ = 4711
    private fun ensureBlePermissions(action: () -> Unit) {
        val perms = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = perms.filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isEmpty()) action() else { pendingBleAction = action; ActivityCompat.requestPermissions(this, missing.toTypedArray(), BLE_PERM_REQ) }
    }
    // 撮影場所「現在地を取得」の位置情報権限(§7.9)
    private var pendingLocAction: (() -> Unit)? = null
    private val LOC_PERM_REQ = 4712

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == BLE_PERM_REQ) {
            if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) pendingBleAction?.invoke()
            else Toast.makeText(this, "BLE権限が必要です", Toast.LENGTH_LONG).show()
            pendingBleAction = null
        } else if (requestCode == LOC_PERM_REQ) {
            if (grantResults.isNotEmpty() && grantResults.any { it == PackageManager.PERMISSION_GRANTED }) pendingLocAction?.invoke()
            else Toast.makeText(this, "位置情報の権限が必要です", Toast.LENGTH_LONG).show()
            pendingLocAction = null
        }
    }

    private val fmtDate = SimpleDateFormat("yyyy-MM-dd", Locale.US)
    private val fmtTime = SimpleDateFormat("HH:mm", Locale.US)
    private val fmtIso = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.US)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        bindViews()

        acquireMulticastLock()   // 3b: ネイティブSSDP受動待ち受けがマルチキャストを受信するため(無いと破棄される)

        val baseDir = getExternalFilesDir(null) ?: filesDir
        // 機材マスタ: 同梱は**同梱のほうが新しいときだけ**置く(取り込んだ一覧を消さない)。
        GearMaster.installBundledIfNewer(this, baseDir)   // nativeInit より前
        startGearMasterCheck(baseDir)                     // 1日1回、公開リポジトリを見に行く
        HgeNative.nativeSetLogDir(baseDir.absolutePath)
        HgeNative.nativeInit()
        // スマホ⇄エッジの通信路(2026-08-14 指示)。選ぶのはスマホだけ。エッジは常に両方で待ち受ける。
        EdgeBleLink.init(this)
        BuiltinCamera.init(this)   // スマホ内蔵カメラ(Camera2)の入口へ Context を渡す

        // 【内蔵カメラは自動で所持カメラへ入れる(2026-09-05)】端末そのものなので
        //  「登録しますか」と聞く意味が無い。複数のカメラを持つ端末では1台ずつ並ぶ。
        //  既にあるものは触らない(名前をユーザーが変えていることがある)。
        //  マスタ読み込みと同じ単一スレッドで行う(所持カメラの書き込み口は1本に保つ)。
        dataExec.execute {
            val n = try { HgeNative.nativeRegisterBuiltinCameras() } catch (_: Exception) { 0 }
            if (n > 0) { runOnUiThread { if (flipper.displayedChild == 6) buildCameraList() } }
        }
        HgeNative.nativeEdgeSetBle(edgeUseBle())
        HgeNative.nativeSetListener(this)
        // 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。端末TZで「当日」を判定。
        val tzOffMin = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000
        Thread { HgeNative.nativePruneOldLogs(tzOffMin) }.start()
        seedFirstPlaceFromLocation()              // 初回だけ、出荷時の場所を現在地で作り直す
        handler.postDelayed(edgeTimeSync, 3000)   // 選択中エッジへ能動的な時刻同期を開始(RTC無し機/電波悪環境向け)
        handler.postDelayed(edgeSweep, 6000)      // エッジ常時スイープ(生存/IP追従+エッジ側開始・停止の検出。撮影の有無に関わらず30秒毎)
        handler.postDelayed(hgePump, 5000)        // 遅延アームのポンプ(スマホ直接撮影の予約計画の開始スレッドを期日に生成)
        loadColors()
        loadExpoValues()
        buildExposureEditors()
        loadRegisteredEdges()   // 設定で登録したエッジ端末(オフラインでも選択可)
        loadEdgeHeld()          // エッジが持っている計画(=編集ロック)。アプリを終了しても保つ
        applyLogOptsToSelf()    // デバッグログの取捨(既定は採らない)を自分の記録へ効かせる
        refreshEdgeSpinner()

        wireListeners()

        // 項目I: Android標準の「戻る」(Pixelの右エッジスワイプや戻るボタン)で、いきなりアプリを
        //  閉じずに、その画面の戻るボタンと同じ動作(前の画面へ)をする。先頭ページ(撮影計画)でだけ
        //  従来どおりアプリ終了に委ねる。
        onBackPressedDispatcher.addCallback(this, object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!goBackOneScreen()) {
                    isEnabled = false                       // これ以上戻り先が無い → 通常の終了へ委ねる
                    onBackPressedDispatcher.onBackPressed()
                    isEnabled = true
                }
            }
        })

        restorePlan()    // 保存済み計画があれば復元、無ければ出荷時計画を表示(再生成しない)
        refreshPlanList()   // 複数計画リスト(分割バー上)を構築
        applyAllMasterDetail()   // 横向きなら一覧のある画面を左右2分割にする
        restoreEdgeState()  // 再起動時: エッジが撮影中なら状態を復元(item9)
        resumePhoneCapture()  // 再起動時: スマホ直結で撮影中だった計画を再開(item2)
        Thread { try { HgeNative.nativePresenceStart() } catch (_: Exception) {} }.start()  // P4: 常駐プレゼンスマップ開始
    }

    // 回転(縦⇄横)。AndroidManifest で configChanges を宣言してあるので、ここへ来るだけで
    //  画面は作り直されない = 開いていた画面も編集中の内容もそのまま残る(2026-08-30 UI依頼)。
    //  文字だけの画面は幅が変わるだけで勝手に流し直されるので何もしなくてよい。
    //  画面幅から寸法を決めて組み立てている所だけ、新しい幅で作り直す。
    //   ・概要スケジュール … 薄明ボックスの幅を画面幅から決めている(equalizeTwilightBoxes)
    //   ・薄明ページ     … ページ幅ぶん横へずらして並べているので現在ページへ置き直す
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        val page = if (::planPager.isInitialized) planPager.current else 0
        if (latestSchedule.isNotEmpty()) { updatePlanDisplay(latestSchedule) }
        applyAllMasterDetail()
        // デバッグログも縦横で並びが変わるので組み直す。選んでいた取得対象は引き継ぐ。
        if (::flipper.isInitialized && flipper.displayedChild == 16) {
            val keep = HashMap<String, Boolean>()
            for ((k, v) in dlogTargets) { keep[k] = v.isChecked }
            buildDebugLogScreen()
            for ((k, v) in dlogTargets) { keep[k]?.let { v.isChecked = it } }
        }
        // 色の設定は縦横で並びが変わるので組み直す。編集途中の色は引き継ぐ(作り直すと保存色に戻るため)。
        if (::flipper.isInitialized && flipper.displayedChild == 9) {
            val keepText = colorTextPicker?.color; val keepBg = colorBgPicker?.color
            buildColorScreen()
            keepText?.let { colorTextPicker?.setColor(it, true) }
            keepBg?.let { colorBgPicker?.setColor(it, true) }
        }
        if (::planPager.isInitialized) { planPager.post { planPager.setCurrent(page, false) } }
    }

    // アプリ再起動時、スマホ直結で撮影中だった計画(/asset/capturing.json)を再開する(item2)。
    // 開始した計画の状態は EV_STATE 通知でUIへ反映される。
    private fun resumePhoneCapture() {
        Thread {
            val n = HgeNative.nativeResumeCapture()
            if (n > 0) runOnUiThread { refreshPlanList(); updateReadOnly() }
        }.start()
    }

    // アプリ再起動時、ネットワーク上のエッジ端末が撮影中なら状態を復元する(§7.3.3/§8)。
    private fun restoreEdgeState() {
        Thread {
            val js = HgeNative.nativeEdgeSearch(2500)
            val found = ArrayList<Edge>()
            try {
                val arr = JSONArray(js)
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    found.add(Edge(o.optString("edgeName"), o.optString("ip"), o.optInt("port", 50506)))
                }
            } catch (_: Exception) {}
            // ローカル計画一覧(id)。各エッジに planId 別に問い合わせ、そのエッジで走行中の“全”計画を復元する。
            // 従来は集約(g_plan.name=最後に撮影した1件)で1台しか復元できず、1エッジ複数カメラの2台目が
            // “開始前アイコン”のまま取り残されていた(エッジ撮影中なのにスマホ開始前)。per-plan で解消する。
            val localPlans = ArrayList<String>()
            try {
                val pa = JSONArray(HgeNative.nativeListPlans())
                for (k in 0 until pa.length()) { val po = pa.optJSONObject(k) ?: continue; val id = po.optString("id"); if (id.isNotEmpty()) localPlans.add(id) }
            } catch (_: Exception) {}
            runOnUiThread { pruneOrphanPlanEdges(localPlans.toSet()) }   // 消えた計画のエッジ割当を掃除
            for (ed in found) {
                runOnUiThread { registerDiscoveredEdge(ed.name, ed.ip, ed.port); refreshEdgeSpinner() }   // 発見したエッジは撮影有無に関わらず登録
                for (pid in localPlans) {
                    val pj = try { HgeNative.nativeEdgeProgress(ed.addr(), ed.port, pid) } catch (_: Exception) { "" }
                    if (pj.isEmpty()) continue
                    val po = try { JSONObject(pj) } catch (_: Exception) { JSONObject() }
                    val st = po.optInt("state", HgeNative.ST_IDLE)
                    val active = st == HgeNative.ST_CAPTURING || st == HgeNative.ST_SEARCHING || st == HgeNative.ST_WAITING ||
                                 st == HgeNative.ST_NOCAMERA || st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_STOPPING
                    if (!active) continue   // このエッジはこの計画を走らせていない(IDLE)
                    val disc = (st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_NOCAMERA)
                    val wait = (st == HgeNative.ST_WAITING || st == HgeNative.ST_SEARCHING)
                    runOnUiThread {
                        setPlanEdgeName(pid, ed.name)   // この計画のエッジ担当を復元(以降の停止/ポールに使う)
                        when { disc -> disconnectedPlans.add(pid); wait -> waitingPlans.add(pid); else -> { capturingPlans.add(pid); startBlink() } }
                        rememberProgress(pid, po)   // 復元でも枚数を控える(選び直した瞬間に出せる)
                        refreshCaptureStatusForCurrent()
                        refreshPlanList(); updateReadOnly(); ensureEdgePoll()
                    }
                    // 複数エッジ/複数計画が並行撮影の場合もあるため continue で全て復元する。
                }
            }
        }.start()
    }

    // Entity が持つ計画(保存済み or 出荷時)の時刻・内容を画面に反映する。
    private fun restorePlan() {
        val sched = HgeNative.nativeScheduleJson()
        var ok = false
        try {
            val o = JSONObject(sched)
            fmtIso.parse(o.optString("start"))?.let { startCal.time = it; ok = true }
            fmtIso.parse(o.optString("end"))?.let { endCal.time = it }
        } catch (_: Exception) {}
        if (!ok) {   // フォールバック: 現在〜2時間後で再生成
            endCal.timeInMillis = startCal.timeInMillis
            endCal.add(Calendar.HOUR_OF_DAY, 2)
            updateTimeButtons(); pushTimesToEntity(); return
        }
        updateTimeButtons()
        latestSchedule = sched
        updatePlanDisplay(sched)
        capturePlanBaseline()
    }

    // 「変更の取り消し」用のベースライン(計画/画面に入った時点の cs JSON)を取り直す。
    // 計画切替(currentPlanId setter)・画面再入・起動時に呼ぶ。編集と直列化(planExec)して安全に取得。
    private fun capturePlanBaseline() {
        if (!::resetButton.isInitialized) return
        planExec.execute {
            val j = try { HgeNative.nativeGetPlanJson() } catch (e: Exception) { "" }
            runOnUiThread { planBaseline = j; if (::resetButton.isInitialized) setCancelEnabled(resetButton, false) }
        }
    }

    private fun bindViews() {
        flipper = findViewById(R.id.flipper)
        startDate = findViewById(R.id.plan_startDate)
        startTime = findViewById(R.id.plan_startTime)
        endDate = findViewById(R.id.plan_endDate)
        endTime = findViewById(R.id.plan_endTime)
        resetButton = findViewById(R.id.plan_resetButton)
        placeText = findViewById(R.id.plan_placeText)
        latlngText = findViewById(R.id.plan_latlngText)
        // 撮影場所をタップ → 入力方法の選択(テキスト貼り付け / 地図から選択)。撮影中は編集不可。
        val placeClick = View.OnClickListener { if (!planReadOnly) showPlaceEditChooser() }
        placeText.setOnClickListener(placeClick)
        latlngText.setOnClickListener(placeClick)
        findViewById<TextView>(R.id.plan_placeLabel).setOnClickListener(placeClick)   // 見出しを押しても入れる
        cameraText = findViewById(R.id.plan_cameraText)
        lensText = findViewById(R.id.plan_lensText)
        intervalText = findViewById(R.id.plan_intervalText)
        npfText = findViewById(R.id.plan_npfText)
        landscapeCheck = findViewById(R.id.plan_landscape)
        syncShotCheck  = findViewById(R.id.plan_syncShot)
        subCamRow      = findViewById(R.id.plan_subCamRow)
        subCamText     = findViewById(R.id.plan_subCamText)
        planOverview = findViewById(R.id.plan_overviewContainer)
        planPager = findViewById(R.id.plan_pager)
        planFormScroll = findViewById(R.id.plan_formScroll)
        planListScroll = findViewById(R.id.plan_listScroll)
        planPager.onPageChanged = { updatePagerTitle() }
        planListContainer = findViewById(R.id.plan_listContainer)
        captureStatus = findViewById(R.id.plan_captureStatus)
        edgeSpinner = findViewById(R.id.plan_edgeSpinner)
        capName = findViewById(R.id.cap_nameText)
        capGear = findViewById(R.id.cap_gearText)
        capDir = findViewById(R.id.cap_dirText)
        capState = findViewById(R.id.cap_stateText)
        capProgress = findViewById(R.id.cap_progressText)
        capCaptured = findViewById(R.id.cap_capturedText)
        capSchedule = findViewById(R.id.cap_scheduleContainer)
        capStopButton = findViewById(R.id.cap_stopButton)
        planMenu = findViewById(R.id.plan_menu)
    }

    // 【ヘッダはホームとメニューの2つ(2026-09-05 UI依頼)】戻るアイコンは廃止した。
    //  メニューから設定画面へ入ったあと撮影計画へ戻るのに操作が多かったため、
    //  どの画面からも1回で「撮影計画(ホーム)」へ行けるようにする。
    //  1つ戻る操作は**端末の戻るキー**に任せる(goBackOneScreen はそのまま残している)。
    //
    //  画面を離れるときの後始末(保存・監視の停止)は行き先で変わらないので、
    //  leaveXxx(dest) に行き先だけ渡す形にした。dest は 0=撮影計画 / 4=メニュー。
    private val kScreenHome = 0
    private val kScreenMenu = 4

    // ヘッダのホーム/メニューを1か所で配線する。押されたら行き先(0/4)を渡して呼ぶ。
    // エッジ端末書き込み画面(別の画面部品)から「ホーム」で戻ってきたときの引き継ぎ。
    //  あちらは finish() で呼び出し元(メニュー)へ戻るだけなので、印を置いてもらって
    //  こちらで撮影計画を出す。
    companion object { @JvmStatic var goHomeOnResume = false }

    override fun onResume() {
        super.onResume()
        if (goHomeOnResume) { goHomeOnResume = false; if (::flipper.isInitialized) gotoScreen(kScreenHome) }
    }

    private fun wireHeader(homeId: Int, menuId: Int, onLeave: (Int) -> Unit) {
        findViewById<ImageView>(homeId)?.setOnClickListener { onLeave(kScreenHome) }
        findViewById<ImageView>(menuId)?.setOnClickListener { onLeave(kScreenMenu) }
    }

    private fun gotoScreen(dest: Int) {
        flipper.displayedChild = dest
        if (dest == kScreenMenu) { buildGearMenu() } else { capturePlanBaseline() }
    }

    // 項目I: 現在の画面に応じて「戻る」を実行する。各画面の戻るボタンと同じ動作にする。
    //  戻り先があれば true、先頭ページ(=これ以上戻れない)なら false を返す。
    //  ViewFlipper index: 0 撮影計画 / 1 撮影中 / 2 ccmメニュー(未使用) / 3 ccm編集 /
    //   4 メニュー / 5 所持カメラ / 6 カメラ追加 / 7 所持レンズ / 8 レンズ追加 / 9 色 /
    //   10 露出平滑化 / 11 撮影場所 / 12 カメラ予約表 / 13 操作履歴 / 14 撮影レポート / 15 エッジ端末設定
    private fun goBackOneScreen(): Boolean {
        if (!::flipper.isInitialized) return false
        when (flipper.displayedChild) {
            0 -> return false                                            // 撮影計画(先頭)→ アプリ終了に委ねる
            1 -> { flipper.displayedChild = 0 }                          // 撮影中 → 撮影計画
            2 -> { flipper.displayedChild = 4; buildGearMenu() }         // ccmメニュー(未使用)→ メニュー
            3 -> { stopDirtyWatch(); persistCcmEdit(); flipper.displayedChild = if (editingPlanCcm) 0 else 4 }
            4 -> { flipper.displayedChild = 0; capturePlanBaseline() }   // メニュー → 撮影計画
            5 -> leaveCameraList()
            6 -> leaveCameraAdd(-1)      // 追加画面 → 元の所持カメラ一覧
            7 -> leaveLensList()
            8 -> leaveLensAdd(-1)        // 追加画面 → 元の所持レンズ一覧
            9 -> leaveColorScreen()
            10 -> leaveSmoothingScreen()
            11 -> leavePlacesList()
            12 -> { flipper.displayedChild = 4; buildGearMenu() }        // カメラ予約表 → メニュー
            13 -> { flipper.displayedChild = 4; buildGearMenu() }        // 操作履歴 → メニュー
            15 -> { flipper.displayedChild = 4; buildGearMenu() }        // エッジ端末設定 → メニュー
            // デバッグログ。取得中は戻らせない(途中で画面を捨てると、どこまで取れたか
            //  分からなくなる)。中断してから戻ってもらう。
            16 -> { if (dlogBusy) Toast.makeText(this, "取得中です。中断してから戻ってください", Toast.LENGTH_SHORT).show()
                    else { flipper.displayedChild = 4; buildGearMenu() } }
            else -> { flipper.displayedChild = 0 }
        }
        return true
    }

    private fun wireListeners() {
        wireSplitFrames()                       // 一覧型7画面の分割バーをまとめて配線する
        // 項目11: 計画1ページ目の方向/仰角ウィジェットは廃止(設定は撮影シミュレーション画面で行う)。
        startDate.setOnClickListener { pickDate(startCal) }
        startTime.setOnClickListener { pickTime(startCal) }
        // 終了日は自動決定なのでタップしても何もしない(2026-08-08 UI依頼)。
        endTime.setOnClickListener { pickTime(endCal) }
        // 旧「リセット」→「変更の取り消し」(他画面と同じ)。画面/計画に入った時点(planBaseline)へ戻す。
        styleCancelButton(resetButton)
        setCancelEnabled(resetButton, false)
        resetButton.setOnClickListener {
            if (planReadOnly) return@setOnClickListener               // 撮影中は編集不可
            val base = planBaseline
            if (base.isEmpty()) return@setOnClickListener
            planExec.execute {
                HgeNative.nativeSetPlanJson(base)                     // g_plan をベースラインへ戻す
                HgeNative.nativeSavePlan()                            // ファイルも戻す
                HgeNative.nativeSelectPlan(currentPlanId)             // 再読込→EV_SCHEDULE でUI更新
            }
        }
        // 「この撮影計画を保存」ボタンは廃止。編集は各操作でその場保存され、画面移動/撮影開始/
        // 他計画の選択でも保存される(他画面と同じ自動保存)。
        handler.postDelayed(planDirtyWatch, 800)                     // 「変更の取り消し」ボタンの dirty 監視開始
        capStopButton.setOnClickListener {
            // 項目6c: スマホからの停止は、エッジ担当なら停止に加えてエッジからも削除しロック解除する。
            stopPlanFromPhone(currentPlanId)
            flipper.displayedChild = 0
        }
        // スピナーでエッジを選ぶと、表示中の計画にその選択を保存する(計画ごとにエッジを指定)。
        //  ユーザーがスピナーに触れた時だけ保存を許す。プログラムによる同期(refreshEdgeSpinner)で届く
        //  通知は常に無視する — これが「別計画へエッジ割当が漏れる」不具合の根治。
        edgeSpinner.setOnTouchListener { _, _ -> edgeSpinnerUserTouched = true; false }
        // 見出し「エッジ端末」を押しても選択に入れる。スピナーを直接押したときと
        // 同じ扱いにしないと、選んだ値が保存されない(上のガードで弾かれる)。
        findViewById<TextView>(R.id.plan_edgeLabel).setOnClickListener {
            if (edgeSpinner.isEnabled) { edgeSpinnerUserTouched = true; edgeSpinner.performClick() }
        }
        edgeSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, pos: Int, idl: Long) {
                if (!edgeSpinnerUserTouched) return           // プログラム由来の通知(=表示の同期)。保存しない
                edgeSpinnerUserTouched = false
                // 保存先はスピナーが表示している計画。念のため選択中の計画と一致する時だけ書く(不一致=表示が追従前)。
                val target = edgeSpinnerPlanId
                val nm = if (pos in 1..edgeSpinnerEdges.size) edgeSpinnerEdges[pos - 1].name else ""
                if (target.isNotEmpty() && target == currentPlanId) { setPlanEdgeName(target, nm) }
                // 【2026-08-06】保存の成否によらず、最後に「実際に保存されている値」を表示へ戻す。
                //  上のガードで弾かれると保存されないのに選んだ表示だけが残り、
                //  「Edje00 と出ているのにスマホ直結で撮影が始まる」という気づけない食い違いになる
                //  (2026-08-05 実機で発生)。表示が実態と一致していれば、その場で気づいて選び直せる。
                showStoredEdgeOnSpinner(target, nm)
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }
        // 撮影計画リスト(分割バー上)の分割バー。
        // 撮影周期(タップでキーボード入力)。最小未満は警告。
        intervalText.setOnClickListener { editInterval() }
        // 横向き(ランドスケープ)。
        landscapeCheck.setOnCheckedChangeListener { _, checked ->
            if (suppressLandscape) return@setOnCheckedChangeListener
            planExec.execute { HgeNative.nativeSetPlanLandscape(if (checked) 1 else 0) }
        }
        // 同期撮影(2026-08-25)。ONで追加カメラの行を出す。
        syncShotCheck.setOnCheckedChangeListener { _, checked ->
            if (suppressSyncShot) return@setOnCheckedChangeListener
            subCamRow.visibility = if (checked) View.VISIBLE else View.GONE
            planExec.execute {
                HgeNative.nativeSetPlanSyncShot(if (checked) 1 else 0)
                val sched = HgeNative.nativeScheduleJson()
                runOnUiThread { latestSchedule = sched; updatePlanDisplay(sched) }
            }
        }
        // 追加カメラ。見出し・内容どちらのタップでも複数選択に入る(カメラ/レンズと同じ)。
        subCamText.setOnClickListener { choosePlanSubCameras() }
        findViewById<TextView>(R.id.plan_subCamLabel).setOnClickListener { choosePlanSubCameras() }
        // センサー/レンズ定数の変更(機材リストに無い値の参考用)。
        // 撮影制御方法の編集ボタン(全種・固定配置)をスケジュールの下に構築する。
        buildCcmEditButtons()
        // メニュー(plan_menu→600.メニュー)。帯付きの一覧から各画面へ分岐。
        wireHeader(R.id.cap_home, R.id.cap_menu) { gotoScreen(it) }
        planMenu.setOnClickListener {
            if (tplMode) { leaveTemplates { openGearMenu() } } else { openGearMenu() }
        }
        // ひな形のときだけ見えるホーム。元の計画へ戻してから撮影計画を出す。
        findViewById<ImageView>(R.id.plan_home).setOnClickListener {
            if (tplMode) { leaveTemplates { gotoScreen(kScreenHome) } }
        }
        // メニュー画面にはホームだけ(メニューの中にメニューは要らない)
        findViewById<ImageView>(R.id.gmenu_home).setOnClickListener { gotoScreen(kScreenHome) }
        // 650 カメラ予約表(項目17)。戻る/メニューどちらもメニューへ戻す。
        wireHeader(R.id.reserve_home, R.id.reserve_menu) { gotoScreen(it) }
        // 660 操作履歴(項目9)
        wireHeader(R.id.history_home, R.id.history_menu) { gotoScreen(it) }
        // 670 撮影レポート。読むだけの画面なので離脱時に保存するものは無い。
        wireHeader(R.id.report_home, R.id.report_menu) { gotoScreen(it) }
        // 8.2 エッジ端末設定(2026-08-08 UI依頼で画面化)
        wireHeader(R.id.edge_home, R.id.edge_menu) { stashEdgeForm(); gotoScreen(it) }
        wireHeader(R.id.dlog_home, R.id.dlog_menu) { dest ->
            // 取得中は戻らせない(端末の戻るキーと同じ扱い)。
            if (dlogBusy) Toast.makeText(this, "取得中です。中断してから移動してください", Toast.LENGTH_SHORT).show()
            else { gotoScreen(dest) }
        }
        findViewById<Button>(R.id.history_clear).setOnClickListener {
            AlertDialog.Builder(this)
                .setTitle("履歴削除")
                .setMessage("操作履歴をすべて削除しますか？")
                .setPositiveButton("削除する") { _, _ ->
                    try { histFile().writeText("[]") } catch (_: Exception) {}
                    buildHistory()
                }
                .setNegativeButton("やめる", null)
                .show()
        }
        // 620 所持カメラ(戻る/メニューで離脱時に自動保存)
        wireHeader(R.id.cameralist_home, R.id.cameralist_menu) { leaveCameraList(it) }
        // 622 カメラ追加(離脱時にチェックを追加 / 取消でチェック解除)
        wireHeader(R.id.cameraadd_home, R.id.cameraadd_menu) { leaveCameraAdd(it) }
        findViewById<Button>(R.id.cameraadd_cancel).setOnClickListener { checkedCamAdd.clear(); buildCameraAdd() }
        // 630 所持レンズ
        wireHeader(R.id.lenslist_home, R.id.lenslist_menu) { leaveLensList(it) }
        // 632 レンズ追加
        wireHeader(R.id.lensadd_home, R.id.lensadd_menu) { leaveLensAdd(it) }
        findViewById<Button>(R.id.lensadd_cancel).setOnClickListener { checkedLensAdd.clear(); buildLensAdd() }
        // 640 撮影場所リスト(§7.9)。戻る/メニューで離脱時に自動保存。
        wireHeader(R.id.places_home, R.id.places_menu) { leavePlacesList(it) }
        // 撮影計画(330)のカメラ/レンズをタップで所持から選択する。
        cameraText.setOnClickListener { choosePlanCamera() }
        lensText.setOnClickListener { choosePlanLens() }
        // 見出しを押しても選択に入れる(撮影場所/エッジ端末と揃える)。
        findViewById<TextView>(R.id.plan_cameraLabel).setOnClickListener { choosePlanCamera() }
        findViewById<TextView>(R.id.plan_lensLabel).setOnClickListener { choosePlanLens() }
        findViewById<Button>(R.id.cmenu_night).setOnClickListener { openCcmEdit("night") }
        findViewById<Button>(R.id.cmenu_sunrise).setOnClickListener { openCcmEdit("sunrise") }
        findViewById<Button>(R.id.cmenu_sunset).setOnClickListener { openCcmEdit("sunset") }
        findViewById<Button>(R.id.cmenu_day).setOnClickListener { openCcmEdit("day") }
        // 撮影制御方法の編集は、計画から開いたときも初期値から開いたときも同じ画面。
        //  ホーム/メニューのどちらを押したかで行き先を決める(以前は開いた経路で決めていた)。
        wireHeader(R.id.edit_home, R.id.edit_menu) { stopDirtyWatch(); persistCcmEdit(); gotoScreen(it) }
        findViewById<Button>(R.id.edit_save).visibility = View.GONE   // 取消はエディタ先頭行へ移動
        // 色はメニュー「色の設定」(システム共通)で設定する(per-ccm色は廃止)。
        wireHeader(R.id.color_home, R.id.color_menu) { leaveColorScreen(it) }
        // 露出平滑化(630)。戻る/メニューで保存して離脱。取り消しで保存値から再読込。
        wireHeader(R.id.smooth_home, R.id.smooth_menu) { leaveSmoothingScreen(it) }
        findViewById<Button>(R.id.smooth_cancel).setOnClickListener { loadSmoothingScreen(); resetDirtyBaseline() }
        setupValueSlider(R.id.smooth_hyst_seek, 20) {
            findViewById<TextView>(R.id.smooth_hyst_val).text = String.format("%.1fev", seekToHyst(it))
        }
        setupValueSlider(R.id.smooth_ma_seek, 10) {
            findViewById<TextView>(R.id.smooth_ma_val).text = "${it}frame"
        }
        // 初期値プリセット一覧の分割バー(620と同挙動)
        // スライダーの値ラベル更新(露出スライダーと形を統一するため Material Slider・仕様8)
        setupValueSlider(R.id.edit_alt_seek, 14, gradient = true) {
            val deg = seekToAlt(it)
            findViewById<TextView>(R.id.edit_alt_val).text = altLabel(deg)
            updateAltTimes(deg.toInt())
        }
        setupValueSlider(R.id.edit_ev_seek, 30, gradient = true) {
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f ev", seekToEv(it))
        }
        setupValueSlider(R.id.edit_postev_seek, 30, gradient = true) {
            findViewById<TextView>(R.id.edit_postev_val).text = String.format("%+.1f ev", seekToEv(it))
        }
        setupValueSlider(R.id.edit_preev_seek, 30, gradient = true) {
            findViewById<TextView>(R.id.edit_preev_val).text = String.format("%+.1f ev", seekToEv(it))
        }
        setupValueSlider(R.id.edit_hyst_seek, 20) {
            findViewById<TextView>(R.id.edit_hyst_val).text = hystLabel(it)
        }
        setupValueSlider(R.id.edit_ma_seek, 10) {
            findViewById<TextView>(R.id.edit_ma_val).text = maLabel(it)
        }
        // 朝日/夕日の太陽高度=範囲スライダー(2つまみ)。明暗バー下地・つまみ●。
        findViewById<RangeSlider>(R.id.edit_alt_range).apply {
            valueFrom = 0f; valueTo = 24f; stepSize = 1f
            isTickVisible = false; labelBehavior = LabelFormatter.LABEL_GONE
            setCustomThumbDrawablesForValues(R.drawable.thumb_dot, R.drawable.thumb_dot)
            trackActiveTintList = transparentTint; trackInactiveTintList = transparentTint
            addOnChangeListener { _, _, _ -> updateAltRangeLabels() }
        }
    }

    // --- 撮影制御方法 初期値: メニュー + 方法別エディタ ---

    private fun openCcmMenu() {
        editingPlanCcm = false   // メニュー経由は初期値ccmの編集
        ccmJson = try { JSONObject(HgeNative.nativeGetCcmDefaults()) } catch (e: Exception) { null }
        flipper.displayedChild = 2
    }

    // ============================================================
    //  600.メニュー(帯=大項目 + 遷移項目)
    // ============================================================
    private fun openGearMenu() { buildGearMenu(); flipper.displayedChild = 4 }

    private fun gearBand(box: LinearLayout, title: String) {
        val tv = TextView(this); tv.text = title; tv.textSize = 14f; tv.setTypeface(null, Typeface.BOLD)
        tv.setTextColor(Color.WHITE); tv.setBackgroundColor(Color.parseColor("#5C6BC0"))
        tv.setPadding(dp(12), dp(6), dp(12), dp(6))
        box.addView(tv)
    }
    private fun gearItem(box: LinearLayout, title: String, enabled: Boolean = true, onClick: () -> Unit) {
        val tv = TextView(this); tv.text = title; tv.textSize = 16f
        tv.setPadding(dp(28), dp(12), dp(12), dp(12))
        if (enabled) { tv.setTextColor(Color.BLACK); tv.setOnClickListener { onClick() } }
        else { tv.setTextColor(Color.parseColor("#BBBBBB")) }   // グレー表示(撮影中/開始要求中は不可)
        box.addView(tv)
        box.addView(thinDivider())
    }

    // スライドスイッチ付きのメニュー項目。
    private fun gearSwitchItem(box: LinearLayout, title: String, checked: Boolean, onChange: (Boolean) -> Unit) {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.setPadding(dp(28), dp(6), dp(12), dp(6))
        val tv = TextView(this)
        tv.text = title; tv.textSize = 16f; tv.setTextColor(Color.BLACK)
        tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        tv.gravity = android.view.Gravity.CENTER_VERTICAL
        val sw = android.widget.Switch(this)
        sw.isChecked = checked
        sw.setOnCheckedChangeListener { _, v -> onChange(v) }
        row.addView(tv); row.addView(sw)
        box.addView(row)
        box.addView(thinDivider())
    }

    // --- スマホ⇄エッジの通信路(BLE か Wi-Fi か)。スマホだけが決める ---
    private fun edgeUseBle(): Boolean = hgcPrefs().getBoolean("edgeUseBle", false)
    private fun setEdgeUseBle(on: Boolean) {
        hgcPrefs().edit().putBoolean("edgeUseBle", on).apply()
        EdgeBleLink.close()                 // 経路を変えるので掴んでいた接続は捨てる
        HgeNative.nativeEdgeSetBle(on)
        Toast.makeText(this, if (on) "外部端末とはBLEで通信します" else "外部端末とはWi-Fiで通信します",
                       Toast.LENGTH_SHORT).show()
    }

    // 折りたたみ見出しの項目(▶/▼)。開閉を切り替えるだけで、中身は buildGearMenu が続けて積む。
    private fun gearExpandItem(box: LinearLayout, title: String, open: Boolean, onToggle: () -> Unit) {
        val tv = TextView(this); tv.text = (if (open) "▼ " else "▶ ") + title; tv.textSize = 16f
        tv.setPadding(dp(28), dp(12), dp(12), dp(12))
        tv.setTextColor(Color.BLACK)
        tv.setOnClickListener { onToggle() }
        box.addView(tv)
        box.addView(thinDivider())
    }

    // 色の設定の項目: 現在の色(背景/文字)で四角く囲って表示(設計書イメージ)。
    private fun gearColorItem(box: LinearLayout, typeKey: String, label: String) {
        val t = keyType(typeKey)
        val tv = TextView(this); tv.text = label; tv.textSize = 16f
        tv.setBackgroundColor(ccmColor(t)); tv.setTextColor(ccmTextColor(t))
        tv.setPadding(dp(16), dp(12), dp(16), dp(12))
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.setMargins(dp(12), dp(4), dp(12), dp(4)); tv.layoutParams = lp
        tv.setOnClickListener { openColorSetting(typeKey) }
        box.addView(tv)
    }

    private fun buildGearMenu() {
        val box = findViewById<LinearLayout>(R.id.gmenu_container)
        box.removeAllViews()
        gearBand(box, "撮影計画")
        gearItem(box, "撮影計画") { flipper.displayedChild = 0 }
        gearItem(box, "撮影場所") { openPlacesList() }
        gearItem(box, "撮影計画ひな形") { openTemplates() }
        gearItem(box, "カメラ予約表") { openReserveTable() }   // 項目17
        gearBand(box, "撮影制御方法 初期値")
        // 項目3: 「月の影響への対処」は撮影制御方法初期値から削除。
        gearItem(box, "夜間撮影") { openPresetScreen("night") }
        gearItem(box, "朝日撮影") { openPresetScreen("sunrise") }
        gearItem(box, "夕日撮影") { openPresetScreen("sunset") }
        gearItem(box, "日中撮影") { openPresetScreen("day") }
        // 色は撮影制御方法ごとに持つものなので、帯を立てず初期値の下へ畳んで置く(2026-08-23 UI依頼)。
        gearExpandItem(box, "色の設定", gearColorsOpen) { gearColorsOpen = !gearColorsOpen; buildGearMenu() }
        if (gearColorsOpen) {
            gearColorItem(box, "night", "夜間撮影")
            gearColorItem(box, "sunrise", "朝日撮影")
            gearColorItem(box, "sunset", "夕日撮影")
            gearColorItem(box, "day", "日中撮影")
            gearColorItem(box, "preNight", "夜間前移行")
            gearColorItem(box, "postNight", "夜間後移行")
        }
        gearBand(box, "自動露出")
        gearItem(box, "露出平滑化") { openSmoothingScreen() }
        gearBand(box, "所持機材")
        gearItem(box, "所持カメラ") { openCameraList() }
        gearItem(box, "所持レンズ") { openLensList() }
        gearBand(box, "外部端末")
        // 2026-08-08 UI依頼: 「登録」と「設定」を1画面へ統合した(登録は画面内の
        // 「＋ 新規エッジ端末」から行う)。
        gearItem(box, "端末設定") { openEdgeSettings() }
        // 買ってきたばかりの端末には自分たちのファームが入っていない。OTA も STA での自己更新も
        //  「今動いているファームが受け取って書く」仕組みなので最初の1回には使えず、
        //  USB で焼くここだけが「開封した端末を使える状態にする」道になる(2026-08-26)。
        gearItem(box, "ファームウェア書き換え(USB)") {
            startActivity(Intent(this, EdgeFlashActivity::class.java))
        }
        // 通信路の切替。エッジは常に Wi-Fi と BLE の両方で待ち受けているので、
        // ここを倒すだけで切り替わる(エッジへ知らせる必要は無い)。
        //  ・屋外でエッジが AP のときは BLE にすると SSID を切り替えずに全台と話せる
        //  ・エッジ側にモードを持たせないので、戻せなくなって現地へ行く経路は無い
        gearSwitchItem(box, "外部端末とBLEで通信する", edgeUseBle()) { on -> setEdgeUseBle(on) }
        gearBand(box, "ログ")
        // 撮影中/開始要求中はグレー表示で不可(コピー処理が撮影と競合しないように)。
        // 【撮影中でも開ける(2026-08-29 実機で気づいた)】この画面には性質の違う2つが同居する。
        //  記録する内容 = 次の撮影に効く設定。**撮影の様子がおかしいときこそ入れたい**
        //  ログ取得     = 重いコピー。撮影中は走らせたくない
        //  画面ごと塞ぐと前者ができなくなるので、塞ぐのは取得ボタンだけにする。
        gearItem(box, "デバッグログ") { openDebugLog() }
        // この2つは「撮影計画」ではなく「記録を見る」側なのでログの下へ置く(2026-08-23 UI依頼)。
        gearItem(box, "操作履歴") { openHistory() }            // 項目9
        gearItem(box, "撮影レポート") { openReportList() }     // 670: 撮影1回ぶんの結果と所見

        gearBand(box, "初期化")
        gearItem(box, "出荷時設定に戻す") { confirmFactoryReset() }

        // 版数を一番下に出す(2026-08-08 UI依頼)。どのビルドを使っているかを画面だけで確認できる。
        // major.minor はエッジ端末と一致させる約束なので、食い違っていたらどちらかの書き込み漏れ。
        box.addView(TextView(this).apply {
            text = "バージョン " + appVersionName()
            textSize = 12f
            setTextColor(0xFF9E9E9E.toInt())
            setPadding(dp(16), dp(16), dp(16), dp(8))
        })
    }

    // ================= 出荷時設定に戻す(2026-09-05 UI依頼) =================
    // インストール直後と同じ状態にする。**消すだけ**で、初期状態を作り直す処理は書かない。
    //  ・撮影計画      → loadFixedPlanImpl() が出荷時の固定計画を1件作る
    //  ・撮影場所      → ensurePlaces() が Tokyo を1件作り、seedFirstPlaceFromLocation() が現在地へ差し替える
    //  ・撮影制御方法  → ensurePresets() が型ごと1件と preferredCcm を作る
    //  ・全体設定      → ensureSettings() は空のまま。既定値は読み出し側が持っている
    //  ・所持カメラ/レンズ → 空のまま(インストール直後と同じ。マスタから選び直す)
    //  ・機材マスタ    → 消さない。消しても installBundledIfNewer() が同梱版を戻すだけで、
    //                    残しておけば通信も発生しない
    //
    // 【プロセスを落とすことが必須】Entity は読み込んだ内容をメモリに抱えている
    //  (g_placesLoaded / g_ownedLoaded / g_settingsLoaded / g_presetsLoaded / g_planReady …)。
    //  SharedPreferences も同じ。消しただけで動き続けると**次の保存で書き戻る**ので、
    //  消した直後にプロセスごと終わらせる。
    private fun confirmFactoryReset() {
        if (isCaptureBusy()) {
            Toast.makeText(this, "撮影中は初期化できません。中止してからやり直してください", Toast.LENGTH_LONG).show()
            return
        }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("出荷時設定に戻す")
            .setMessage(
                "このスマホのデータをすべて消して、インストール直後の状態に戻します。\n\n" +
                "・撮影計画と撮影計画ひな形\n" +
                "・撮影場所\n" +
                "・所持カメラ / 所持レンズ\n" +
                "・撮影制御方法の初期値と全体設定\n" +
                "・外部端末の一覧とネットワーク設定\n" +
                "・撮影ログ / 撮影レポート / 操作履歴\n\n" +
                "機材マスタ(カメラ・レンズの一覧)は残します。\n" +
                "外部端末本体の設定と、そこにある撮影計画は消えません。\n\n" +
                "元に戻せません。消したあとアプリを開き直します。")
            .setPositiveButton("消して開き直す") { _, _ -> doFactoryReset() }
            .setNegativeButton("やめる", null)
            .show()
    }

    private fun doFactoryReset() {
        try {
            val base = getExternalFilesDir(null) ?: filesDir
            // master は残す(同梱版が戻るだけなので消す意味が無く、通信も避けられる)
            for (d in listOf("asset", "plan", "log")) {
                java.io.File(base, d).listFiles()?.forEach { it.delete() }
            }
            // **commit を使う**。apply は非同期で、書き終わる前にプロセスを落とすと消えない。
            hgcPrefs().edit().clear().commit()
            getSharedPreferences("gearMaster", MODE_PRIVATE).edit().clear().commit()
        } catch (_: Exception) {
            // 消せなかったものがあっても、残りは消して終了する(中途半端でも次の起動で作り直される)
        }
        Toast.makeText(this, "初期化しました。開き直します", Toast.LENGTH_SHORT).show()
        // トーストが出てから落とす。**必ず落とすこと**(落とさないとメモリから書き戻る)。
        //  落としたあと自分では起動できないので、別プロセスの RestartActivity に任せる。
        handler.postDelayed({ RestartActivity.restart(this) }, 900)
    }

    // このアプリの版数("0.0.x")。build.gradle.kts の versionName をそのまま読む。
    private fun appVersionName(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
    } catch (_: Exception) { "?" }

    // 撮影実行中/開始要求(待機・未検出含む)中か。ログ取得の可否判定に使う。
    private fun isCaptureBusy(): Boolean =
        capturingPlans.isNotEmpty() || waitingPlans.isNotEmpty() || disconnectedPlans.isNotEmpty()

    // 指定した**その1台**のエッジ端末が今カメラを使っているか(撮影中/開始要求中)。
    //  ネットワーク設定の変更を止めるのは、その端末とカメラの回線が切れて撮影が壊れるから。
    //  別の端末が撮影していても、この端末を触る妨げにはならない(2026-08-17 指示。Edge00が
    //  撮影中というだけで、何もしていない Edge01 の設定まで変更できなくなっていた)。
    //  エッジ名が空の計画=スマホ直結なので、どのエッジとも無関係として数えない。
    private fun isEdgeCaptureBusy(edgeName: String): Boolean {
        if (edgeName.isEmpty()) return false
        return (capturingPlans + waitingPlans + disconnectedPlans).any { planEdgeName(it) == edgeName }
    }

    // ---------- デバッグログ(2026-08-29 UI依頼。旧「ログ取得」をダイアログから画面へ) ----------
    //
    // 【なぜ画面にしたか】ダイアログでは、取る対象を選べず、進み具合も1行しか出せなかった。
    //  端末が増えると「どれを取っているのか」「どこで失敗したのか」が分からない。
    //  ・何を採るか(撮影ログ/バッテリログ)は**次の撮影に効く設定**なので、取得とは別物として置く
    //  ・どの端末から採るかを選べるようにする(既定は全部)
    //  ・取得中は中断できるようにする。エッジが増えると数分かかることがある
    private var dlogBusy = false            // 取得中(この間は他の操作を止める)
    private var dlogAbort = false           // 中断の要求
    private var dlogRunBtn: Button? = null
    private var dlogProgress: TextView? = null
    // "" = スマホ、それ以外はエッジ名。**並び順を保つ**(LinkedHashMap)。
    //  ただの HashMap だと取得の順が画面の並びと食い違い、どこまで進んだか読みにくい。
    private val dlogTargets = LinkedHashMap<String, CheckBox>()
    private var dlogShotCb: CheckBox? = null


    // 採る/採らないの設定。**既定はすべて採らない**(量が多く、肝心の出来事が埋もれるため)。
    //
    // 【端末ごとに持つ(2026-08-29 UI依頼)】以前は1組の設定を全端末へ配っていた。
    //  ところが困るのはたいてい1台だけで、全部の端末で記録を増やす理由がない。
    //  スマホは自分で撮ることもあるので**撮影ログだけ**持つ。電池と STACK/HEAP は
    //  エッジの話なので、エッジ端末設定の画面で端末ごとに入れてもらう。
    private fun logOptShot() = hgcPrefs().getBoolean("logShot", false)
    private fun setLogOptShot(on: Boolean) {
        hgcPrefs().edit().putBoolean("logShot", on).apply()
        applyLogOptsToSelf()
    }
    // スマホ自身の記録に効かせる。起動時と変更時に呼ぶ。効くのは撮影ログだけ。
    private fun applyLogOptsToSelf() {
        try { HgeNative.nativeSetLogOptions(logOptShot()) } catch (_: Exception) {}
    }

    // エッジ1台ぶんのログ設定。prefs の el_<端末名> に {"shot","batt","sys"}。
    private class EdgeLogOpt(var shot: Boolean = false, var batt: Boolean = false, var sys: Boolean = false)

    private fun edgeLogKey(name: String) = "el_" + name

    private fun loadEdgeLogOpt(name: String): EdgeLogOpt {
        val c = EdgeLogOpt()
        if (name.isEmpty()) return c
        try {
            val o = JSONObject(hgcPrefs().getString(edgeLogKey(name), "") ?: "")
            c.shot = o.optBoolean("shot", false)
            c.batt = o.optBoolean("batt", false)
            c.sys  = o.optBoolean("sys",  false)
        } catch (_: Exception) {}
        return c
    }

    private fun saveEdgeLogOpt(name: String, c: EdgeLogOpt) {
        if (name.isEmpty()) return
        try {
            val o = JSONObject().put("shot", c.shot).put("batt", c.batt).put("sys", c.sys)
            hgcPrefs().edit().putString(edgeLogKey(name), o.toString()).apply()
        } catch (_: Exception) {}
        sendEdgeLogOpt(name)
    }

    // その端末へ設定を送る。エッジは不揮発へ残さないので、スイープでも毎回送り直す。
    private fun sendEdgeLogOpt(name: String) {
        val ed = edges.firstOrNull { it.name == name } ?: return
        if (!ed.reachable()) return
        val c = loadEdgeLogOpt(name)
        Thread { try { HgeNative.nativeEdgeSendLogOpt(ed.addr(), ed.port, c.shot, c.batt, c.sys) } catch (_: Exception) {} }.start()
    }

    private fun openDebugLog() {
        dlogBusy = false; dlogAbort = false
        buildDebugLogScreen()
        flipper.displayedChild = 16
    }

    private fun buildDebugLogScreen() {
        val box = findViewById<LinearLayout>(R.id.dlog_container)
        box.removeAllViews()
        dlogTargets.clear()

        // 横向きは 左=状況 / 右=操作 に分ける(2026-08-30 UI依頼)。横へ広げただけだと
        //  操作部が上へ積まれて状況が画面外へ押し出され、見えなくなるため。
        val land = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
        val ops: LinearLayout      // 操作(記録の指定・取得・取得する端末)
        val st: LinearLayout       // 状況
        if (land) {
            ops = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            st = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            row.addView(st, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
            row.addView(View(this).apply { setBackgroundColor(0xFF000000.toInt()) },
                        LinearLayout.LayoutParams(dp(1), ViewGroup.LayoutParams.MATCH_PARENT))
            row.addView(ops, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
                        .apply { setMargins(dp(12), 0, 0, 0) })
            box.addView(row, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            // この画面は縦スクロールの中身なので、放っておくと高さが内容の分しかなく、
            //  区切りの線が途中で切れる。fillViewport で中身をスクロール枠の高さまで
            //  引き伸ばし、線を画面の下まで届かせる(内容が長いときは内容に合わせて伸びる)。
            (box.parent as? ScrollView)?.isFillViewport = true
        } else { ops = box; st = box }

        fun heading(t: String, target: LinearLayout = ops) {
            target.addView(TextView(this).apply {
                text = t; setTypeface(null, Typeface.BOLD); textSize = 15f
                setPadding(0, dp(10), 0, dp(4))
            })
        }

        heading("このスマートフォンの記録")
        ops.addView(TextView(this).apply {
            // エッジのぶんはエッジ端末設定にある。ここに全部置くと「どの端末の話か」が
            //  分からなくなるため、持ち主のところへ置く(2026-08-29 UI依頼)。
            text = "次の撮影から効きます。外部端末のログは端末設定で端末ごとに指定します。"
            textSize = 12f; setTextColor(Color.GRAY); setPadding(0, 0, 0, dp(4))
        })
        val shotCb = CheckBox(this).apply {
            text = "撮影ログ (1コマごとの露出・測光)"
            isChecked = logOptShot()
            setOnCheckedChangeListener { _, _ -> setLogOptShot(isChecked) }
        }
        dlogShotCb = shotCb
        ops.addView(shotCb)

        heading("取得")
        val run = blueButton("ログ取得") { if (dlogBusy) { dlogAbort = true } else { startLogFetch() } }
        ops.addView(run); dlogRunBtn = run
        // 撮影中は触らせない(2026-08-29 UI依頼)。設定は次の撮影からしか効かず、取得は重い。
        //  走行中に触れる意味がないので、まとめて止めて理由を出す。
        if (isCaptureBusy()) {
            ops.addView(TextView(this).apply {
                text = "撮影中です。撮影が終わってから操作してください。"
                textSize = 12f; setTextColor(Color.GRAY); setPadding(0, dp(2), 0, 0)
            })
        }

        heading("取得する端末")
        // スマホは常に対象にできる。エッジは**いまオンラインのものだけ**(届かない相手を
        //  選ばせても失敗するだけで、原因が分からなくなる)。既定はすべてチェック。
        val phoneCb = CheckBox(this).apply { text = "スマートフォン"; isChecked = true }
        ops.addView(phoneCb); dlogTargets[""] = phoneCb
        val online = edges.sortedBy { it.name.lowercase() }.filter { edgeOnline[it.name] == true }
        for (e in online) {
            val cb = CheckBox(this).apply { text = e.name; isChecked = true }
            ops.addView(cb); dlogTargets[e.name] = cb
        }
        if (online.isEmpty()) {
            ops.addView(TextView(this).apply {
                text = "(オンラインの外部端末はありません)"
                textSize = 12f; setTextColor(Color.GRAY); setPadding(dp(8), 0, 0, 0)
            })
        }

        heading("状況", st)
        val pg = TextView(this).apply { textSize = 13f; setTextColor(Color.DKGRAY) }
        st.addView(pg); dlogProgress = pg
        setDlogEnabled(!dlogBusy && !isCaptureBusy())
    }

    // 取得中は取得ボタン以外を触らせない。押せることと押せないことを見た目で分ける。
    private fun setDlogEnabled(on: Boolean) {
        dlogShotCb?.isEnabled = on
        for (cb in dlogTargets.values) { cb.isEnabled = on }
        val c = if (on) Color.BLACK else Color.GRAY
        dlogShotCb?.setTextColor(c)
        for (cb in dlogTargets.values) { cb.setTextColor(c) }
        dlogRunBtn?.text = if (dlogBusy) "中断" else "ログ取得"
        // 中断は押せる必要があるので、取得中だけは有効のままにする。
        dlogRunBtn?.isEnabled = dlogBusy || on
    }

    // 選んだ端末からログを集める。進み具合は画面へ書き、終わったら結果を残す。
    private fun startLogFetch() {
        if (dlogBusy) return
        if (isCaptureBusy()) {
            Toast.makeText(this, "撮影中/開始要求中はログ取得できません", Toast.LENGTH_SHORT).show(); return
        }
        // API28以下は公開Downloadsへ直接書くため書込み権限が要る(29+はMediaStoreで不要)。
        if (Build.VERSION.SDK_INT < 29 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE), 4713)
            Toast.makeText(this, "ストレージ権限を許可してからもう一度お試しください", Toast.LENGTH_LONG).show()
            return
        }
        val wantPhone = dlogTargets[""]?.isChecked ?: false
        val wantEdges = dlogTargets.filter { it.key.isNotEmpty() && it.value.isChecked }.keys.toList()
        if (!wantPhone && wantEdges.isEmpty()) {
            Toast.makeText(this, "取得する端末を選んでください", Toast.LENGTH_SHORT).show(); return
        }
        dlogBusy = true; dlogAbort = false
        setDlogEnabled(false)
        val lines = StringBuilder()
        fun say(m: String) = runOnUiThread {
            lines.append(m).append('\n'); dlogProgress?.text = lines.toString()
        }
        Thread {
            var copied = 0
            val errors = StringBuilder()
            try {
                if (wantPhone && !dlogAbort) {
                    say("スマートフォン: コピー中…")
                    val logDir = java.io.File(getExternalFilesDir(null), "log")
                    val phoneLogs = logDir.listFiles { f -> f.isFile && f.name.endsWith(".log") } ?: emptyArray()
                    for (f in phoneLogs) {
                        if (dlogAbort) break
                        try { saveToDownloads("hgclog", f.name, f.readBytes()); copied++ }
                        catch (e: Exception) { errors.append("phone/${f.name} ") }
                    }
                    say("スマートフォン: ${phoneLogs.size} 件")
                }
                for (nm in wantEdges) {
                    if (dlogAbort) break
                    val ed = edges.firstOrNull { it.name == nm } ?: continue
                    val target = ed.addr()
                    if (target.isEmpty()) { say("$nm: 宛先が分かりません"); continue }
                    say("$nm: 一覧を取得中…")
                    val listJson = try { JSONArray(HgeNative.nativeEdgeLogList(target, ed.port)) } catch (e: Exception) { JSONArray() }
                    var n = 0
                    for (k in 0 until listJson.length()) {
                        if (dlogAbort) break
                        val logName = listJson.optString(k); if (logName.isEmpty()) continue
                        say("$nm: $logName")
                        val bytes = fetchEdgeLog(target, ed.port, logName)
                        if (bytes.isNotEmpty()) {
                            try { saveToDownloads("hgclog-" + sanitizeFolder(nm), logName, bytes); copied++; n++ }
                            catch (e: Exception) { errors.append("$nm/$logName ") }
                        }
                    }
                    say("$nm: $n 件")
                }
            } catch (e: Exception) { errors.append("(${e.message}) ") }
            val n = copied
            val stopped = dlogAbort
            runOnUiThread {
                dlogBusy = false; dlogAbort = false
                setDlogEnabled(true)
                val head = if (stopped) "中断しました。" else "完了しました。"
                val tail = if (errors.isEmpty()) "" else "\n取れなかったもの: $errors"
                say(head + "$n 件を Download/hgclog(スマホ)・hgclog-<端末名>(外部端末) へ保存しました。" + tail)
            }
        }.start()
    }


    // エッジのログ name を分割取得して全バイトを返す(4KBチャンクで offset を進める)。
    private fun fetchEdgeLog(target: String, port: Int, name: String): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        var offset = 0; var guard = 0
        while (guard++ < 20000) {
            val chunk = try { HgeNative.nativeEdgeLogRead(target, port, name, offset) } catch (e: Exception) { ByteArray(0) }
            if (chunk.isEmpty()) break
            out.write(chunk); offset += chunk.size
            if (chunk.size < 4096) break   // <CHUNK = EOF
        }
        return out.toByteArray()
    }

    private fun sanitizeFolder(s: String): String = s.replace(Regex("[^A-Za-z0-9._-]"), "_").ifEmpty { "edge" }

    // Download/<folder>/fileName へ bytes を書く。同名は上書き。API29+ は MediaStore(権限不要)、以前は公開Downloads。
    private fun saveToDownloads(folder: String, fileName: String, bytes: ByteArray) {
        if (Build.VERSION.SDK_INT >= 29) {
            val relPath = "Download/$folder"
            // 上書き: MediaStore は同名があると "(1)" を付けるため、既存エントリを先に削除する。
            // MIME=text/plain で .txt が付く環境もあるので .txt 付きも消す。RELATIVE_PATH は末尾スラッシュ付きで保存される。
            try {
                val sel = "${MediaStore.Downloads.RELATIVE_PATH}=? AND ${MediaStore.Downloads.DISPLAY_NAME}=?"
                contentResolver.delete(MediaStore.Downloads.EXTERNAL_CONTENT_URI, sel, arrayOf("$relPath/", fileName))
                contentResolver.delete(MediaStore.Downloads.EXTERNAL_CONTENT_URI, sel, arrayOf("$relPath/", "$fileName.txt"))
            } catch (_: Exception) {}
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                put(MediaStore.Downloads.MIME_TYPE, "text/plain")
                put(MediaStore.Downloads.RELATIVE_PATH, relPath)
            }
            val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: throw java.io.IOException("MediaStore insert failed")
            contentResolver.openOutputStream(uri)?.use { it.write(bytes) } ?: throw java.io.IOException("openOutputStream failed")
        } else {
            @Suppress("DEPRECATION")
            val dir = java.io.File(android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS), folder)
            dir.mkdirs()
            java.io.File(dir, fileName).writeBytes(bytes)   // File.writeBytes は既定で上書き
        }
    }

    // ---------- 8.2 エッジ端末設定(QR+PoP プロビジョニング) ----------
    // エッジが表示する QR {"n":端末名,"pop":乱数} を読み取り、端末識別名/SSID/password を
    // PoP 由来鍵で暗号化して BLE 送信する(送信本体は M3)。ここでは入力とQR読取まで。

    // ---------- 602 色の設定(文字色/背景色。jaredrummler ColorPicker) ----------
    private fun colorTypeName(k: String) = when (k) {
        "night" -> "夜間撮影"; "sunrise" -> "朝日撮影"; "sunset" -> "夕日撮影"; "day" -> "日中撮影"
        "preNight" -> "夜間前移行"; "postNight" -> "夜間後移行"; else -> k
    }
    private fun openColorSetting(typeKey: String) { colorType = typeKey; buildColorScreen(); flipper.displayedChild = 9 }

    private fun buildColorScreen() {
        val t = keyType(colorType)
        findViewById<TextView>(R.id.color_title).text = colorTypeName(colorType) + "の色"
        applyHeaderColor(R.id.color_header, R.id.color_title, t)
        val box = findViewById<LinearLayout>(R.id.color_container); box.removeAllViews()
        // 横向きは縦に2分割して 左=文字の色 / 右=背景の色 に並べる(2026-08-30 UI依頼)。
        //  縦向きは従来どおり上下に積む。
        val land = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
        val lab1 = TextView(this); lab1.text = "文字の色"; lab1.textSize = 14f
        val p1 = com.jaredrummler.android.colorpicker.ColorPickerView(this)
        p1.setAlphaSliderVisible(true); p1.setColor(ccmTextColor(t), true)
        // ピッカー変更で即タイトル文字色に反映(その場で見た目確認)。
        p1.setOnColorChangedListener { c -> findViewById<TextView>(R.id.color_title).setTextColor(0xFF000000.toInt() or (c and 0xFFFFFF)) }
        colorTextPicker = p1
        val lab2 = TextView(this); lab2.text = "背景の色"; lab2.textSize = 14f
        if (!land) lab2.setPadding(0, dp(16), 0, 0)
        val p2 = com.jaredrummler.android.colorpicker.ColorPickerView(this)
        p2.setAlphaSliderVisible(true); p2.setColor(ccmColor(t), true)
        // ピッカー変更で即タイトル背景色に反映。
        p2.setOnColorChangedListener { c -> findViewById<View>(R.id.color_header).setBackgroundColor(0xFF000000.toInt() or (c and 0xFFFFFF)) }
        colorBgPicker = p2
        if (land) {
            val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL
            val c1 = LinearLayout(this); c1.orientation = LinearLayout.VERTICAL
            val c2 = LinearLayout(this); c2.orientation = LinearLayout.VERTICAL
            c1.addView(lab1); c1.addView(p1)
            c2.addView(lab2); c2.addView(p2)
            row.addView(c1, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(c2, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(12), 0, 0, 0) })
            box.addView(row, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        } else {
            box.addView(lab1); box.addView(p1); box.addView(lab2); box.addView(p2)
        }
        // 項目2: 色の設定にも「変更の取り消し」ボタンを新設(dirty 連動。取消=保存色から作り直し)。
        val colorCancel = addCancelButton(box, atTop = true) { buildColorScreen() }
        startDirtyWatch(colorCancel) { "${colorTextPicker?.color ?: 0},${colorBgPicker?.color ?: 0}" }
    }

    // メニューの作り直しは saveColorScreen の保存完了後にやる。
    // ここで同期に呼ぶと、保存と読み直しを待たずに描いて古い色が出る。
    private fun leaveColorScreen(dest: Int = kScreenMenu) { stopDirtyWatch(); saveColorScreen(); gotoScreen(dest) }

    // ---------- 630 露出平滑化(自動露出 全体設定。settings.json) ----------
    private fun openSmoothingScreen() {
        loadSmoothingScreen()
        startDirtyWatch(findViewById(R.id.smooth_cancel)) {
            "${sliderProgress(R.id.smooth_hyst_seek)},${sliderProgress(R.id.smooth_ma_seek)}"
        }
        flipper.displayedChild = 10
    }
    private fun loadSmoothingScreen() {
        val o = try { JSONObject(HgeNative.nativeGetSmoothing()) } catch (e: Exception) { JSONObject() }
        val h = hystToSeek(o.optDouble("hysteresis", 0.5))
        setSliderProgress(R.id.smooth_hyst_seek, h)
        findViewById<TextView>(R.id.smooth_hyst_val).text = String.format("%.1fev", seekToHyst(h))
        val m = o.optInt("movingAverage", 5).coerceIn(0, 10)
        setSliderProgress(R.id.smooth_ma_seek, m)
        findViewById<TextView>(R.id.smooth_ma_val).text = "${m}frame"
    }
    private fun leaveSmoothingScreen(dest: Int = kScreenMenu) { stopDirtyWatch(); saveSmoothingScreen(); gotoScreen(dest) }
    private fun saveSmoothingScreen() {
        val js = JSONObject()
            .put("hysteresis", seekToHyst(sliderProgress(R.id.smooth_hyst_seek)))
            .put("movingAverage", sliderProgress(R.id.smooth_ma_seek))
            .toString()
        Thread { HgeNative.nativeSetSmoothing(js) }.start()
    }

    private fun saveColorScreen() {
        val tp = colorTextPicker ?: return
        val bp = colorBgPicker ?: return
        val all = try { JSONObject(HgeNative.nativeGetColors()) } catch (e: Exception) { JSONObject() }
        val one = JSONObject().put("text", tp.color and 0xFFFFFF).put("bg", bp.color and 0xFFFFFF)
        all.put(colorType, one)
        val js = all.toString()
        // 保存は別スレッド。完了後に読み直してから、色を焼き込んでいる画面を作り直す。
        //  バンド(renderOverview)と薄明ページ(rebuildTwilightPages)は生成時の色を持つので、
        //  キャッシュを更新するだけでは変わらない(計画を編集するまで古い色のままだった)。
        //  メニューの色チップも、ここで作ることで 1回ぶん遅れなくなる。
        Thread {
            HgeNative.nativeSetColors(js)
            runOnUiThread {
                loadColors()
                buildGearMenu()
                buildCcmEditButtons()	// 計画1ページ目の撮影制御方法ボタンも生成時の色を持つ
                if (latestSchedule.isNotEmpty()) { updatePlanDisplay(latestSchedule) }
            }
        }.start()
    }

    // 600メニューから撮影制御方法の初期値を直接編集する(中間メニューを廃止)。
    private fun openInitialCcm(key: String) {
        editingPlanCcm = false
        ccmJson = try { JSONObject(HgeNative.nativeGetCcmDefaults()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        openCcmEdit(key)
    }
    // ============================================================
    //  撮影制御方法の初期値プリセット(型ごとに複数。分割ビュー=上:一覧/下:選択内容)
    // ============================================================
    private fun loadPresets(type: String) {
        presetCcms.clear()
        try { val a = JSONArray(HgeNative.nativeGetCcmPresets(type)); for (i in 0 until a.length()) a.optJSONObject(i)?.let { presetCcms.add(it) } } catch (_: Exception) {}
    }
    private fun presetByName(name: String?): JSONObject? = presetCcms.firstOrNull { it.optString("name") == name }
    private fun preferredTypeCcm(type: String): JSONObject? {
        val arr = try { JSONArray(HgeNative.nativeGetCcmPresets(type)) } catch (e: Exception) { return null }
        val pref = HgeNative.nativeGetPreferredCcm(type); var first: JSONObject? = null
        for (i in 0 until arr.length()) { val o = arr.optJSONObject(i) ?: continue; if (first == null) first = o; if (o.optString("name") == pref) return o }
        return first
    }

    private fun presetListId() = R.id.edit_presetList
    private fun presetScrollId() = R.id.edit_presetScroll

    private fun openPresetScreen(type: String) {
        editingPlanCcm = false
        presetType = type
        loadPresets(type)
        val names = presetCcms.map { it.optString("name") }
        val pref = HgeNative.nativeGetPreferredCcm(type)
        selPresetName = if (pref in names) pref else names.firstOrNull()
        loadEditorOnly()
        buildPresetList(presetListId())
        setInitialSplit(presetListId())
    }

    // 選択中プリセットを ccmJson に積んでエディタを開く(一覧は作り直さない)。
    private fun loadEditorOnly() {
        val p = presetByName(selPresetName) ?: presetCcms.firstOrNull() ?: return
        selPresetName = p.optString("name")
        ccmJson = JSONObject()
        run {
            ccmJson!!.put(presetType, p)
            openCcmEdit(presetType)
        }
    }

    // プリセット一覧。名称は行で直接編集(他の一覧と同じ)。★=優先的な初期値。
    //  【「＋ 新規追加」は押した時点で作る(2026-09-04 UI依頼)】以前はこの行が入力欄で、
    //   名前を打って確定してから作っていた。他の一覧(撮影場所/所持カメラ/所持レンズ)は
    //   押した時点で作って行で名前を直す形なので、そちらへ揃えた。
    private fun buildPresetList(containerId: Int) {
        val prefName = HgeNative.nativeGetPreferredCcm(presetType)
        renderList(ListPane(
            containerId = containerId,
            rows = {
                presetCcms.map { p ->
                    val nm = p.optString("name")
                    ListItem(nm, nm, "", listOf("削除" to { removePreset(nm) }),
                             mark = if (nm == prefName) "★" else "　")
                }
            },
            selected = { selPresetName }, setSelected = { selPresetName = it },
            onSelect = { selectPreset(it) },
            onRename = { orig, nm -> commitRename(orig, nm) },
            // 名前は「今の1件をコピーした」と分かるものにする(addPresetNamed が空きを探す)。
            addLabel = "＋ 新規追加", onAdd = { addPresetNamed(selPresetName ?: "Preset") }))
    }

    private fun rebuildPresetList() { buildPresetList(presetListId()) }
    // 名前を宛先にして書くので、編集中の名前があれば先に確定させる。
    private fun persistCurrentPreset() { commitListNameEdit(presetListId()); persistCcmEdit() }

    private fun selectPreset(name: String) {
        if (name == selPresetName) return   // 同じ行の再タップは編集(フォーカス)のみ
        persistCurrentPreset()
        selPresetName = name
        loadPresets(presetType)
        loadEditorOnly()
        rebuildPresetList()
    }
    private fun commitRename(orig: String, newName: String) {
        val nm = newName.trim()
        if (nm.isEmpty() || nm == orig) return
        if (presetCcms.any { it.optString("name") == nm }) { showNameInUse(nm); rebuildPresetList(); return }  // item5: 重複拒否
        val p = presetCcms.firstOrNull { it.optString("name") == orig } ?: return
        p.put("name", nm)
        HgeNative.nativeSetCcmPreset(presetType, orig, p.toString())
        if (HgeNative.nativeGetPreferredCcm(presetType) == orig) HgeNative.nativeSetPreferredCcm(presetType, nm)
        if (selPresetName == orig) selPresetName = nm
        loadPresets(presetType); rebuildPresetList()
    }
    private fun addPresetNamed(name: String) {
        persistCurrentPreset()
        val base = presetByName(selPresetName)?.let { JSONObject(it.toString()) } ?: JSONObject().put("type", keyType(presetType))
        val names = presetCcms.map { it.optString("name") }
        var nm = name; var n = 1; while (nm in names) { n++; nm = "$name$n" }
        base.put("name", nm)
        HgeNative.nativeSetCcmPreset(presetType, "", base.toString())
        selPresetName = nm
        loadPresets(presetType); loadEditorOnly(); rebuildPresetList()
    }
    private fun removePreset(name: String) {
        if (presetCcms.size <= 1) { Toast.makeText(this, "最後の1件は削除できません", Toast.LENGTH_SHORT).show(); return }
        HgeNative.nativeRemoveCcmPreset(presetType, name)
        loadPresets(presetType)
        if (selPresetName == name) { selPresetName = presetCcms.firstOrNull()?.optString("name"); loadEditorOnly() }
        rebuildPresetList()
    }

    // エディタ内容の先頭に [初期値リストから選択] [優先的な初期値にする] [変更の取り消し(赤)] の行を差し込む。
    // 戻り値=変更の取り消しボタン(dirty 連動の監視に使う)。onPickPreset!=null で「初期値リストから選択」を出す。
    private fun addPresetTopRow(contentId: Int, onCancel: () -> Unit, onPickPreset: (() -> Unit)? = null): Button {
        val box = findViewById<LinearLayout>(contentId)
        box.findViewWithTag<View>("ptop")?.let { box.removeView(it) }
        presetPreferCheck = null
        val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL; row.tag = "ptop"
        if (onPickPreset != null) {
            val pick = Button(this); pick.text = "初期値リストから選択"; pick.isAllCaps = false; pick.textSize = 12f
            pick.setOnClickListener { onPickPreset() }
            row.addView(pick, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        if (!editingPlanCcm) {
            val cb = CheckBox(this); cb.text = "優先的な初期値にする"
            cb.isChecked = selPresetName == HgeNative.nativeGetPreferredCcm(presetType)
            cb.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            cb.setOnCheckedChangeListener { _, c ->
                if (c) { selPresetName?.let { HgeNative.nativeSetPreferredCcm(presetType, it) }; rebuildPresetList() } else cb.isChecked = true
            }
            row.addView(cb); presetPreferCheck = cb
        } else {
            val sp = View(this); sp.layoutParams = LinearLayout.LayoutParams(0, 1, 1f); row.addView(sp)
        }
        val cancel = Button(this); styleCancelButton(cancel)
        cancel.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        cancel.setOnClickListener { onCancel() }
        row.addView(cancel)
        box.addView(row, 0)
        return cancel
    }

    // --- 変更の取り消しボタンの dirty 連動(item8) ---
    // 未変更=グレー無効、変更=赤有効、元に戻すと無効へ。開いた時の baseline と現在値を
    // 比較して状態を更新する(短周期の監視で全ウィジェットに非依存)。
    private var dirtyBtn: Button? = null
    private var dirtyBaseline: String = ""
    private var dirtyProvider: (() -> String)? = null
    private val dirtyWatch = object : Runnable {
        override fun run() {
            val b = dirtyBtn; val p = dirtyProvider ?: return
            if (b != null) { setCancelEnabled(b, p() != dirtyBaseline); handler.postDelayed(this, 300) }
        }
    }
    // 監視開始。provider は「現在の編集内容」を表す文字列(保存JSON等)を返す。
    private fun startDirtyWatch(btn: Button, provider: () -> String) {
        handler.removeCallbacks(dirtyWatch)
        dirtyBtn = btn; dirtyProvider = provider; dirtyBaseline = provider()
        setCancelEnabled(btn, false)
        handler.postDelayed(dirtyWatch, 300)
    }
    private fun stopDirtyWatch() { handler.removeCallbacks(dirtyWatch); dirtyBtn = null; dirtyProvider = null }
    // 取り消しでベースラインへ戻したら無効へ(取り消しハンドラの末尾で呼ぶ)。
    private fun resetDirtyBaseline() { dirtyProvider?.let { dirtyBaseline = it() }; dirtyBtn?.let { setCancelEnabled(it, false) } }
    private fun setCancelEnabled(btn: Button, enabled: Boolean) {
        btn.isEnabled = enabled
        btn.background = androidx.core.content.ContextCompat.getDrawable(
            this, if (enabled) R.drawable.btn_cancel_double else R.drawable.btn_cancel_gray)
        btn.setTextColor(if (enabled) Color.WHITE else 0xFFEEEEEE.toInt())
    }

    // 初期値リストから選択(§7.4.1)。型のプリセット名一覧をポップアップし、選んだ内容を
    // 現在編集中の撮影制御方法に反映する(以後そこから変更してこの計画の設定とする)。
    private fun showPresetPicker(type: String, onApply: (JSONObject) -> Unit) {
        val arr = try { JSONArray(HgeNative.nativeGetCcmPresets(type)) } catch (e: Exception) { JSONArray() }
        val names = ArrayList<String>(); val objs = ArrayList<JSONObject>()
        for (i in 0 until arr.length()) { val o = arr.getJSONObject(i); names.add(o.optString("name")); objs.add(o) }
        if (names.isEmpty()) { Toast.makeText(this, "初期値がありません", Toast.LENGTH_SHORT).show(); return }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("初期値リストから選択")
            .setItems(names.toTypedArray()) { _, which -> onApply(objs[which]) }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // 「変更の取り消し」ボタン共通スタイル: 赤の丸ボタン + 白の内側リング(2重)。文字は白。
    private fun styleCancelButton(btn: Button) {
        btn.text = "変更の取り消し"
        btn.isAllCaps = false
        btn.textSize = 12f
        btn.setTextColor(Color.WHITE)
        btn.background = androidx.core.content.ContextCompat.getDrawable(this, R.drawable.btn_cancel_double)
        btn.backgroundTintList = null
        btn.minWidth = 0; btn.minHeight = 0
        btn.setPadding(dp(20), dp(8), dp(20), dp(8))
    }

    // 縦並びコンテナに「変更の取り消し」ボタンを右寄せで追加する。atTop=true で先頭(分割バー直下)へ。
    private fun addCancelButton(box: LinearLayout, atTop: Boolean = false, onCancel: () -> Unit): Button {
        val btn = Button(this); styleCancelButton(btn)
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.gravity = Gravity.END; lp.topMargin = dp(4); lp.bottomMargin = dp(4)
        btn.layoutParams = lp
        btn.setOnClickListener { onCancel() }
        if (atTop) box.addView(btn, 0) else box.addView(btn)
        return btn
    }

    // エディタ内容をプリセットとして保存する(初期値編集時)。名称は一覧側で編集する。
    private fun savePresetFromEditor(o: JSONObject) {
        val nm = selPresetName ?: return
        o.put("name", nm)
        HgeNative.nativeSetCcmPreset(presetType, nm, o.toString())
    }

    // ============================================================
    //  機材マスタ・所持機材(600/620/622/630/632。データ構造仕様書43 §5.5〜5.9 / §7.6)
    // ============================================================

    // 機材マスタ(カメラ/レンズ)を1日1回だけ公開リポジトリへ見に行く。
    //  ・最初の起動でしか動かない(1日1回)。撮影中に通信を増やさないため
    //  ・失敗は握りつぶす。圏外でもエッジのAPに繋いでいても、手元の一覧で動き続ける
    //  ・取り込みは版(revision)が手元より大きいときだけ。構造(schema)が読めなければ触らない
    private fun startGearMasterCheck(baseDir: java.io.File) {
        if (GearMaster.checkedToday(this)) return
        Thread {
            // 何が起きたかは記録に残す。黙って失敗すると「更新されない」原因を追えない。
            val rev = GearMaster.fetchIfNewer(this, baseDir) { m ->
                android.util.Log.i("GearMaster", m)
            }
            GearMaster.markChecked(this)
            if (rev != null) {
                // 取り込めたら Entity に読み直させる(次に一覧を開いたときから新しくなる)
                runOnUiThread { HgeNative.nativeReloadMaster() }
            }
        }.start()
    }

    // --- 選択状態と詳細編集の参照 ---
    private var selCamera: String? = null               // 620 選択中の所持カメラ名
    private var selLens: String? = null                 // 630 選択中の所持レンズ名
    private val camFields = HashMap<String, EditText>()  // カメラ詳細の入力欄
    private var camAutoInsert: CheckBox? = null
    private var camMeterLv: CheckBox? = null       // ライブビューで測光する(機体ごと)
    private var camAuthBaseline = ""              // 詳細を開いた時点の認証欄(変更検知用)
    private var camEditBaseline = ""              // 詳細を開いた時点の編集欄すべて(ロック判定用)
    private val camLensNames = ArrayList<String>()       // 組み合わせるレンズ(順序=先頭が初期値)
    private var camLensContainer: LinearLayout? = null   // 組み合わせレンズの並べ替えコンテナ
    private val lensRowViews = mutableListOf<View>()
    private val lensGapViews = mutableListOf<View>()
    private var lensDragFrom = -1
    private val lensFields = HashMap<String, EditText>()
    private var lensContact: CheckBox? = null
    private val checkedCamAdd = LinkedHashSet<String>()  // 622 チェック中
    private val checkedLensAdd = LinkedHashSet<String>() // 632 チェック中
    private val expandedMakers = HashSet<String>()       // 632 展開中メーカー

    private fun camArray(json: String): JSONArray = try { JSONArray(json) } catch (e: Exception) { JSONArray() }

    private fun openCameraList() {
        buildCameraList(); buildCameraDetail()
        setInitialSplit(R.id.cameralist_container)
        flipper.displayedChild = 5
    }
    private fun openCameraAdd()  { checkedCamAdd.clear(); buildCameraAdd(); flipper.displayedChild = 6 }
    private fun openLensList()   { buildLensList(); buildLensDetail(); setInitialSplit(R.id.lenslist_container); flipper.displayedChild = 7 }
    private fun openLensAdd()    { checkedLensAdd.clear(); expandedMakers.clear(); buildLensAdd(); flipper.displayedChild = 8 }

    // 分割バーの初期高さ(上=リスト)。リストが短ければ内容ぴったりまで上に詰め、
    // 多い場合(内容が画面の1/4超)は1/4で止める。ほとんどは1件なので上寄せになる。
    // 一覧型の画面(上=一覧 / 分割バー / 下=内容)を、端末の向きに合わせて組み替える。
    //  縦向き … XML のまま。ヘッダ / 一覧 / 分割バー / 内容 を上下に積む(従来と同じ)。
    //  横向き … ヘッダ / [一覧 │ 内容]。間に縦線を入れ、上下の分割バーは隠す。
    // 【なぜコードでやるか】回転は configChanges で受けていて画面を作り直さない。
    //  そのため res/layout-land/ に分けると、一度横向きで開いた形が縦へ戻しても残ってしまう
    //  (2026-08-30 に実際に起きた)。作り直さずに付け替えるこの方法なら向きに必ず追従する。
    private val mdBoxes = HashMap<Int, LinearLayout>()   // listId -> 横向きの横並び箱
    private val mdListH = HashMap<Int, Int>()            // listId -> 縦向きに戻すときの一覧の高さ

    private fun applyMasterDetail(listId: Int, dividerId: Int) {
        val list = findViewById<View>(listId) ?: return
        val divider = findViewById<View>(dividerId) ?: return
        // 一覧が隠れている画面(撮影制御方法の編集で、計画固有のときはプリセット一覧を出さない)は
        //  分割すると左が空になるので縦積みのままにする。
        val land = resources.configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE &&
                   list.visibility == View.VISIBLE
        val box = mdBoxes[listId]
        val mp = ViewGroup.LayoutParams.MATCH_PARENT
        if (land && box == null) {
            val root = divider.parent as? LinearLayout ?: return
            val li = root.indexOfChild(list)
            val di = root.indexOfChild(divider)
            val detail = root.getChildAt(di + 1) ?: return
            if (li < 0 || di < 0) return
            mdListH[listId] = list.layoutParams.height
            root.removeView(detail); root.removeView(list)      // 後ろから外す(番号がずれないように)
            val h = LinearLayout(this)
            h.orientation = LinearLayout.HORIZONTAL
            h.addView(list, LinearLayout.LayoutParams(0, mp, 1f))
            val bar = View(this)
            bar.setBackgroundColor(0xFF000000.toInt())          // 左右の境目(縦の黒い線)
            h.addView(bar, LinearLayout.LayoutParams(dp(1), mp))
            h.addView(detail, LinearLayout.LayoutParams(0, mp, 1f))
            root.addView(h, li, LinearLayout.LayoutParams(mp, 0, 1f))
            divider.visibility = View.GONE
            mdBoxes[listId] = h
        } else if (land && box != null) {
            // すでに分割済み。一覧の出し入れなどで分割バーが表示に戻されることがあるので隠し直す
            //  (撮影制御方法の編集で、初期値編集に切り替えたときに画面下へ帯が残っていた)。
            divider.visibility = View.GONE
        } else if (!land && box != null) {
            val root = box.parent as? LinearLayout ?: return
            val bi = root.indexOfChild(box)
            val detail = box.getChildAt(2) ?: return
            box.removeAllViews(); root.removeView(box)
            root.addView(list, bi, LinearLayout.LayoutParams(mp, mdListH[listId] ?: dp(120)))
            root.addView(detail, bi + 2, LinearLayout.LayoutParams(mp, 0, 1f))   // 分割バーの次へ戻す
            divider.visibility = View.VISIBLE
            mdBoxes.remove(listId)
        }
    }

    // 一覧のある画面をまとめて向きに合わせる。画面はすべて起動時に組み立て済み(ViewFlipper)なので、
    //  表示中かどうかに関わらず全部まとめて掛けてよい。
    // 【「上=一覧 / 分割バー / 下=詳細」の枠は1つの表で回す(2026-09-04 UI依頼)】
    //  同じ3点セット(スクロール/分割バー/入れ物)を持つ画面が7つある。以前は分割バーの配線・
    //  横向きの左右分割・初期の高さを画面ごとに書いていて、追加や修正のたびに書き漏らす形だった。
    //  ここに1行足せば3つとも回る。レイアウトXMLは id が画面ごとに違うので4本のまま
    //  (同じ id を持つ画面を ViewFlipper に並べると findViewById が区別できなくなるため)。
    private class SplitFrame(val scrollId: Int, val dividerId: Int, val containerId: Int)
    private val splitFrames = listOf(
        SplitFrame(R.id.plan_listScroll,       R.id.plan_listDivider,   R.id.plan_listContainer),
        SplitFrame(R.id.cameralist_listScroll, R.id.cameralist_divider, R.id.cameralist_container),
        SplitFrame(R.id.lenslist_listScroll,   R.id.lenslist_divider,   R.id.lenslist_container),
        SplitFrame(R.id.places_listScroll,     R.id.places_divider,     R.id.places_container),
        SplitFrame(R.id.edge_listScroll,       R.id.edge_divider,       R.id.edge_container),
        SplitFrame(R.id.report_listScroll,     R.id.report_divider,     R.id.report_container),
        SplitFrame(R.id.edit_presetScroll,     R.id.edit_presetDivider, R.id.edit_presetList))

    private fun wireSplitFrames() { splitFrames.forEach { setupDivider(it.dividerId, it.scrollId) } }

    private fun applyAllMasterDetail() { splitFrames.forEach { applyMasterDetail(it.scrollId, it.dividerId) } }

    // 一覧の入れ物の id だけで初期の高さを決める(スクロールの id を書き間違えようがない)。
    private fun setInitialSplit(containerId: Int) {
        val f = splitFrames.firstOrNull { it.containerId == containerId } ?: return
        setInitialSplit(f.scrollId, f.containerId)
    }

    private fun setInitialSplit(listId: Int, containerId: Int) {
        val v = findViewById<View>(listId)
        val c = findViewById<View>(containerId)
        // 横向きで左右2分割にした画面は一覧が列の高さいっぱい(match_parent)なので上下比率を使わない。
        //  縦向きは固定 dp(120dp など)なので height > 0 で見分けられる。
        if (v.layoutParams.height <= 0) { return }
        v.post {
            val quarter = resources.displayMetrics.heightPixels / 4
            val content = c.height
            val h = if (content in 1 until quarter) content else quarter
            val lp = v.layoutParams; lp.height = h; v.layoutParams = lp
        }
    }

    // 分割バーのドラッグで上(リスト)の高さを変える。上限=1行が見える、下限=下から1/4。
    private fun setupDivider(dividerId: Int, listId: Int) {
        val divider = findViewById<View>(dividerId)
        val list = findViewById<View>(listId)
        if (list.layoutParams.height <= 0) { return }   // 左右2分割中は上下比率を使わない(上の説明を参照)
        var startY = 0f; var startH = 0
        divider.setOnTouchListener { _, ev ->
            when (ev.action) {
                MotionEvent.ACTION_DOWN -> { startY = ev.rawY; startH = list.height; true }
                MotionEvent.ACTION_MOVE -> {
                    val root = list.parent as ViewGroup
                    val headerH = list.top    // リスト上の余白(ヘッダ等)。計画画面はリストが先頭=0。
                    val minH = dp(48)                                                   // 1行が見える(最初の項目まで上げられる)
                    // 画面3/4より下には下げない上限。
                    val quarterMax = root.height - headerH - divider.height - resources.displayMetrics.heightPixels / 4
                    // リスト内容の実高さ(ScrollViewの子)。これ以上は下げない=リスト最下段で止める(item6)。
                    val contentH = (list as? ScrollView)?.getChildAt(0)?.height ?: list.height
                    var maxH = if (contentH > 0) minOf(quarterMax, contentH) else quarterMax
                    if (maxH < minH) maxH = minH
                    var h = (startH + (ev.rawY - startY)).toInt()
                    if (h < minH) h = minH
                    if (h > maxH) h = maxH
                    val lp = list.layoutParams; lp.height = h; list.layoutParams = lp
                    true
                }
                else -> true
            }
        }
    }

    private fun linkText(text: String, onClick: () -> Unit): TextView {
        val tv = TextView(this); tv.text = text; tv.textSize = 15f
        tv.setTextColor(Color.parseColor("#1565C0"))
        tv.setPadding(dp(4), dp(10), dp(4), dp(10))
        tv.setOnClickListener { onClick() }
        return tv
    }

    // 押せるボタンの見た目を1つに揃える(2026-08-29 UI依頼)。
    //  画面ごとに素の Button を使っていて、端末の既定テーマ次第で角も色もばらついていた。
    //  ここを通せば全部そろう。文字は白、背景は丸い青(btn_blue_round)。
    private fun blueButton(label: String, onClick: () -> Unit): Button {
        val b = Button(this)
        b.text = label
        b.isAllCaps = false                     // 既定で大文字化する端末があるため止める
        // 文字色は状態で変える。無効のとき白のままだと薄い背景に乗って読めない。
        //  無効時の値はテーマ既定(ファーム書き込み画面の素の Button)と同じ(colors.xml 参照)。
        b.setTextColor(android.content.res.ColorStateList(
            arrayOf(intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
            intArrayOf(androidx.core.content.ContextCompat.getColor(this, R.color.btn_disabled_text), Color.WHITE)))
        b.setBackgroundResource(R.drawable.btn_blue_round)
        b.stateListAnimator = null              // 影が付くと角の丸みが目立たなくなる
        b.setPadding(dp(16), dp(8), dp(16), dp(8))
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.setMargins(0, dp(6), 0, 0)
        b.layoutParams = lp
        b.setOnClickListener { onClick() }
        return b
    }

    private fun thinDivider(): View {
        val v = View(this)
        v.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1)
        v.setBackgroundColor(0xFFDDDDDD.toInt())
        return v
    }

    // 一覧の1行(タップで選択。選択中は太字。右にコンテキストメニュー「⋮」)。
    // item5: 名称が既に使われているときのポップアップ(全リスト+分割バー画面で共通)。
    private fun showNameInUse(name: String) {
        AlertDialog.Builder(this).setMessage("「$name」は使用されています")
            .setPositiveButton("OK", null).show()
    }
    // item5: 重複チェック用の現在の名称一覧。
    private fun ownedCameraNames(): List<String> { val a = camArray(HgeNative.nativeGetOwnedCameras()); return (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optString("name") } }
    private fun ownedLensNames(): List<String> { val a = camArray(HgeNative.nativeGetOwnedLenses()); return (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optString("name") } }

    // onRename を渡すと名称をインライン編集できる(タップで選択、確定/フォーカス外で改名)。全分割バー画面で共通。
    // onEditor: 選択行の名前入力欄を作ったときに呼ぶ(確定処理を持っておく側で使う)。
    // mark: 行頭の印(撮影制御方法の初期値の★=優先。空なら場所を取らない)。
    private fun listRow(title: String, sub: String, selected: Boolean, onSelect: () -> Unit,
                        menuItems: List<Pair<String, () -> Unit>>, onRename: ((String) -> Unit)? = null,
                        onEditor: ((EditText) -> Unit)? = null, mark: String = ""): View {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(dp(6), dp(8), dp(6), dp(8))
        if (mark.isNotEmpty()) {
            row.addView(TextView(this).apply {
                text = mark; textSize = 14f; setPadding(dp(2), 0, dp(4), 0)
                setOnClickListener { onSelect() }
            })
        }
        val txt = LinearLayout(this); txt.orientation = LinearLayout.VERTICAL
        txt.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        if (selected && onRename != null) {
            // 選択中のみ名称インライン編集可: フォーカス外/確定で改名(未選択行はタップで選択のみ)。
            val e = EditText(this); e.setText(title); e.isSingleLine = true
            e.textSize = if (selected) 19f else 16f
            e.setTypeface(null, if (selected) Typeface.BOLD else Typeface.NORMAL)
            e.setTextColor(if (selected) Color.BLACK else Color.parseColor("#888888"))
            e.setBackgroundColor(0x00000000); e.setPadding(0, 0, 0, 0)
            e.inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            e.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
            e.setOnClickListener { onSelect() }
            e.setOnFocusChangeListener { _, has -> if (!has) onRename(e.text.toString()) }
            e.setOnEditorActionListener { v, a, _ ->
                if (a == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) {
                    onRename(v.text.toString()); v.clearFocus()
                    (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager).hideSoftInputFromWindow(v.windowToken, 0)
                    true
                } else false
            }
            txt.addView(e)
            onEditor?.invoke(e)
        } else {
            val t = TextView(this); t.text = title
            t.textSize = if (selected) 19f else 16f
            t.setTypeface(null, if (selected) Typeface.BOLD else Typeface.NORMAL)
            t.setTextColor(if (selected) Color.BLACK else Color.parseColor("#888888"))
            txt.addView(t)
            txt.setOnClickListener { onSelect() }
        }
        if (sub.isNotEmpty()) { val s = TextView(this); s.text = sub; s.textSize = 12f; s.setTextColor(Color.GRAY); s.setOnClickListener { onSelect() }; txt.addView(s) }
        row.addView(txt)
        if (menuItems.isNotEmpty()) { row.addView(ctxMenuButton(menuItems)) }
        return row
    }

    // コンテキストメニューボタン(緑ピル ⋮。設計書イメージ)。
    // メニュー内容を「タップした瞬間」に組み立てる版。
    //  固定リスト版は行を作った時点の状態でメニューが凍るため、状態が変わっても反映されず
    //  「出るときと出ないときがある」原因になる(項目2)。状態依存の項目はこちらを使う。
    private fun ctxMenuButtonDynamic(provider: () -> List<Pair<String, () -> Unit>>): View {
        val btn = TextView(this); btn.text = "⋮"; btn.textSize = 18f
        btn.setTextColor(Color.WHITE); btn.gravity = Gravity.CENTER
        btn.setBackgroundResource(R.drawable.ctx_menu_pill)
        btn.setPadding(dp(12), dp(2), dp(12), dp(2))
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.setMargins(dp(6), 0, dp(2), 0); btn.layoutParams = lp
        btn.setOnClickListener { anchor ->
            val items = provider()   // ← タップ時に最新状態で作る
            val pm = PopupMenu(this, anchor)
            items.forEachIndexed { i, mi -> pm.menu.add(0, i, i, mi.first) }
            pm.setOnMenuItemClickListener { mi -> items[mi.itemId].second(); true }
            pm.show()
        }
        return btn
    }

    private fun ctxMenuButton(menuItems: List<Pair<String, () -> Unit>>): View {
        val btn = TextView(this); btn.text = "⋮"; btn.textSize = 18f
        btn.setTextColor(Color.WHITE); btn.gravity = Gravity.CENTER
        btn.setBackgroundResource(R.drawable.ctx_menu_pill)
        btn.setPadding(dp(12), dp(2), dp(12), dp(2))
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.setMargins(dp(6), 0, dp(2), 0); btn.layoutParams = lp
        btn.setOnClickListener { anchor ->
            val pm = PopupMenu(this, anchor)
            menuItems.forEachIndexed { i, mi -> pm.menu.add(0, i, i, mi.first) }
            pm.setOnMenuItemClickListener { mi -> menuItems[mi.itemId].second(); true }
            pm.show()
        }
        return btn
    }

    // ---------- 620 所持カメラ ----------
    private fun buildCameraList(): Unit = renderList(ListPane(
        containerId = R.id.cameralist_container,
        rows = {
            val arr = camArray(HgeNative.nativeGetOwnedCameras())
            (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera") }.map { cam ->
                val name = cam.optString("name")
                // 項目D: 愛称/シリアルは未取得なら「未定義」と出す(SSDPでオンライン取得後に実値が入る)。
                ListItem(name, name,
                    "愛称:" + cam.optString("assignedName").ifEmpty { "未定義" } +
                        "  S/N:" + cam.optString("serial").ifEmpty { "未定義" },
                    listOf(
                        "削除" to {
                            // エッジが計画を持っているカメラは消せない(消すと台帳から資格情報が落ち、
                            //  その計画が単独復帰したときに認証できなくなる)。
                            if (!blockedByEdge(name, "削除")) {
                                dataExec.execute { HgeNative.nativeRemoveOwnedCamera(name)
                                    runOnUiThread { if (selCamera == name) selCamera = null; buildCameraList(); buildCameraDetail()
                                                    pushCameraBookToEdges() } }
                            }
                        },
                        "接続カメラ検索" to { searchAndAddCameras() }))
            }
        },
        selected = { selCamera }, setSelected = { selCamera = it },
        onSelect = { selectCamera(it) },
        // エッジが計画を持っているカメラは名前も変えられない。
        onRename = { orig, nm -> if (!blockedByEdge(orig, "変更")) commitCameraRename(orig, nm) },
        addLabel = "＋ 新規カメラ追加", onAdd = { openCameraAdd() }))

    // マスタに無いカメラを手入力で追加する(レンタル機など)。型番だけ聞き、残りは詳細画面で埋めてもらう。
    private fun promptAddCustomCamera() {
        val et = EditText(this)
        et.hint = "例: EOS R50V"
        et.setSingleLine()
        val wrap = LinearLayout(this)
        wrap.orientation = LinearLayout.VERTICAL
        wrap.setPadding(dp(20), dp(8), dp(20), 0)
        wrap.addView(et)
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("一覧に無いカメラを追加")
            .setMessage("カメラの型番を入れてください。センサーサイズとISO/シャッター速度の範囲は仮の値が入るので、追加したあと詳細画面で実機に合わせてください。")
            .setView(wrap)
            .setPositiveButton("追加") { _, _ ->
                val nm = et.text.toString().trim()
                if (nm.isEmpty()) { Toast.makeText(this, "型番を入れてください", Toast.LENGTH_SHORT).show() }
                else { addCustomCamera(nm) }
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // 手入力のカメラを1台作る。nativeSetOwnedCameraDetail は「その名前が無ければ新規作成」なので、
    //  専用のネイティブ関数は要らない。ただし**名称は所持カメラ一覧のキー**で、既存と衝突すると
    //  追加ではなく上書きになってしまうため、ここで一意化する。
    private fun addCustomCamera(rawName: String) {
        Thread {
            val arr = camArray(HgeNative.nativeGetOwnedCameras())
            val used = (0 until arr.length())
                .mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera")?.optString("name") }.toSet()
            var nm = rawName
            var n = 2
            while (nm in used) { nm = "$rawName ($n)"; n++ }
            // model は接続したカメラと自動で紐づけるための照合キー。表示名(name)は後で変えられるので、
            //  照合が壊れないよう model には入力した型番をそのまま入れる。
            val o = JSONObject()
                .put("name", nm)
                .put("model", rawName)
                .put("maker", "Canon")        // 現状の対応はCCAPI(Canon)のみ。違うなら詳細画面で直す
                .put("sensorSize", 0.0)
                .put("sensorSizeV", 0.0)
                .put("sensorPixel", 0)
                // 仮の範囲。空にすると計画側で使えないので入れておく。実機に合わせて詳細画面で直す。
                .put("isoMin", "100").put("isoMax", "25600")
                .put("ssMin", "1/4000").put("ssMax", "30")
            HgeNative.nativeSetOwnedCameraDetail(nm, o.toString())
            runOnUiThread {
                selCamera = nm
                openCameraList()
                Toast.makeText(this, "「$nm」を追加しました。センサーとISO/SSを確認してください", Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun arrMinMax(a: JSONArray?): Pair<String, String> {
        if (a == null || a.length() == 0) return Pair("", "")
        val lo = a.optString(0)
        var hi = ""
        for (i in a.length() - 1 downTo 0) { val v = a.optString(i); if (v != "Bulb") { hi = v; break } }
        return Pair(lo, hi)
    }

    private fun buildCameraDetail() {
        val box = findViewById<LinearLayout>(R.id.cameralist_detail)
        box.removeAllViews(); camFields.clear(); camAutoInsert = null; camMeterLv = null; camLensNames.clear()
        buildingLens = false
        val sel = selCamera
        if (sel == null) {
            val tv = TextView(this); tv.text = "カメラを選択してください"; tv.setPadding(dp(4), dp(16), dp(4), dp(16)); box.addView(tv); return
        }
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        var cam: JSONObject? = null
        var ocObj: JSONObject? = null
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optJSONObject("camera")?.optString("name") == sel) { cam = o.optJSONObject("camera"); ocObj = o; break }
        }
        if (cam == null) { val tv = TextView(this); tv.text = "(データなし)"; box.addView(tv); return }
        val camCancel = addCancelButton(box, atTop = true) { buildCameraDetail() }   // 分割バー直下に右寄せ(取消=保存内容から作り直し)
        box.addView(editRow("メーカー", "maker", cam.optString("maker")))
        box.addView(editRow("モデル", "model", cam.optString("model")))
        // 名称はリストの行でインライン編集する(分割バー画面共通の動作)。詳細からは除外。
        // 項目D: 愛称(assignedName=カメラ本体で付けたニックネーム)は、カメラがオンラインになりSSDPで
        //  取得できてから自動で入る。手入力はしない。未取得のうちは「未定義」と表示する。
        box.addView(displayRow("愛称", cam.optString("assignedName").ifEmpty { "未定義" }))
        // シリアルNo. は手で入れられる(2026-08-30 UI依頼)。同じ機種を複数台そろえるとき、
        //  つなぐ前に区別を付けておきたいため。つないだら従来どおり実値で上書きされる。
        box.addView(editRow("シリアルNo.", "serial", cam.optString("serial")))
        // 未登録(0)は空欄で出す。0.0 と書くと「0という値が入っている」ように見えるため(2026-08-19)。
        // センサー寸法と画素数は機材マスターにある機種しか埋まらない。無い機種はここに手で入れる。
        fun blankIfZero(v: Double) = if (v > 0.0) v.toString() else ""
        fun blankIfZeroI(v: Int)   = if (v > 0) v.toString() else ""
        box.addView(editRow2("センサーサイズ", "sensorSize", blankIfZero(cam.optDouble("sensorSize", 0.0)),
            "sensorSizeV", blankIfZero(cam.optDouble("sensorSizeV", 0.0)), "×", "mm", true))
        box.addView(editRow2("センサーpixel", "sensorPixel", blankIfZeroI(cam.optInt("sensorPixel", 0)),
            "sensorPixelV", blankIfZeroI(cam.optInt("sensorPixelV", 0)), "×", "px", true))
        val iso = arrMinMax(cam.optJSONArray("isoList"))
        box.addView(editRow2("ISO感度", "isoMin", iso.first, "isoMax", iso.second, "〜", "", false))
        val ss = arrMinMax(cam.optJSONArray("ssList"))
        box.addView(editRow2("シャッター速度", "ssMin", ss.first, "ssMax", ss.second, "〜", "", false))
        val cb = CheckBox(this); cb.text = "撮影計画の初期値にする"; cb.isChecked = ocObj?.optBoolean("autoInsert", false) ?: false
        camAutoInsert = cb; box.addView(cb)
        // 測光方式。既定はサムネイルだけ(最も正確)。撮影済みサムネイルの取得回数に上限がある
        //  機種(EOS R10)だけこれを入れ、普段はライブビューで測って足りないときだけサムネイルへ落ちる。
        val cbLv = CheckBox(this); cbLv.text = "ライブビューで測光する"; cbLv.isChecked = cam.optBoolean("meterLv", false)
        camMeterLv = cbLv; box.addView(cbLv)
        // ダイジェスト認証。カメラ側の設定で有効にすると、CCAPI の全要求が 401 で弾かれる。
        //  空のままなら認証なしの機体として扱う(要求は 401 を受けてから作るので、事前設定は不要)。
        box.addView(thinDivider())
        val ahdr = TextView(this); ahdr.text = "カメラの認証(設定している機体のみ)"; ahdr.textSize = 13f
        ahdr.setTextColor(Color.GRAY); ahdr.setPadding(0, dp(8), 0, dp(4)); box.addView(ahdr)
        box.addView(editRow("ユーザーID", "authUser", cam.optString("authUser")))
        // パスワードは JSON では暗号文なので、平文はネイティブから別途もらう。
        box.addView(editRowPass("パスワード", "authPass", HgeNative.nativeOwnedCameraAuthPass(sel)))
        camAuthBaseline = camAuthSig()      // ここからの変化だけを「変更」とみなす

        // 組み合わせるレンズ(先頭=初期値)。並べ替えはハンドルをドラッグ(ss/iso/fnと同じ)。
        box.addView(thinDivider())
        val hdr = TextView(this); hdr.text = "組み合わせるレンズ(先頭が初期値)"; hdr.textSize = 13f; hdr.setTextColor(Color.GRAY)
        hdr.setPadding(0, dp(8), 0, dp(4)); box.addView(hdr)
        ocObj?.optJSONArray("lensList")?.let { ll -> for (i in 0 until ll.length()) ll.optJSONObject(i)?.optString("name")?.let { camLensNames.add(it) } }
        val lensBox = LinearLayout(this); lensBox.orientation = LinearLayout.VERTICAL
        box.addView(lensBox); camLensContainer = lensBox
        renderCamLensReorder()
        // ロック中に「本当に変えたか」を見るための基準。**組み合わせレンズを読み込んだ後**に採る
        //  (2026-09-01 修正)。以前は認証欄の直後で採っていて camLensNames が空のままだったため、
        //  レンズを持つカメラは開いて戻るだけで「変更できません」が出ていた。
        camEditBaseline = camEditSig()
        // 項目2: 「変更の取り消し」を dirty 連動に(未変更=グレー無効、変更で有効、戻すと無効)。
        startDirtyWatch(camCancel) { camDetailSig() }
    }

    // 所持カメラ詳細の編集内容シグネチャ(dirty 比較用)。編集欄・初期値チェック・レンズ順序を連結。
    private fun camDetailSig(): String {
        val sb = StringBuilder()
        camFields.toSortedMap().forEach { (k, v) -> sb.append(k).append('=').append(v.text).append(';') }
        sb.append("auto=").append(camAutoInsert?.isChecked == true)
          .append(";lv=").append(camMeterLv?.isChecked == true)
          .append(";lens=").append(camLensNames.joinToString(","))
        return sb.toString()
    }

    // 組み合わせレンズの並べ替え行を描く。ハンドル(▲▼)をドラッグ、挿入位置を線で示す(仕様5/6)。
    private fun renderCamLensReorder() {
        val box = camLensContainer ?: return
        box.removeAllViews(); lensRowViews.clear(); lensGapViews.clear()
        for (i in 0..camLensNames.size) {
            val gap = View(this)
            gap.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4))
            gap.setBackgroundColor(0x00000000); lensGapViews.add(gap); box.addView(gap)
            if (i < camLensNames.size) {
                val idx = i; val nm = camLensNames[i]
                val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
                row.setBackgroundColor(0xFFF2EEFA.toInt()); row.setPadding(dp(2), dp(2), dp(2), dp(2))
                val handle = TextView(this); handle.text = "▲\n▼"; handle.textSize = 12f; handle.gravity = Gravity.CENTER
                handle.setBackgroundColor(0xFFD1C4E9.toInt())
                handle.layoutParams = LinearLayout.LayoutParams(dp(40), dp(40))
                handle.setOnTouchListener(lensDragTouch(idx))
                val tv = TextView(this); tv.text = nm; tv.textSize = 14f
                tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                tv.setPadding(dp(8), 0, 0, 0)
                val menu = Button(this); menu.text = "⋮"; menu.textSize = 18f; menu.minWidth = dp(44)
                menu.setOnClickListener { anchor ->
                    val pm = PopupMenu(this, anchor); pm.menu.add("削除")
                    pm.setOnMenuItemClickListener { camLensNames.removeAt(idx); persistCameraDetail(true); true }; pm.show()
                }
                row.addView(handle); row.addView(tv); row.addView(menu)
                lensRowViews.add(row); box.addView(row)
            }
        }
        box.addView(linkText("＋ 新規レンズ追加") { addLensToCamera() })
    }

    private fun lensDragTouch(index: Int) = View.OnTouchListener { v, ev ->
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> { v.parent?.requestDisallowInterceptTouchEvent(true); lensDragFrom = index; lensHighlight(ev.rawY); true }
            MotionEvent.ACTION_MOVE -> { lensHighlight(ev.rawY); true }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { lensDrop(ev.rawY); true }
            else -> false
        }
    }
    private fun lensGapFor(rawY: Float): Int {
        val box = camLensContainer ?: return 0
        val loc = IntArray(2); box.getLocationOnScreen(loc)
        val y = rawY - loc[1]
        var g = 0
        for (c in lensRowViews) { if (c.top + c.height / 2f < y) g++ }
        return g.coerceIn(0, camLensNames.size)
    }
    private fun lensHighlight(rawY: Float) {
        val g = lensGapFor(rawY)
        for (k in lensGapViews.indices) lensGapViews[k].setBackgroundColor(if (k == g) 0xFF1565C0.toInt() else 0x00000000)
    }
    private fun lensDrop(rawY: Float) {
        val g = lensGapFor(rawY); val from = lensDragFrom; lensDragFrom = -1
        camLensContainer?.post {
            if (from in camLensNames.indices) {
                val item = camLensNames.removeAt(from)
                val insertAt = (if (g > from) g - 1 else g).coerceIn(0, camLensNames.size)
                camLensNames.add(insertAt, item)
            }
            persistCameraDetail(true)
        }
    }

    private fun addLensToCamera() {
        val arr = camArray(HgeNative.nativeGetOwnedLenses())
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }
        if (names.isEmpty()) { Toast.makeText(this, "所持レンズがありません。先に「所持レンズ」で登録してください", Toast.LENGTH_SHORT).show(); return }
        val checks = BooleanArray(names.size) { names[it] in camLensNames }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("組み合わせるレンズを選択")
            .setMultiChoiceItems(names.toTypedArray(), checks) { _, which, isChecked -> checks[which] = isChecked }
            .setPositiveButton("OK") { _, _ ->
                for (i in names.indices) { if (checks[i] && names[i] !in camLensNames) camLensNames.add(names[i]) }
                // チェックを外したものは除外
                camLensNames.retainAll { nm -> val ix = names.indexOf(nm); ix < 0 || checks[ix] }
                persistCameraDetail(true)
            }
            .setNegativeButton("キャンセル", null).show()
    }

    private fun leaveCameraList(dest: Int = kScreenMenu) { stopDirtyWatch(); persistCameraDetail(false); gotoScreen(dest) }

    // リスト選択(タップ)。同じ行の再タップは編集(フォーカス)のみ。別の行なら前の詳細を保存して切替。
    private fun selectCamera(name: String) {
        if (name == selCamera) return
        persistCameraDetail(false)            // 前の選択の編集内容を保存(分割バー画面共通の動作)
        selCamera = name; buildCameraList(); buildCameraDetail()
    }
    // リストでの名称インライン編集の確定。orig→newName へ改名(キー変更)。
    private fun commitCameraRename(orig: String, newName: String) {
        val nm = newName.trim()
        if (nm.isEmpty() || nm == orig) return
        if (ownedCameraNames().any { it == nm }) { showNameInUse(nm); buildCameraList(); return }  // item5: 重複拒否
        selCamera = nm
        persistCameraDetail(rebuild = true, origName = orig)
    }

    // カメラ詳細を保存する。名称はリストで編集するため selCamera を用いる。
    // rebuild=true は一覧/詳細を作り直す。origName 指定時はその名前を保存対象(改名時の元キー)とする。
    private fun persistCameraDetail(rebuild: Boolean, origName: String? = null) {
        // 名前を宛先にして書くので、編集中の名前があれば**先に**確定させる。
        //  (自分自身の改名から来たときは既に消費済みなので何もしない)
        if (origName == null) { commitListNameEdit(R.id.cameralist_container) }
        val orig = origName ?: selCamera ?: return
        if (camFields.isEmpty()) return
        // エッジが計画を持っているカメラは変更できない。ただし**中身が変わっていなければ素通し**
        //  にする(この関数はレンズ変更や画面離脱でも走るため、無条件に止めると画面から出られない)。
        if (camEditSig() != camEditBaseline && blockedByEdge(orig, "変更")) {
            buildCameraDetail()          // 画面を保存済みの値へ戻す
            return
        }
        val o = JSONObject()
        for ((k, et) in camFields) {
            when (k) {
                "sensorSize", "sensorSizeV" -> o.put(k, et.text.toString().toDoubleOrNull() ?: 0.0)
                "sensorPixel", "sensorPixelV" -> o.put(k, et.text.toString().toIntOrNull() ?: 0)
                else -> o.put(k, et.text.toString())
            }
        }
        o.put("name", selCamera ?: orig)      // 名称はリストのインライン編集分(selCamera)
        o.put("autoInsert", camAutoInsert?.isChecked ?: false)
        o.put("meterLv", camMeterLv?.isChecked ?: false)
        val ln = JSONArray(); camLensNames.forEach { ln.put(it) }; o.put("lensNames", ln)
        val js = o.toString()
        // 認証情報を変えた場合だけ、エッジ端末が持っている計画は古いままだと知らせる。
        //  保存はレンズ変更や画面離脱でも走るので、変わっていないときは黙っている。
        val authSig = camAuthSig()
        val authChanged = (authSig != camAuthBaseline)
        camAuthBaseline = authSig
        if (rebuild) {
            dataExec.execute {
                HgeNative.nativeSetOwnedCameraDetail(orig, js)
                runOnUiThread { buildCameraList(); buildCameraDetail(); pushCameraBookToEdges() }
            }
        } else {
            dataExec.execute { HgeNative.nativeSetOwnedCameraDetail(orig, js)
                               runOnUiThread { pushCameraBookToEdges() } }
        }
        if (authChanged) { noticeAuthChangedIfHeld(selCamera ?: orig) }
    }

    // 所持カメラ詳細で**ユーザーが触れる欄**すべての内容。ロック中に「本当に変えたか」を見る。
    //  保存はレンズ変更や画面離脱でも走るので、中身が同じなら黙って通す(素通しでも何も変わらない)。
    //  変わっているときだけ止める。こうしないと、ロック中は画面から出ることもできなくなる。
    private fun camEditSig(): String {
        val sb = StringBuilder()
        for (k in camFields.keys.sorted()) { sb.append(k).append('=').append(camFields[k]?.text?.toString() ?: "").append('	') }
        sb.append("autoInsert=").append(camAutoInsert?.isChecked ?: false).append('	')
        sb.append("meterLv=").append(camMeterLv?.isChecked ?: false).append('	')
        sb.append("lens=").append(camLensNames.joinToString(","))
        return sb.toString()
    }

    // 所持カメラ詳細の認証欄の内容(変更検知用)。
    private fun camAuthSig(): String =
        (camFields["authUser"]?.text?.toString() ?: "") + "\t" + (camFields["authPass"]?.text?.toString() ?: "")

    // そのカメラを使う撮影計画を保有しているエッジ端末の名前(=このカメラは変更/削除できない)。
    //
    //  【なぜ禁止するのか(2026-08-28 仕様確定)】エッジは受け取った計画をそのまま持ち続け、
    //   **スマホ抜きでも単独で開始できる**。手元のカメラだけ書き換えると、画面の値と実際に
    //   撮る値が食い違う。以前は警告を出すだけだったが、単独開始がある以上「次の開始で
    //   反映される」は当てにできないので、変更そのものを止める。
    //  【解除は遅らせる】planLockActive を参照。早すぎる解除は無防備になる。
    //  【自動の識別確定は止めない】シリアルや愛称がカメラ接続で自動的に入るのは従来どおり。
    //   止めているのは**ユーザーの編集と削除**だけ。
    private fun edgesHoldingCamera(camName: String): List<String> {
        if (camName.isEmpty()) return emptyList()
        val ids = HashSet<String>()
        try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            for (k in 0 until pa.length()) {
                val po = pa.optJSONObject(k) ?: continue
                val id = po.optString("id")
                if (po.optString("camName") == camName && planLockActive(id)) ids.add(id)
            }
        } catch (_: Exception) { return emptyList() }
        if (ids.isEmpty()) return emptyList()
        return edgeHeldByEdge.entries
            .filter { e -> e.value.any { ids.contains(it) } }
            .map { it.key }.distinct()
    }

    // 変更/削除を止めて理由を告げる。止めたときだけ true。
    private fun blockedByEdge(camName: String, what: String): Boolean {
        val edges = edgesHoldingCamera(camName)
        if (edges.isEmpty()) return false
        AlertDialog.Builder(this)
            .setTitle("${what}できません")
            .setMessage("このカメラを使用する撮影計画が、すでに外部端末「" +
                        edges.joinToString("」「") + "」に送られています。\n\n" +
                        "外部端末は受け取った計画をそのまま使うため、ここで${what}すると" +
                        "実際に撮影される内容と食い違います。\n\n" +
                        "先に計画一覧からその計画を外部端末から削除してください。" +
                        "撮影が終われば自動的に解除されます。")
            .setPositiveButton("OK", null)
            .show()
        return true
    }

    private fun noticeAuthChangedIfHeld(camName: String) {
        val edges = edgesHoldingCamera(camName)
        if (edges.isEmpty()) return
        runOnUiThread {
            AlertDialog.Builder(this)
                .setTitle("認証情報を変更しました")
                .setMessage("このカメラを使う撮影計画が外部端末「" + edges.joinToString("」「") + "」に置かれています。\n\n" +
                            "外部端末が持っている計画は変更前のままです。次回の撮影開始で送り直され、そのときに反映されます。")
                .setPositiveButton("OK", null)
                .show()
        }
    }

    // ---------- 622 カメラ追加(マスタ。チェックで複数追加) ----------
    private fun buildCameraAdd() {
        val box = findViewById<LinearLayout>(R.id.cameraadd_container)
        box.removeAllViews()
        // 機材マスタは出荷時の固定表なので、新しい機種やレンタル機は載っていない。
        //  一覧に無いカメラを手で足せる入り口をここに置く(型番だけ聞いて、残りは詳細画面で埋めてもらう)。
        box.addView(linkText("＋ 一覧に無いカメラを追加") { promptAddCustomCamera() })
        box.addView(thinDivider())
        val arr = camArray(HgeNative.nativeGetMasterCameras())
        for (i in 0 until arr.length()) {
            val cam = arr.optJSONObject(i)?.optJSONObject("camera") ?: continue
            val name = cam.optString("name")
            val cbx = CheckBox(this); cbx.text = "$name  (${cam.optString("maker")})"
            cbx.isChecked = name in checkedCamAdd
            cbx.setOnCheckedChangeListener { _, c -> if (c) checkedCamAdd.add(name) else checkedCamAdd.remove(name) }
            box.addView(cbx)
        }
    }

    // 622 を離れる時: チェックしたカメラを追加(toMenu=true は600へ、false は620へ)。
    // dest: 0=撮影計画 / 4=メニュー / それ以外=元の所持カメラ一覧へ戻る(取消ボタン用)
    private fun leaveCameraAdd(dest: Int) {
        val sel = ArrayList(checkedCamAdd); checkedCamAdd.clear()
        val back = { if (dest == kScreenHome || dest == kScreenMenu) gotoScreen(dest) else openCameraList() }
        if (sel.isEmpty()) { back(); return }
        Thread {
            // §4a: 同機種で未識別(愛称もシリアルも無い)のカメラが既にあると、もう一台は区別できないため追加不可。
            var ok = 0; val failed = ArrayList<String>()
            sel.forEach { if (HgeNative.nativeAddOwnedCamera(it) == 0) ok++ else failed.add(it) }
            runOnUiThread {
                if (ok > 0) pushCameraBookToEdges()
                val msg = when {
                    failed.isEmpty() -> "${ok}台を追加しました"
                    ok == 0 -> "追加できません: 同機種で未識別のカメラが既にあります。先に愛称かシリアルを設定してください"
                    else -> "${ok}台追加。${failed.size}台は追加不可(同機種の未識別カメラが既にあるため)"
                }
                Toast.makeText(this, msg, Toast.LENGTH_LONG).show(); back()
            }
        }.start()
    }

    // ---------- 630 所持レンズ ----------
    private fun buildLensList(): Unit = renderList(ListPane(
        containerId = R.id.lenslist_container,
        rows = {
            val arr = camArray(HgeNative.nativeGetOwnedLenses())
            (0 until arr.length()).mapNotNull { arr.optJSONObject(it) }.map { l ->
                val name = l.optString("name")
                ListItem(name, name,
                    "${l.optString("maker")}  ${l.optDouble("focalLength", 0.0).toInt()}mm  F${l.optDouble("fn", 0.0)}",
                    listOf("削除" to {
                        dataExec.execute { HgeNative.nativeRemoveOwnedLens(name)
                            runOnUiThread { if (selLens == name) selLens = null; buildLensList(); buildLensDetail() } }
                    }))
            }
        },
        selected = { selLens }, setSelected = { selLens = it },
        onSelect = { selectLens(it) },
        onRename = { orig, nm -> commitLensRename(orig, nm) },
        addLabel = "＋ 新規レンズ追加", onAdd = { openLensAdd() }))

    private fun buildLensDetail() {
        val box = findViewById<LinearLayout>(R.id.lenslist_detail)
        box.removeAllViews(); lensFields.clear(); lensContact = null
        buildingLens = true
        val sel = selLens
        if (sel == null) { val tv = TextView(this); tv.text = "レンズを選択してください"; tv.setPadding(dp(4), dp(16), dp(4), dp(16)); box.addView(tv); return }
        val arr = camArray(HgeNative.nativeGetOwnedLenses())
        var l: JSONObject? = null
        for (i in 0 until arr.length()) { val o = arr.optJSONObject(i) ?: continue; if (o.optString("name") == sel) { l = o; break } }
        if (l == null) { val tv = TextView(this); tv.text = "(データなし)"; box.addView(tv); return }
        val lensCancel = addCancelButton(box, atTop = true) { buildLensDetail() }   // 分割バー直下に右寄せ(取消=保存内容から作り直し)
        box.addView(editRow("メーカー", "maker", l.optString("maker")))
        // 名称(モデル)はリストの行でインライン編集する(分割バー画面共通の動作)。詳細からは除外。
        val cb = CheckBox(this); cb.text = "電子接点あり"; cb.isChecked = l.optBoolean("hasContact", true); lensContact = cb; box.addView(cb)
        box.addView(editRow2("F値", "fn", l.optDouble("fn", 0.0).toString(), "fnMax", l.optDouble("fnMax", 0.0).toString(), "〜", "", true))
        box.addView(editRow("焦点距離", "focalLength", l.optDouble("focalLength", 0.0).toString(), true))
        val note = TextView(this); note.text = "ズームの場合は撮影計画実行時の焦点距離を設定してください。"
        note.textSize = 12f; note.setTextColor(Color.GRAY); note.setPadding(0, dp(8), 0, dp(8)); box.addView(note)
        // 項目2: 「変更の取り消し」を dirty 連動に。
        startDirtyWatch(lensCancel) { lensDetailSig() }
    }

    private fun lensDetailSig(): String {
        val sb = StringBuilder()
        lensFields.toSortedMap().forEach { (k, v) -> sb.append(k).append('=').append(v.text).append(';') }
        sb.append("contact=").append(lensContact?.isChecked == true)
        return sb.toString()
    }

    private fun leaveLensList(dest: Int = kScreenMenu) { stopDirtyWatch(); persistLensDetail(false); gotoScreen(dest) }

    private fun selectLens(name: String) {
        if (name == selLens) return
        persistLensDetail(false)              // 前の選択の編集内容を保存(分割バー画面共通の動作)
        selLens = name; buildLensList(); buildLensDetail()
    }
    private fun commitLensRename(orig: String, newName: String) {
        val nm = newName.trim()
        if (nm.isEmpty() || nm == orig) return
        if (ownedLensNames().any { it == nm }) { showNameInUse(nm); buildLensList(); return }  // item5: 重複拒否
        selLens = nm
        persistLensDetail(rebuild = true, origName = orig)
    }

    private fun persistLensDetail(rebuild: Boolean, origName: String? = null) {
        if (origName == null) { commitListNameEdit(R.id.lenslist_container) }   // 名前を宛先にする前に先に確定させる
        val orig = origName ?: selLens ?: return
        if (lensFields.isEmpty()) return
        val o = JSONObject()
        for ((k, et) in lensFields) {
            when (k) {
                "fn", "fnMax", "focalLength" -> o.put(k, et.text.toString().toDoubleOrNull() ?: 0.0)
                else -> o.put(k, et.text.toString())
            }
        }
        o.put("name", selLens ?: orig)        // 名称(モデル)はリストのインライン編集分(selLens)
        o.put("hasContact", lensContact?.isChecked ?: true)
        val js = o.toString()
        if (rebuild) {
            dataExec.execute { HgeNative.nativeSetOwnedLensDetail(orig, js); runOnUiThread { buildLensList(); buildLensDetail() } }
        } else {
            dataExec.execute { HgeNative.nativeSetOwnedLensDetail(orig, js) }
        }
    }

    // ---------- 632 レンズ追加(マスタ。メーカー分類＋▼開閉、チェックで複数追加) ----------
    private fun buildLensAdd() {
        val box = findViewById<LinearLayout>(R.id.lensadd_container)
        box.removeAllViews()
        val arr = camArray(HgeNative.nativeGetMasterLenses())
        val byMaker = LinkedHashMap<String, MutableList<JSONObject>>()
        for (i in 0 until arr.length()) {
            val l = arr.optJSONObject(i) ?: continue
            byMaker.getOrPut(l.optString("maker", "その他")) { mutableListOf() }.add(l)
        }
        for ((maker, lenses) in byMaker) {
            val open = maker in expandedMakers
            val hdr = TextView(this); hdr.text = "${if (open) "▼" else "▶"}  $maker  (${lenses.size})"
            hdr.textSize = 16f; hdr.setTypeface(null, Typeface.BOLD); hdr.setPadding(dp(4), dp(10), dp(4), dp(10))
            hdr.setOnClickListener { if (open) expandedMakers.remove(maker) else expandedMakers.add(maker); buildLensAdd() }
            box.addView(hdr)
            if (open) {
                for (l in lenses) {
                    val name = l.optString("name")
                    val cbx = CheckBox(this)
                    cbx.text = "$name   ${l.optDouble("focalLength", 0.0).toInt()}mm F${l.optDouble("fn", 0.0)}"
                    cbx.setPadding(dp(24), 0, 0, 0)
                    cbx.isChecked = name in checkedLensAdd
                    cbx.setOnCheckedChangeListener { _, c -> if (c) checkedLensAdd.add(name) else checkedLensAdd.remove(name) }
                    box.addView(cbx)
                }
            }
            box.addView(thinDivider())
        }
    }

    // dest: 0=撮影計画 / 4=メニュー / それ以外=元の所持レンズ一覧へ戻る(取消ボタン用)
    private fun leaveLensAdd(dest: Int) {
        val sel = ArrayList(checkedLensAdd); checkedLensAdd.clear()
        val back = { if (dest == kScreenHome || dest == kScreenMenu) gotoScreen(dest) else openLensList() }
        if (sel.isEmpty()) { back(); return }
        Thread {
            sel.forEach { HgeNative.nativeAddOwnedLens(it) }
            runOnUiThread { Toast.makeText(this, "${sel.size}本を追加しました", Toast.LENGTH_SHORT).show(); back() }
        }.start()
    }

    // 接続カメラ検索→検出一覧をチェックして所持へ追加。
    private fun searchAndAddCameras() {
        Toast.makeText(this, "接続カメラを検索中…", Toast.LENGTH_SHORT).show()
        Thread {
            val js = HgeNative.nativeSearchDevicesList()
            runOnUiThread {
                val arr = camArray(js)
                if (arr.length() == 0) { Toast.makeText(this, "カメラが見つかりませんでした", Toast.LENGTH_SHORT).show(); return@runOnUiThread }
                val labels = (0 until arr.length()).map { val d = arr.optJSONObject(it)
                    "${d.optString("model")}  ${if (d.optString("serial").isNotEmpty()) "S/N:" + d.optString("serial") else ""}" }
                val checks = BooleanArray(arr.length()) { true }
                androidx.appcompat.app.AlertDialog.Builder(this)
                    .setTitle("検出したカメラ")
                    .setMultiChoiceItems(labels.toTypedArray(), checks) { _, which, c -> checks[which] = c }
                    .setPositiveButton("追加") { _, _ ->
                        Thread {
                            for (i in 0 until arr.length()) if (checks[i]) HgeNative.nativeAddOwnedDetected(i)
                            runOnUiThread { buildCameraList(); buildCameraDetail() }
                        }.start()
                    }
                    .setNegativeButton("キャンセル", null).show()
            }
        }.start()
    }

    // ラベル＋入力欄1つの行。
    // 表示専用の行(項目D: 愛称/シリアルなど、SSDP取得で自動的に入る値。手入力させない)。
    private fun displayRow(label: String, value: String): View {
        val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(3), 0, dp(3))
        val lab = TextView(this); lab.text = label; lab.textSize = 14f; lab.width = dp(118)
        val v = TextView(this); v.text = value; v.textSize = 14f
        v.setTextColor(if (value == "未定義") Color.GRAY else Color.DKGRAY)
        v.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        row.addView(lab); row.addView(v)
        return row
    }

    private fun editRow(label: String, key: String, value: String, numeric: Boolean = false): View {
        val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(3), 0, dp(3))
        val lab = TextView(this); lab.text = label; lab.textSize = 14f; lab.width = dp(118)
        val et = EditText(this); et.setText(value); et.textSize = 14f
        et.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        if (numeric) et.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        camFieldsOrLens(key, et)
        row.addView(lab); row.addView(et)
        return row
    }

    // ラベル＋伏せ字の入力欄(パスワード)。「表示」で平文に切り替えられる。
    //  カメラが繋がらないときに打ち間違いを確かめられないと原因が絞れないため、見る手段は残す。
    private fun editRowPass(label: String, key: String, value: String): View {
        val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(3), 0, dp(3))
        val lab = TextView(this); lab.text = label; lab.textSize = 14f; lab.width = dp(118)
        val et = EditText(this); et.setText(value); et.textSize = 14f
        // 【既定で見えるようにする(2026-08-27 UI依頼)】伏せてあると打ち間違いに気づけず、
        //  カメラが繋がらない原因を絞れない。人に見られて困る場面では外してもらう。
        et.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        et.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        val cb = CheckBox(this); cb.text = "表示"; cb.textSize = 12f; cb.isChecked = true
        cb.setOnCheckedChangeListener { _, on ->
            val p = et.selectionStart
            et.inputType = if (on) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                           else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            et.setSelection(p.coerceIn(0, et.text.length))
        }
        camFieldsOrLens(key, et)
        row.addView(lab); row.addView(et); row.addView(cb)
        return row
    }

    // ラベル＋入力欄2つの行(min/max や 横×縦)。
    private fun editRow2(label: String, k1: String, v1: String, k2: String, v2: String, sep: String, unit: String, numeric: Boolean): View {
        val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(0, dp(3), 0, dp(3))
        val lab = TextView(this); lab.text = label; lab.textSize = 14f; lab.width = dp(118)
        val e1 = EditText(this); e1.setText(v1); e1.textSize = 14f
        e1.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        val sp = TextView(this); sp.text = " $sep "; sp.textSize = 14f
        val e2 = EditText(this); e2.setText(v2); e2.textSize = 14f
        e2.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        if (numeric) { e1.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL; e2.inputType = e1.inputType }
        camFieldsOrLens(k1, e1); camFieldsOrLens(k2, e2)
        row.addView(lab); row.addView(e1); row.addView(sp); row.addView(e2)
        if (unit.isNotEmpty()) { val u = TextView(this); u.text = " $unit"; u.textSize = 14f; row.addView(u) }
        return row
    }

    // 現在組み立て中の画面(カメラ詳細 or レンズ詳細)の参照マップへ EditText を登録する。
    private var buildingLens = false
    private fun camFieldsOrLens(key: String, et: EditText) { if (buildingLens) lensFields[key] = et else camFields[key] = et }

    // 撮影計画のカメラ/レンズを所持機材から選ぶ(無ければ登録画面へ誘導)。
    // 同期撮影の追加カメラを複数選ぶ(2026-08-25)。
    //  主カメラ(計画の camera)は測光担当として必ず撮るので、この一覧からは外す。
    //  チェックの並び順ではなく所持カメラの並び順で確定する(順序に意味は無い)。
    private fun choosePlanSubCameras() {
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        val cams = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera") }
        val primary = latestSchedule.let {
            if (it.isEmpty()) "" else try { org.json.JSONObject(it).optString("camera") } catch (_: Exception) { "" }
        }
        val pick = cams.filter { it.optString("name").isNotEmpty() && it.optString("name") != primary }
        if (pick.isEmpty()) {
            Toast.makeText(this, "追加できるカメラがありません(所持カメラを登録してください)", Toast.LENGTH_LONG).show()
            return
        }
        val names = pick.map { it.optString("name") }
        val labels = pick.map { c ->
            val an = c.optString("assignedName")
            if (an.isNotEmpty()) c.optString("name") + "  (" + an + ")" else c.optString("name")
        }
        val checked = BooleanArray(names.size) { planSubCamNames.contains(names[it]) }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("追加で撮るカメラ")
            .setMultiChoiceItems(labels.toTypedArray(), checked) { _, which, isChecked ->
                checked[which] = isChecked
            }
            .setPositiveButton("決定") { _, _ ->
                val sel = org.json.JSONArray()
                for (i in names.indices) { if (checked[i]) sel.put(names[i]) }
                planExec.execute {
                    HgeNative.nativeSetPlanSubCameras(sel.toString())
                    val sched = HgeNative.nativeScheduleJson()
                    runOnUiThread { latestSchedule = sched; updatePlanDisplay(sched) }
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    // 【スマホ内蔵カメラかどうか(2026-09-05)】シリアルの頭で見分ける(apiBuiltin::kSerialPrefix)。
    //  内蔵カメラはその端末の中にしか無いので、外部端末へ送った計画では使えない。
    private fun isBuiltinSerial(serial: String): Boolean = serial.startsWith("BUILTIN:")

    // いまの計画のカメラが内蔵カメラか。
    private fun planUsesBuiltinCamera(): Boolean {
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        val want = try { JSONObject(HgeNative.nativeGetPlanJson()).optJSONObject("camera")?.optString("name") ?: "" }
                   catch (_: Exception) { "" }
        if (want.isEmpty()) return false
        for (i in 0 until arr.length()) {
            val c = arr.optJSONObject(i)?.optJSONObject("camera") ?: continue
            if (c.optString("name") == want) { return isBuiltinSerial(c.optString("serial")) }
        }
        return false
    }

    private fun choosePlanCamera() {
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        if (arr.length() == 0) { openCameraList(); return }
        val cams = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera") }
        val names = cams.map { it.optString("name") }
        // 選択肢は「名称(カメラ本体で付けた名前)」。同機種を2台持つと名称だけでは
        //  どちらか分からないため(2026-08-19 依頼)。選ぶキーは従来どおり名称。
        val labels = cams.map { c ->
            val an = c.optString("assignedName")
            if (an.isNotEmpty()) c.optString("name") + "  (" + an + ")" else c.optString("name")
        }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("カメラを選択")
            .setItems(labels.toTypedArray()) { _, which ->
                val name = names[which]
                // 【内蔵カメラを選んだら端末はスマホ(2026-09-05 ユーザー判断)】内蔵カメラは
                //  その端末の中にしか無いので外部端末へは送れない。選べなくして戸惑わせるより、
                //  黙って正しい組み合わせへ寄せる(何が起きたかはトーストで知らせる)。
                val builtin = isBuiltinSerial(cams[which].optString("serial"))
                if (builtin && planEdgeName(currentPlanId).isNotEmpty()) {
                    setPlanEdgeName(currentPlanId, "")
                    runOnUiThread {
                        refreshEdgeSpinner()
                        Toast.makeText(this, "内蔵カメラはこのスマホでしか使えないので、端末をスマホにしました",
                                       Toast.LENGTH_LONG).show()
                    }
                }
                planExec.execute {
                    HgeNative.nativeSetPlanCamera(name)
                    val sched = HgeNative.nativeScheduleJson()
                    // 一覧の副行にカメラを出すようになったので(2026-09-02)、ここでも作り直す。
                    //  詳細だけ直して一覧が古いままだと、同じ画面で違うカメラが2か所に出る。
                    runOnUiThread { latestSchedule = sched; updatePlanDisplay(sched); reloadExpoEditors()  // item3: iso/ss範囲をカメラに合わせ直す
                                    refreshPlanList() }
                }
            }.show()
    }

    private fun choosePlanLens() {
        val arr = camArray(HgeNative.nativeGetOwnedLenses())
        if (arr.length() == 0) { openLensList(); return }
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("レンズを選択")
            .setItems(names.toTypedArray()) { _, which ->
                val name = names[which]
                planExec.execute {
                    HgeNative.nativeSetPlanLens(name)
                    val sched = HgeNative.nativeScheduleJson()
                    runOnUiThread { latestSchedule = sched; updatePlanDisplay(sched); reloadExpoEditors() }  // item3: fn範囲をレンズに合わせ直す
                }
            }.show()
    }

    // 撮影計画画面の色別リストから「この計画の」撮影制御方法を編集する(初期値とは別)。
    private fun openPlanCcmEdit(key: String) {
        editingPlanCcm = true
        ccmJson = try { JSONObject(HgeNative.nativeGetPlanCcm()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        openCcmEdit(key)
    }

    // 撮影制御方法の型番号(C++ hgc::ccmType と一致させること)。月は廃止したので 5=夜間前移行。
    private val ccmTypeToKey = mapOf(1 to "night", 2 to "sunrise", 3 to "sunset", 4 to "day")
    private val ccmTypeName = mapOf(1 to "夜間撮影", 2 to "朝日撮影", 3 to "夕日撮影", 4 to "日中撮影",
                                    5 to "夜間前移行", 6 to "夜間後移行")

    // 撮影制御方法の編集ボタンを常に定位置(スケジュールの下)に並べる。タップで編集画面へ。
    // 夜間/朝日/夕日/日中の4つを横1列に並べる(以前は3列グリッドで日中だけが下に落ちていた)。
    // 色はシステム共通の設定から取るので、色を変えたらここを呼び直す必要がある。
    private fun buildCcmEditButtons() {
        val parent = findViewById<LinearLayout>(R.id.plan_ccmButtons)
        parent.removeAllViews()
        // 1=夜間 2=朝日 3=夕日 4=日中(月は廃止)。
        run {
            val row = LinearLayout(this)
            row.orientation = LinearLayout.HORIZONTAL
            row.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            for (t in listOf(1, 2, 3, 4)) {
                val key = ccmTypeToKey[t] ?: continue
                // 他のボタン(保存/撮影開始/接続/リセット)と同じ形にするため、テーマ既定の
                // materialButtonStyle で生成し、角丸はスタイル(=他ボタンと同一)を継承する。
                val btn = com.google.android.material.button.MaterialButton(
                    this, null, com.google.android.material.R.attr.materialButtonStyle)
                btn.text = ccmTypeName[t]
                btn.isAllCaps = false
                btn.textSize = 12f	// 4列にするので少し小さく
                // MaterialButton の既定最小幅(88dp)を残すと 4列ではみ出すので外す。
                btn.minWidth = 0; btn.minimumWidth = 0
                btn.setPadding(dp(2), btn.paddingTop, dp(2), btn.paddingBottom)
                btn.setTextColor(ccmTextColor(t))
                btn.backgroundTintList = ColorStateList.valueOf(ccmColor(t))
                btn.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    .apply { setMargins(dp(2), dp(2), dp(2), dp(2)) }
                btn.setOnClickListener { openPlanCcmEdit(key) }
                row.addView(btn)
            }
            parent.addView(row)
        }
    }

    // 太陽高度: Slider 0..14 ⇔ -19..-5°(1°刻み。分単位は将来キーボードで)
    private fun altToSeek(v: Double) = (v + 19.0).toInt().coerceIn(0, 14)
    private fun seekToAlt(p: Int) = -19.0 + p
    // ev: SeekBar 0..30 ⇔ -5.0..+5.0(1/3刻み)
    private fun evToSeek(v: Double) = ((v + 5.0) * 3.0).toInt().coerceIn(0, 30)
    private fun seekToEv(p: Int) = -5.0 + p / 3.0
    // ヒステリシス: Slider 0..20 ⇔ 0.0..2.0 ev(0.1刻み)。0=全体設定に従う(ccm個別では未設定扱い)。
    private fun hystToSeek(v: Double) = (v * 10.0).toInt().coerceIn(0, 20)
    private fun seekToHyst(p: Int) = p / 10.0
    private fun hystLabel(p: Int) = if (p == 0) "全体設定" else String.format("%.1fev", seekToHyst(p))
    // 移動平均フレーム数: Slider 0..10(1刻み)。0=全体設定に従う(ccm個別では未設定扱い)。
    private fun maLabel(p: Int) = if (p == 0) "全体設定" else "${p}frame"

    private fun altLabel(v: Double) = String.format("%.0f°", v)

    // 固定露出太陽高度の start(日没側)/end(日の出側) 時刻を天文計算で更新する(夜間のみ)。
    // start は上の行、end は固定露出太陽高度と同じ行に右寄せ表示(ラベル文言は付けない)。
    private fun updateAltTimes(deg: Int) {
        val startTv = findViewById<TextView>(R.id.edit_alt_start)
        val endTv = findViewById<TextView>(R.id.edit_alt_end)
        if (editingKey != "night") { startTv.text = ""; endTv.text = ""; return }
        Thread {
            val js = HgeNative.nativeSunAltitudeTimes(deg)
            runOnUiThread {
                try {
                    val o = JSONObject(js)
                    // "MM/dd HH:mm" → 時刻だけ表示。等幅・桁揃えで縦に整列。
                    startTv.text = "Start".padEnd(5) + " " + o.optString("start", "--").substringAfterLast(" ")
                    endTv.text = "End".padEnd(5) + " " + o.optString("end", "--").substringAfterLast(" ")
                } catch (_: Exception) { startTv.text = ""; endTv.text = "" }
            }
        }.start()
    }

    // 薄明帯ラベルと明暗バーを、スライダーのつまみ可動域(両端から trackSidePadding 内側)に
    // 合わせる。これをしないと端の度数がつまみ位置とずれる。
    private fun applyAltPadding(tsp: Int) {
        findViewById<LinearLayout>(R.id.edit_alt_bands).setPadding(tsp, 0, tsp, 0)
        val bar = findViewById<View>(R.id.edit_alt_bar)
        val lp = bar.layoutParams as FrameLayout.LayoutParams
        lp.setMargins(tsp, lp.topMargin, tsp, lp.bottomMargin)
        bar.layoutParams = lp
    }

    // 朝日/夕日の太陽高度範囲スライダー: 値 0..24 ⇔ 朝日 -18..+6 / 夕日 +6..-18(1°刻み)。
    private fun altRangeValToDeg(v: Int): Double = if (editingKey == "sunset") 6.0 - v else -18.0 + v
    private fun altRangeDegToVal(deg: Double): Int =
        (if (editingKey == "sunset") 6.0 - deg else deg + 18.0).toInt().coerceIn(0, 24)

    // 朝日/夕日: Start/End それぞれに角度と時刻(朝日=日の出側rise、夕日=日没側set)を表示する。
    private fun updateAltRangeLabels() {
        val v = findViewById<RangeSlider>(R.id.edit_alt_range).values
        if (v.size < 2) return
        val rising = editingKey == "sunrise"
        setAltThumbLabel(R.id.edit_alt_start, "Start", altRangeValToDeg(v[0].toInt()), rising)
        setAltThumbLabel(R.id.edit_alt_end, "End", altRangeValToDeg(v[1].toInt()), rising)
    }
    private val altTimeCache = HashMap<Int, String>()   // tvId -> 直近の時刻(ちらつき防止に保持)

    // 等幅・固定幅で「Start -06.0°  HH:mm」を表示。角度は符号+整数2桁+小数1桁の固定幅。
    // 時刻は直近値を保持したまま角度だけ即時更新し、計算後に時刻を差し替える(空白化させない)。
    private fun setAltThumbLabel(tvId: Int, label: String, deg: Double, rising: Boolean) {
        val tv = findViewById<TextView>(tvId)
        val lab = label.padEnd(5)                       // "Start"/"End  " で桁を揃える
        val angleStr = String.format("%+05.1f°", deg)   // 例 -06.0° / +00.0° / -18.0°
        val curTime = altTimeCache[tvId] ?: "--:--"
        tv.text = "$lab $angleStr  $curTime"
        Thread {
            val js = HgeNative.nativeSunAltitudeTimes(deg.toInt())
            runOnUiThread {
                val time = try {
                    val o = JSONObject(js)
                    (if (rising) o.optString("end", "--") else o.optString("start", "--")).substringAfterLast(" ")
                } catch (_: Exception) { "--:--" }
                altTimeCache[tvId] = time
                if (tv.text.toString().contains(angleStr)) {   // つまみがまだ同じ角度なら時刻を更新
                    tv.text = "$lab $angleStr  $time"
                }
            }
        }.start()
    }

    // 薄明帯の1区分(帯名・度幅の重み・背景色)。
    private data class Band(val label: String, val weight: Float, val color: Int)

    // 薄明帯ラベルを汎用に組む。bands=各区分、ticks=各区分の左端の度目盛り+右端(計 bands+1 個)。
    private fun buildBands(container: LinearLayout, bands: List<Band>, ticks: List<String>) {
        container.removeAllViews()
        val scale = LinearLayout(this); scale.orientation = LinearLayout.HORIZONTAL
        for (i in bands.indices) {
            val tv = TextView(this); tv.text = ticks.getOrElse(i) { "" }; tv.textSize = 9f
            tv.setTextColor(0xFF888888.toInt())
            tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, bands[i].weight)
            scale.addView(tv)
        }
        val end = TextView(this); end.text = ticks.getOrElse(bands.size) { "" }; end.textSize = 9f
        end.setTextColor(0xFF888888.toInt()); end.gravity = Gravity.END
        end.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        scale.addView(end)
        container.addView(scale)
        val bandRow = LinearLayout(this); bandRow.orientation = LinearLayout.HORIZONTAL
        for (b in bands) {
            val tv = TextView(this); tv.text = b.label; tv.textSize = 10f; tv.gravity = Gravity.CENTER
            tv.setTextColor(0xFFFFFFFF.toInt()); tv.setBackgroundColor(b.color)
            val lp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, b.weight)
            lp.setMargins(dp(1), 0, dp(1), 0); tv.layoutParams = lp
            bandRow.addView(tv)
        }
        container.addView(bandRow)
    }

    private val colAstro = 0xFF37474F.toInt(); private val colNaut = 0xFF607D8B.toInt()
    private val colCivil = 0xFF9E9E9E.toInt(); private val colDay = 0xFFE0B96A.toInt(); private val colNight = 0xFF1A1A2E.toInt()

    // 夜間: 左=-19°暗 → 右=-5°明。-19〜-18=夜(無ラベル)/-18〜-12 天文/-12〜-6 航海/-6〜-5 市民。
    private fun buildNightBands(container: LinearLayout) = buildBands(container,
        listOf(Band("", 1f, colNight), Band("天文薄明", 6f, colAstro), Band("航海薄明", 6f, colNaut), Band("市民", 1f, colCivil)),
        listOf("-19°", "-18°", "-12°", "-6°", "-5°"))

    // 朝日: 左=-18°暗 → 右=+6°明。天文/航海/市民/地平線上(0〜+6)。
    private fun buildSunriseBands(container: LinearLayout) = buildBands(container,
        listOf(Band("天文薄明", 6f, colAstro), Band("航海薄明", 6f, colNaut), Band("市民薄明", 6f, colCivil), Band("地平線上", 6f, colDay)),
        listOf("-18°", "-12°", "-6°", "0°", "+6°"))

    // 夕日: 左=+6°明 → 右=-18°暗(朝日と逆)。地平線上/市民/航海/天文。
    private fun buildSunsetBands(container: LinearLayout) = buildBands(container,
        listOf(Band("地平線上", 6f, colDay), Band("市民薄明", 6f, colCivil), Band("航海薄明", 6f, colNaut), Band("天文薄明", 6f, colAstro)),
        listOf("+6°", "0°", "-6°", "-12°", "-18°"))

    private fun openCcmEdit(key: String) {
        val o = ccmJson?.optJSONObject(key) ?: return
        editingKey = key
        // 計画固有の編集で、その計画がロックされていれば読取専用(初期値の編集は常に可)。
        ccmReadOnly = editingPlanCcm && planReadOnly
        val title = mapOf("night" to "夜間撮影", "sunrise" to "朝日撮影", "sunset" to "夕日撮影", "day" to "日中撮影")[key]
        findViewById<TextView>(R.id.edit_title).text = title +
            (if (!editingPlanCcm) "（初期値）" else if (ccmReadOnly) "（この計画・変更不可）" else "（この計画）")
        applyHeaderColor(R.id.edit_header, R.id.edit_title, keyType(key))   // タイトルバーにシステム共通色
        val showPreset = !editingPlanCcm   // 初期値編集時のみプリセット一覧を出す
        findViewById<View>(R.id.edit_presetScroll).visibility = if (showPreset) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_presetDivider).visibility = if (showPreset) View.VISIBLE else View.GONE
        // 一覧を出す/隠すで左右2分割の可否が変わるので掛け直す(横向きのときだけ効く)。
        applyMasterDetail(R.id.edit_presetScroll, R.id.edit_presetDivider)

        val hasAlt = false   // 開始/終了(太陽高度)はスケジュール画面の境界で設定するため ccm からは撤去
        val hasEv = key != "night"
        val isNight = key == "night"
        findViewById<View>(R.id.edit_alt_section).visibility = if (hasAlt) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_ev_section).visibility = if (hasEv) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_fixed_section).visibility = if (isNight) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_postev_section).visibility = if (isNight) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_preev_section).visibility = if (isNight) View.VISIBLE else View.GONE
        val isSun = key == "sunrise" || key == "sunset"
        findViewById<View>(R.id.edit_smooth_section).visibility = if (isSun) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_limit_section).visibility = if (isNight) View.GONE else View.VISIBLE

        if (hasAlt) {
            val isNightAlt = key == "night"
            findViewById<TextView>(R.id.edit_alt_title).text =
                if (isNightAlt) "固定露出太陽高度" else "太陽高度"
            findViewById<View>(R.id.edit_alt_seek).visibility = if (isNightAlt) View.VISIBLE else View.GONE
            findViewById<View>(R.id.edit_alt_range).visibility = if (isNightAlt) View.GONE else View.VISIBLE
            findViewById<View>(R.id.edit_alt_val).visibility = if (isNightAlt) View.VISIBLE else View.GONE
            val bar = findViewById<View>(R.id.edit_alt_bar)
            if (isNightAlt) {
                bar.setBackgroundResource(R.drawable.brightness_bar)
                buildNightBands(findViewById(R.id.edit_alt_bands))
                val sl = findViewById<Slider>(R.id.edit_alt_seek)
                sl.post { applyAltPadding(sl.trackSidePadding) }
                val p = altToSeek(o.optDouble("sunAltitude", -18.0))
                setSliderProgress(R.id.edit_alt_seek, p)
                findViewById<TextView>(R.id.edit_alt_val).text = altLabel(seekToAlt(p))
                updateAltTimes(seekToAlt(p).toInt())
            } else {
                val reversed = key == "sunset"
                bar.setBackgroundResource(if (reversed) R.drawable.brightness_bar_rev else R.drawable.brightness_bar)
                if (reversed) buildSunsetBands(findViewById(R.id.edit_alt_bands))
                else buildSunriseBands(findViewById(R.id.edit_alt_bands))
                val rg = findViewById<RangeSlider>(R.id.edit_alt_range)
                rg.post { applyAltPadding(rg.trackSidePadding) }
                val startDeg = o.optDouble("sunAltitude", if (reversed) 0.0 else -6.0)
                val endDeg = o.optDouble("sunAltitudeEnd", if (reversed) -6.0 else 0.0)
                val v1 = altRangeDegToVal(startDeg); val v2 = altRangeDegToVal(endDeg)
                rg.values = listOf(minOf(v1, v2).toFloat(), maxOf(v1, v2).toFloat())
                altTimeCache.clear()   // 別画面の時刻を引きずらない
                updateAltRangeLabels()
            }
        }
        if (hasEv) {
            val p = evToSeek(o.optDouble("ev", 0.0))
            setSliderProgress(R.id.edit_ev_seek, p)
            findViewById<TextView>(R.id.edit_ev_val).text = String.format("%+.1f ev", seekToEv(p))
        }
        if (isNight) {
            val pp = evToSeek(o.optDouble("postNightEv", 0.0))   // 夜間後露出補正
            setSliderProgress(R.id.edit_postev_seek, pp)
            findViewById<TextView>(R.id.edit_postev_val).text = String.format("%+.1f ev", seekToEv(pp))
            val pe = evToSeek(o.optDouble("preNightEv", 0.0))    // 夜間前露出補正(仕様3.7)
            setSliderProgress(R.id.edit_preev_seek, pe)
            findViewById<TextView>(R.id.edit_preev_val).text = String.format("%+.1f ev", seekToEv(pe))
            fixEditor.set(o.optJSONObject("limitBright"))
        } else {
            editLimit.set(o.optJSONObject("limitBright"), o.optJSONObject("limitDark"),
                o.optJSONArray("priority"), o.optJSONObject("initial"), dayMode = (key == "day"))
        }
        if (isSun) {
            val hp = hystToSeek(o.optDouble("hysteresis", 0.3))   // ccm個別の平滑化(項目7)
            setSliderProgress(R.id.edit_hyst_seek, hp)
            findViewById<TextView>(R.id.edit_hyst_val).text = hystLabel(hp)
            val mp = o.optInt("movingAverage", 3).coerceIn(0, 10)
            setSliderProgress(R.id.edit_ma_seek, mp)
            findViewById<TextView>(R.id.edit_ma_val).text = maLabel(mp)
        }
        // 計画固有編集では「初期値リストから選択」ボタンを出す(§7.4.1)。
        val onPick: (() -> Unit)? = if (editingPlanCcm) ({
            showPresetPicker(editingKey) { preset ->
                val all = ccmJson ?: return@showPresetPicker
                val merged = JSONObject(preset.toString()); merged.put("type", keyType(editingKey))
                all.put(editingKey, merged); openCcmEdit(editingKey)   // プリセット値を読み込む(以後変更可)
            }
        }) else null
        if (ccmReadOnly) {
            // 見るだけなので「変更の取り消し」も「初期値リストから選択」も出さない。
            findViewById<LinearLayout>(R.id.edit_content).let { b ->
                b.findViewWithTag<View>("ptop")?.let { v -> b.removeView(v) } }
            stopDirtyWatch()
        } else {
            val cancelBtn = addPresetTopRow(R.id.edit_content, { cancelCcmEdit() }, onPick)
            startDirtyWatch(cancelBtn) { buildCcmEditJson()?.toString() ?: "" }  // item8: 変更で赤・未変更でグレー
        }
        flipper.displayedChild = 3
        // 入力部をまとめて有効/無効にする。読取専用を解いたとき戻す必要があるので毎回掛ける。
        setEnabledDeep(findViewById<View>(R.id.edit_content), !ccmReadOnly)
    }

    // 入れ物(ViewGroup)はそのままに、中の部品だけ有効/無効にする。
    //  入れ物まで無効にするとスクロールが効かなくなり、読むこともできなくなる。
    //  並べ替えのドラッグは OnTouchListener なので isEnabled では止まらない(ccmReadOnly で見る)。
    private fun setEnabledDeep(v: View, enabled: Boolean) {
        if (v is ViewGroup) { for (i in 0 until v.childCount) setEnabledDeep(v.getChildAt(i), enabled) }
        else v.isEnabled = enabled
    }

    // 撮影制御方法編集の取り消し(保存済みから再読込して破棄)。
    private fun cancelCcmEdit() {
        if (!editingPlanCcm) { loadPresets(presetType); loadEditorOnly(); rebuildPresetList(); return }
        ccmJson = try { JSONObject(HgeNative.nativeGetPlanCcm()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        openCcmEdit(editingKey)
    }

    // 現在のエディタ内容から ccm の JSON を組み立てる(保存せず・破壊しない)。dirty 比較にも使う。
    private fun buildCcmEditJson(): JSONObject? {
        val all = ccmJson ?: return null
        val src = all.optJSONObject(editingKey) ?: return null
        val o = JSONObject(src.toString())   // コピー(保存時まで元を変えない)
        // 開始/終了(太陽高度)はスケジュール画面の境界で設定するため、ここでは書き込まない。
        if (editingKey != "night") o.put("ev", seekToEv(sliderProgress(R.id.edit_ev_seek)))
        if (editingKey == "sunrise" || editingKey == "sunset") {
            o.put("hysteresis", seekToHyst(sliderProgress(R.id.edit_hyst_seek)))    // ccm個別の平滑化(項目7)
            o.put("movingAverage", sliderProgress(R.id.edit_ma_seek))
        }
        if (editingKey == "night") {
            o.put("postNightEv", seekToEv(sliderProgress(R.id.edit_postev_seek)))   // 夜間後露出補正
            o.put("preNightEv", seekToEv(sliderProgress(R.id.edit_preev_seek)))     // 夜間前露出補正(仕様3.7)
            val e = fixEditor.get()
            o.put("limitBright", e)
            o.put("limitDark", JSONObject(e.toString()))   // 固定露出は明暗同値
        } else {
            o.put("priority", editLimit.getPriority())
            o.put("limitBright", editLimit.getBright())
            o.put("limitDark", editLimit.getDark())
            o.put("initial", editLimit.getInitial())
        }
        return o
    }

    // 編集内容を保存する(離脱時に呼ぶ。トースト/画面遷移はしない)。
    private fun persistCcmEdit() {
        if (ccmReadOnly) return         // 見るだけの表示。画面を離れるときも書き戻さない
        val all = ccmJson ?: return
        val o = buildCcmEditJson() ?: return
        all.put(editingKey, o)
        if (editingPlanCcm) HgeNative.nativeSetPlanCcm(all.toString()) else savePresetFromEditor(o)
    }

    // SeekBar 値変更だけ拾う簡易リスナ(カラーピッカーのRGBで使用)。
    private fun seekListener(onChange: (Int) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(sb: SeekBar?, p: Int, fromUser: Boolean) = onChange(p)
        override fun onStartTrackingTouch(sb: SeekBar?) {}
        override fun onStopTrackingTouch(sb: SeekBar?) {}
    }

    // 単純な値スライダー(太陽高度・ev・月補正等)を Material Slider で統一する(仕様8)。
    // 内部は従来通り 0..maxStep の整数段で扱う。gradient=true で明暗バー下地用に
    // トラックを透明化(左側の塗りつぶしを消す)。つまみは丸(●)。
    private fun setupValueSlider(id: Int, maxStep: Int, gradient: Boolean = false,
                                 thumbRes: Int = R.drawable.thumb_dot, onChange: (Int) -> Unit) {
        val s = findViewById<Slider>(id)
        s.valueFrom = 0f; s.valueTo = maxStep.toFloat(); s.stepSize = 1f
        s.isTickVisible = false; s.labelBehavior = LabelFormatter.LABEL_GONE
        s.setCustomThumbDrawable(thumbRes)
        if (gradient) { s.trackActiveTintList = transparentTint; s.trackInactiveTintList = transparentTint }
        s.addOnChangeListener { _, v, _ -> onChange(v.toInt()) }
    }
    private fun setSliderProgress(id: Int, p: Int) {
        val s = findViewById<Slider>(id); s.value = p.toFloat().coerceIn(s.valueFrom, s.valueTo)
    }
    private fun sliderProgress(id: Int): Int = findViewById<Slider>(id).value.toInt()

    // --- 露出値スライダー(iso/ss/fn 文字列配列から選択) ---

    private fun dp(v: Int) = (v * resources.displayMetrics.density).toInt()

    private fun jsonToList(a: JSONArray?): List<String> {
        val l = mutableListOf<String>()
        if (a != null) for (i in 0 until a.length()) l.add(a.optString(i))
        return l
    }

    // 設定可能な露出値(カメラ設定値の文字列)を Entity から取得して保持する。
    private fun loadExpoValues() {
        try {
            val o = JSONObject(HgeNative.nativeGetExpoValues())
            isoValues = jsonToList(o.optJSONArray("iso"))
            ssValues = jsonToList(o.optJSONArray("ss"))
            fnValues = jsonToList(o.optJSONArray("fn"))
        } catch (_: Exception) {}
        // 表示順: 左=暗い時→右=明るい時。iso/ss は反転、fn はそのまま。
        isoDisp = isoValues.reversed()
        ssDisp = ssValues.reversed()
        fnDisp = fnValues
    }

    // item3: カメラ/レンズ/計画が変わったら露出値リストを取り直し、スライダ(エディタ)を範囲ごと作り直す。
    // ExposureEditor は構築時にリストを取り込むため、範囲反映には再構築が必要。
    private fun reloadExpoEditors() { loadExpoValues(); buildExposureEditors() }

    private fun buildExposureEditors() {
        // 再構築(reloadExpoEditors: カメラ/レンズ変更・計画選択)ごとにエディタがコンテナへ行を
        // 追加するため、先にクリアしないとスライダーが1セットずつ積み増しされる(夜間撮影に5セット等)。
        val fixC = findViewById<LinearLayout>(R.id.edit_fix_container); fixC.removeAllViews()
        val limC = findViewById<LinearLayout>(R.id.edit_limit_container); limC.removeAllViews()
        fixEditor = ExposureEditor(fixC)
        editLimit = LimitEditor(limC)
    }

    // 左=暗(月)→右=明(太陽) を示す明暗バー(仕様5)。スライダーの下地に敷く。
    private fun brightnessBar(): android.graphics.drawable.GradientDrawable {
        val d = android.graphics.drawable.GradientDrawable(
            android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
            intArrayOf(0xFF263238.toInt(), 0xFF90A4AE.toInt(), 0xFFFFF9C4.toInt())
        )
        d.cornerRadius = dp(5).toFloat()
        return d
    }

    // 露出スライダーの行コンテナ(薄いカード)を作る。
    private fun sliderColumn(parent: LinearLayout): LinearLayout {
        val col = LinearLayout(this)
        col.orientation = LinearLayout.VERTICAL
        val colLp = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        colLp.setMargins(0, dp(4), 0, dp(4)); col.layoutParams = colLp
        col.setPadding(dp(4), dp(2), dp(4), dp(4))
        parent.addView(col)
        return col
    }

    // Material スライダーのトラックを透明にし、明暗バーの上に重ねた行を作る。
    // showIcons=true で両端に月/太陽を置く。端のつまみが画面端に来ないよう左右に余白(仕様8)。
    private fun sliderWithIcons(parent: LinearLayout, slider: View, showIcons: Boolean = true) {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(dp(16), 0, dp(16), 0)
        if (showIcons) {
            val moon = TextView(this); moon.text = "🌙"; moon.textSize = 16f
            moon.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            row.addView(moon)
        }
        val frame = FrameLayout(this)
        frame.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        val bar = View(this); bar.background = brightnessBar()
        val barLp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10))
        barLp.gravity = Gravity.CENTER_VERTICAL; barLp.setMargins(dp(10), 0, dp(10), 0)
        bar.layoutParams = barLp
        frame.addView(bar)
        slider.layoutParams = FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        frame.addView(slider)
        row.addView(frame)
        if (showIcons) {
            val sun = TextView(this); sun.text = "☀"; sun.textSize = 16f
            sun.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            row.addView(sun)
        }
        parent.addView(row)
    }

    private val transparentTint get() = ColorStateList.valueOf(0x00000000)

    // 単一露出を選ぶ1本スライダー行。タイトルは左寄せ(仕様7)、値は右に表示。
    // 夜間固定露出・月の開始時露出に使う。
    private inner class SingleRow(parent: LinearLayout, title: String, private val vals: List<String>) {
        private val slider = Slider(this@MainActivity)
        private val valTv: TextView
        init {
            val col = sliderColumn(parent)
            // タイトル(左)と値(センター)を同じ行に。タイトルが値を表す形「ISO感度  1600」。
            val head = FrameLayout(this@MainActivity)
            head.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            head.setPadding(dp(2), 0, dp(2), 0)
            val titleTv = TextView(this@MainActivity)
            titleTv.text = title; titleTv.textSize = 14f; titleTv.setTypeface(null, Typeface.BOLD)
            titleTv.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.START or Gravity.CENTER_VERTICAL)
            valTv = TextView(this@MainActivity)
            valTv.textSize = 18f; valTv.setTypeface(null, Typeface.BOLD); valTv.gravity = Gravity.CENTER
            valTv.layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER)
            head.addView(titleTv); head.addView(valTv); col.addView(head)

            slider.valueFrom = 0f
            slider.valueTo = (vals.size - 1).coerceAtLeast(1).toFloat()
            slider.stepSize = 1f
            slider.isTickVisible = false
            slider.labelBehavior = LabelFormatter.LABEL_GONE
            slider.trackActiveTintList = transparentTint
            slider.trackInactiveTintList = transparentTint
            slider.setCustomThumbDrawable(R.drawable.thumb_dot)
            slider.addOnChangeListener { _, value, _ -> valTv.text = vals.getOrElse(value.toInt()) { "" } }
            sliderWithIcons(col, slider, showIcons = false)   // 夜間固定露出・月の開始時露出は外側アイコン無し
        }
        fun set(value: String) {
            val idx = vals.indexOf(value).let { if (it < 0) 0 else it }
            slider.value = idx.toFloat().coerceIn(slider.valueFrom, slider.valueTo)
            valTv.text = vals.getOrElse(idx) { "" }
        }
        fun get(): String = vals.getOrElse(slider.value.toInt()) { "" }
    }

    // 単一露出(iso/ss/fn)を3スライダーで編集する。夜間固定露出・月の開始時露出に使う。
    private inner class ExposureEditor(container: LinearLayout) {
        private val isoRow = SingleRow(container, "ISO感度", isoDisp)
        private val ssRow = SingleRow(container, "シャッター速度", ssDisp)
        private val fnRow = SingleRow(container, "F値", fnDisp)

        fun set(o: JSONObject?) {
            isoRow.set(o?.optString("iso") ?: "")
            ssRow.set(o?.optString("ss") ?: "")
            fnRow.set(o?.optString("fn") ?: "")
        }
        fun get(): JSONObject = JSONObject()
            .put("iso", isoRow.get()).put("ss", ssRow.get()).put("fn", fnRow.get())
    }

    // 露出限界エディタ(仕様7 / §7.4.3): 項目(ISO/SS/F)ごとに「暗所限界〜明所限界の範囲スライダー」
    // を1枚のカードに置く。カードの並び順 = 優先度(上ほど先に変化)。
    // 並べ替えは左端のドラッグハンドルを上下に動かし、挿入位置バーを離した所へ移動(仕様3)。
    // priority値は exposureType(iso=0,ss=1,fn=2)。bright=limitBright=暗所限界 / dark=limitDark=明所限界。
    private inner class LimitEditor(private val container: LinearLayout) {
        private val order = mutableListOf(0, 1, 2)
        private val darkPlace = HashMap<Int, String>()    // 暗所限界 = limitBright(高ISO側・左つまみ)
        private val brightPlace = HashMap<Int, String>()  // 明所限界 = limitDark(低ISO側・右つまみ)
        private val keys = listOf("iso", "ss", "fn")
        private var initMode = 0                          // 基準: 0=明所限界 1=中間点 2=暗所限界(仕様4d)
        private var dayMode = false                       // 日中=3択ピッカー(明所/中間/暗所)。他=チェックボックス
        private val cards = mutableListOf<View>()
        private val dividers = mutableListOf<View>()
        private val darkTvs = HashMap<Int, TextView>(); private val brightTvs = HashMap<Int, TextView>()
        private val initTvs = HashMap<Int, TextView>()
        private var dragFrom = -1

        fun set(brightObj: JSONObject?, darkObj: JSONObject?, prio: JSONArray?, initialObj: JSONObject?,
                dayMode: Boolean = false) {
            order.clear()
            if (prio != null) for (i in 0 until prio.length()) order.add(prio.optInt(i))
            if (order.sorted() != listOf(0, 1, 2)) { order.clear(); order.addAll(listOf(0, 1, 2)) }
            for (t in 0..2) {
                darkPlace[t] = brightObj?.optString(keys[t]) ?: ""     // limitBright = 暗所限界
                brightPlace[t] = darkObj?.optString(keys[t]) ?: ""     // limitDark = 明所限界
            }
            this.dayMode = dayMode
            initMode = resolveInitMode(initialObj)
            render()
        }
        fun getBright(): JSONObject = JSONObject().put("iso", darkPlace[0]).put("ss", darkPlace[1]).put("fn", darkPlace[2])
        fun getDark(): JSONObject = JSONObject().put("iso", brightPlace[0]).put("ss", brightPlace[1]).put("fn", brightPlace[2])
        fun getPriority(): JSONArray { val a = JSONArray(); order.forEach { a.put(it) }; return a }
        fun getInitial(): JSONObject = JSONObject().put("iso", initVal(0)).put("ss", initVal(1)).put("fn", initVal(2))

        private fun valsFor(t: Int) = when (t) { 0 -> isoDisp; 1 -> ssDisp; else -> fnDisp }
        private fun nameFor(t: Int) = when (t) { 0 -> "ISO感度"; 1 -> "シャッター速度"; else -> "F値" }
        private fun updateVals(t: Int) {
            darkTvs[t]?.text = darkPlace[t]; brightTvs[t]?.text = brightPlace[t]
            initTvs[t]?.text = initVal(t)
        }
        private fun refreshInit() { for (t in 0..2) initTvs[t]?.text = initVal(t) }
        // 基準の各軸の値: 0=明所限界(limitDark=brightPlace) 2=暗所限界(limitBright=darkPlace) 1=中間点(index中点=APEX中点)。
        private fun initVal(t: Int): String {
            val vals = valsFor(t)
            return when (initMode) {
                0 -> brightPlace[t] ?: ""
                2 -> darkPlace[t] ?: ""
                else -> {
                    val di = vals.indexOf(darkPlace[t]).let { if (it < 0) 0 else it }
                    val bi = vals.indexOf(brightPlace[t]).let { if (it < 0) vals.size - 1 else it }
                    vals.getOrElse((di + bi + 1) / 2) { brightPlace[t] ?: "" }
                }
            }
        }
        // 保存済み initial(exposure) から 3択モードを復元(明所/暗所のどちらでもなければ中間点扱い)。
        private fun resolveInitMode(o: JSONObject?): Int {
            if (o == null) return 0
            val iso = o.optString("iso"); val ss = o.optString("ss"); val fn = o.optString("fn")
            if (iso == brightPlace[0] && ss == brightPlace[1] && fn == brightPlace[2]) return 0
            if (iso == darkPlace[0] && ss == darkPlace[1] && fn == darkPlace[2]) return 2
            return 1
        }
        private fun makeValTv(color: Int): TextView {
            val tv = TextView(this@MainActivity); tv.textSize = 15f; tv.setTypeface(null, Typeface.BOLD)
            tv.gravity = Gravity.CENTER; tv.setTextColor(color)
            tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            return tv
        }

        // 上部に「基準チェック」と列見出し(一か所)、続けて divider 挟みのカード(並べ替え対象)。
        private fun render() {
            container.removeAllViews()
            cards.clear(); dividers.clear(); darkTvs.clear(); brightTvs.clear(); initTvs.clear()
            if (dayMode) {   // 日中: 明所限界→中間点→暗所限界 をタップで巡回
                val tv = TextView(this@MainActivity)
                fun lbl() = "基準(タップで切替): " + when (initMode) { 0 -> "明所限界"; 1 -> "中間点"; else -> "暗所限界" }
                tv.text = lbl(); tv.textSize = 14f; tv.setPadding(dp(8), dp(8), dp(8), dp(8))
                tv.setBackgroundColor(0xFFD1C4E9.toInt()); tv.setTextColor(0xFF222222.toInt())
                tv.setOnClickListener { initMode = (initMode + 1) % 3; tv.text = lbl(); refreshInit() }
                container.addView(tv)
            } else {   // 朝日/夕日: 明所限界 or 暗所限界 のチェックボックス(従来どおり)
                val cb = CheckBox(this@MainActivity)
                cb.text = "明所限界を基準にする"; cb.isChecked = (initMode == 0)
                cb.setOnCheckedChangeListener { _, c -> initMode = if (c) 0 else 2; refreshInit() }
                container.addView(cb)
            }
            container.addView(headerRow())
            for (i in 0..order.size) {
                val div = View(this@MainActivity)
                div.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4))
                div.setBackgroundColor(0x00000000)
                dividers.add(div); container.addView(div)
                if (i < order.size) { val card = buildCard(i); cards.add(card); container.addView(card) }
            }
        }

        private fun headerRow(): View {
            val row = LinearLayout(this@MainActivity); row.orientation = LinearLayout.HORIZONTAL
            row.setPadding(dp(4), dp(2), dp(4), 0)
            fun col(text: String, weight: Float) {
                val tv = TextView(this@MainActivity); tv.text = text; tv.textSize = 11f
                tv.setTextColor(0xFF666666.toInt()); tv.gravity = Gravity.CENTER
                tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, weight)
                row.addView(tv)
            }
            col("", 1.5f); col("暗所限界", 1f); col("基準", 1f); col("明所限界", 1f); col("", 1f)
            return row
        }

        private fun buildCard(i: Int): LinearLayout {
            val t = order[i]; val vals = valsFor(t)
            val card = LinearLayout(this@MainActivity); card.orientation = LinearLayout.VERTICAL
            card.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            card.setBackgroundColor(0xFFF2EEFA.toInt())
            card.setPadding(dp(4), dp(2), dp(4), dp(4))

            // 行1: 名称(左) + 数値。通常は 暗所/基準/明所 の3値、月モードは明所限界のみ(仕様4g/6e)。
            val valRow = LinearLayout(this@MainActivity); valRow.orientation = LinearLayout.HORIZONTAL
            valRow.gravity = Gravity.CENTER_VERTICAL
            val name = TextView(this@MainActivity); name.text = nameFor(t); name.textSize = 14f; name.setTypeface(null, Typeface.BOLD)
            name.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.5f)
            val brightTv = makeValTv(0xFF222222.toInt())
            valRow.addView(name)
            run {
                val darkTv = makeValTv(0xFF222222.toInt()); val initTv = makeValTv(0xFF1565C0.toInt())
                val rspacer = View(this@MainActivity); rspacer.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                valRow.addView(darkTv); valRow.addView(initTv); valRow.addView(brightTv); valRow.addView(rspacer)
                darkTvs[t] = darkTv; brightTvs[t] = brightTv; initTvs[t] = initTv
            }
            card.addView(valRow)

            // 行2: ドラッグハンドル(左) + 範囲スライダー(右へ・少し短く)(仕様4f)
            val slRow = LinearLayout(this@MainActivity); slRow.orientation = LinearLayout.HORIZONTAL; slRow.gravity = Gravity.CENTER_VERTICAL
            val handle = TextView(this@MainActivity)
            handle.text = "▲\n▼"; handle.textSize = 12f; handle.gravity = Gravity.CENTER
            handle.setBackgroundColor(0xFFD1C4E9.toInt()); handle.setPadding(dp(4), dp(2), dp(4), dp(2))
            handle.layoutParams = LinearLayout.LayoutParams(dp(40), dp(40))
            handle.setOnTouchListener(dragTouch(i))
            slRow.addView(handle)
            val frame = FrameLayout(this@MainActivity)
            val frameLp = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f); frameLp.setMargins(dp(6), 0, dp(4), 0)
            frame.layoutParams = frameLp
            val bar = View(this@MainActivity); bar.setBackgroundResource(R.drawable.brightness_bar)
            val barLp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10)); barLp.gravity = Gravity.CENTER_VERTICAL; barLp.setMargins(dp(10), 0, dp(10), 0)
            bar.layoutParams = barLp; frame.addView(bar)
            val rs = RangeSlider(this@MainActivity)
            rs.layoutParams = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            rs.valueFrom = 0f; rs.valueTo = (vals.size - 1).coerceAtLeast(1).toFloat(); rs.stepSize = 1f
            rs.isTickVisible = false; rs.labelBehavior = LabelFormatter.LABEL_GONE
            rs.trackActiveTintList = transparentTint; rs.trackInactiveTintList = transparentTint
            // 左=月(暗所限界)/右=太陽(明所限界)。
            rs.setCustomThumbDrawablesForValues(R.drawable.ic_moon, R.drawable.ic_sun)
            val di = vals.indexOf(darkPlace[t]).let { if (it < 0) 0 else it }   // 暗所限界
            val bi = vals.indexOf(brightPlace[t]).let { if (it < 0) vals.size - 1 else it }
            rs.values = listOf(minOf(di, bi).toFloat(), maxOf(di, bi).toFloat())
            var lock = false
            rs.addOnChangeListener { _, _, _ ->
                if (lock) return@addOnChangeListener
                val v = rs.values
                darkPlace[t] = vals.getOrElse(v[0].toInt()) { "" }
                brightPlace[t] = vals.getOrElse(v[1].toInt()) { "" }
                updateVals(t)
            }
            frame.addView(rs); slRow.addView(frame)
            card.addView(slRow)
            updateVals(t)
            rs.post {   // 明暗バーをつまみ可動域に合わせる
                val tsp = rs.trackSidePadding
                val lp = bar.layoutParams as FrameLayout.LayoutParams
                lp.setMargins(tsp, lp.topMargin, tsp, lp.bottomMargin); bar.layoutParams = lp
            }
            return card
        }

        // ドラッグハンドルのタッチ処理。挿入位置(divider)を色で示し、離した位置へ並べ替える。
        private fun dragTouch(index: Int) = View.OnTouchListener { v, ev ->
            if (ccmReadOnly) return@OnTouchListener false   // 読取専用では並べ替えない
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.parent?.requestDisallowInterceptTouchEvent(true)   // ScrollView のスクロールを抑止
                    dragFrom = index; highlightGap(ev.rawY); showMoveLine(ev.rawY); true
                }
                MotionEvent.ACTION_MOVE -> { highlightGap(ev.rawY); showMoveLine(ev.rawY); true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { hideMoveLine(); dropAt(ev.rawY); true }
                else -> false
            }
        }

        // 指のY座標から挿入位置(0..order.size)を求める。
        private fun gapFor(rawY: Float): Int {
            val loc = IntArray(2); container.getLocationOnScreen(loc)
            val y = rawY - loc[1]
            var g = 0
            for (c in cards) { if (c.top + c.height / 2f < y) g++ }
            return g.coerceIn(0, order.size)
        }
        private fun highlightGap(rawY: Float) {
            val g = gapFor(rawY)
            for (k in dividers.indices) {
                dividers[k].setBackgroundColor(if (k == g) 0xFF1565C0.toInt() else 0x00000000)   // 移動先(青)
            }
        }

        // 移動中の線(2026-08-30 UI依頼)。**指のいる場所**に出し、指について動く。
        //  移動先(青・入る場所)とは役割が違うので色を変える(橙)。
        //  行の間に挟むと並びが動いてしまうので、レイアウトに関わらない overlay へ置く。
        private val moveLine = android.graphics.drawable.ColorDrawable(0xFFEF6C00.toInt())
        private var moveLineShown = false

        private fun showMoveLine(rawY: Float) {
            val loc = IntArray(2); container.getLocationOnScreen(loc)
            val y = (rawY - loc[1]).toInt().coerceIn(0, container.height)
            val h = dp(3)
            moveLine.setBounds(0, y - h / 2, container.width, y + h / 2)
            if (!moveLineShown) { container.overlay.add(moveLine); moveLineShown = true }
            container.invalidate()
        }
        private fun hideMoveLine() {
            if (moveLineShown) { container.overlay.remove(moveLine); moveLineShown = false }
            container.invalidate()
        }
        private fun dropAt(rawY: Float) {
            val g = gapFor(rawY)
            val from = dragFrom
            dragFrom = -1
            // タッチ処理中にツリーを作り替えると描画が壊れるので、次フレームへ遅延する。
            container.post {
                if (from in order.indices) {
                    val item = order.removeAt(from)
                    val insertAt = (if (g > from) g - 1 else g).coerceIn(0, order.size)
                    order.add(insertAt, item)
                }
                render()
            }
        }
    }

    // 簡易カラーピッカー(R/G/B スライダー + プレビュー)。
    private fun showColorPicker(initial: Int, onPick: (Int) -> Unit) {
        val ctx = this
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(ctx); root.orientation = LinearLayout.VERTICAL; root.setPadding(pad, pad, pad, pad)
        val preview = View(ctx)
        preview.layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, (40 * resources.displayMetrics.density).toInt())
        var r = (initial shr 16) and 0xFF; var g = (initial shr 8) and 0xFF; var b = initial and 0xFF
        fun cur() = (r shl 16) or (g shl 8) or b
        fun refresh() { preview.setBackgroundColor(0xFF000000.toInt() or cur()) }
        refresh(); root.addView(preview)
        fun bar(label: String, init: Int, set: (Int) -> Unit): SeekBar {
            val t = TextView(ctx); t.text = label; root.addView(t)
            val s = SeekBar(ctx); s.max = 255; s.progress = init
            s.setOnSeekBarChangeListener(seekListener { set(it); refresh() })
            root.addView(s); return s
        }
        bar("赤 R", r) { r = it }; bar("緑 G", g) { g = it }; bar("青 B", b) { b = it }
        androidx.appcompat.app.AlertDialog.Builder(ctx)
            .setTitle("色の設定")
            .setView(root)
            .setPositiveButton("OK") { _, _ -> onPick(cur()) }
            .setNegativeButton("取消", null)
            .show()
    }

    // 撮影開始日のみ選べる(終了日は開始日と時刻の関係から自動で決まる。2026-08-08 UI依頼)。
    // 日付を変えても時刻は動かさない。終了日だけを derive で決め直す。
    private fun pickDate(cal: Calendar) {
        DatePickerDialog(this, { _, y, m, d ->
            cal.set(Calendar.YEAR, y); cal.set(Calendar.MONTH, m); cal.set(Calendar.DAY_OF_MONTH, d)
            deriveEndDate(); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
    }

    // 開始/終了の時刻。片方を変えてももう片方の時刻は動かさない(2026-08-08 UI依頼)。
    // 変わるのは終了日だけで、それも開始時刻と終了時刻の関係から自動で決まる。
    private fun pickTime(cal: Calendar) {
        TimePickerDialog(this, { _, h, min ->
            cal.set(Calendar.HOUR_OF_DAY, h); cal.set(Calendar.MINUTE, min); cal.set(Calendar.SECOND, 0)
            deriveEndDate(); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
    }

    // 終了日を「開始日 + (終了時刻が開始時刻以下なら1日)」で決める(2026-08-08 UI依頼)。
    //
    // 【なぜ自動にするか】従来は開始/終了を独立に編集でき、間隔が 1分〜24時間 に収まらない
    //  ときは「もう片方を動かして」帳尻を合わせていた。そのため日付を1日ずらすと開始時刻が
    //  勝手に書き換わり(実際に 17:00 が 21:00 へ押し出される)、操作の順番を知らないと
    //  設定できなかった。撮影は「その日の何時から翌朝の何時まで」の形しか使わないので、
    //  終了日はユーザーが決めるものではなく、開始日と2つの時刻から一意に決まる。
    //
    // 終了時刻＝開始時刻のときは 24時間(翌日)とする。0分の窓は意味がないため。
    private fun deriveEndDate() {
        endCal.set(Calendar.YEAR,  startCal.get(Calendar.YEAR))
        endCal.set(Calendar.MONTH, startCal.get(Calendar.MONTH))
        endCal.set(Calendar.DAY_OF_MONTH, startCal.get(Calendar.DAY_OF_MONTH))
        endCal.set(Calendar.SECOND, 0); endCal.set(Calendar.MILLISECOND, 0)
        if (endCal.timeInMillis <= startCal.timeInMillis) { endCal.add(Calendar.DAY_OF_MONTH, 1) }
    }

    private fun updateTimeButtons() {
        startDate.text = fmtDate.format(startCal.time)
        startTime.text = fmtTime.format(startCal.time)
        endDate.text = fmtDate.format(endCal.time)
        endTime.text = fmtTime.format(endCal.time)
        // 終了日は自動で決まる項目なので押せない・グレー表示にする(2026-08-08 UI依頼)。
        // isEnabled=false だと setPlanEditEnabled 側の一括操作と競合するので、
        // クリック不可と色だけをここで固定する。
        endDate.isClickable = false
        endDate.isFocusable = false
        endDate.setTextColor(0xFF9E9E9E.toInt())
    }

    // 開始/終了時刻をEntityへ渡してスケジュールを再生成させる(結果はEV_SCHEDULEで反映)。
    private fun pushTimesToEntity() {
        val s = fmtIso.format(startCal.time)
        val e = fmtIso.format(endCal.time)
        val off = TimeZone.getDefault().getOffset(startCal.timeInMillis) / 60000
        planExec.execute {
            HgeNative.nativeSetPlanTimes(s, e, off)
            // 時刻変更で撮影可否(終了>現在)が変わるので、開始アイコンを即更新する(指示2)。
            runOnUiThread { refreshPlanList() }
        }
    }

    // 撮影方向/仰角をEntityへ渡す(撮影シミュレーション画面から呼ぶ。項目11で計画1ページ目からは削除)。
    // 画角ゲートは廃止したのでスケジュールの帯分類は変わらないが、cs の azimuth/elevation は保存される。
    private fun pushDirectionToEntity(az: Float, el: Float) {
        planExec.execute { HgeNative.nativeSetPlanDirection(az.toDouble(), el.toDouble()) }
    }

    // ============================================================
    //  複数撮影計画リスト(分割バー上。§7.3.1/§7.3.3)
    // ============================================================
    // 一覧に載っている計画idを取り出す(選択が生きているかの判定用)。
    private fun planIdsIn(js: String): Set<String> {
        val out = mutableSetOf<String>()
        try {
            val a = JSONArray(js)
            for (i in 0 until a.length()) { a.optJSONObject(i)?.optString("id")?.let { if (it.isNotEmpty()) out.add(it) } }
        } catch (_: Exception) {}
        return out
    }

    private fun refreshPlanList() {
        // 一覧の読み出しも計画操作と同じ単一スレッドで実行し、改名・編集の直後に最新状態を読む。
        planExec.execute {
            // 【ひな形モード(2026-09-04 UI依頼)】一覧をひな形に差し替える。選択を native へ
            //  戻す処理はしない(戻すと、いま開いているひな形が押し出される)。
            if (tplMode) {
                val tj = HgeNative.nativeListTemplates()
                val tids = planIdsIn(tj)
                runOnUiThread {
                    if (currentPlanId.isEmpty() || !tids.contains(currentPlanId)) {
                        currentPlanId = tids.firstOrNull() ?: ""
                    }
                    buildPlanList(tj); updateReadOnly()
                }
                return@execute
            }
            var js = HgeNative.nativeListPlans()
            // 【選択を持つのはUI側だけ(2026-09-04 UI依頼)】一覧の作り直しは「中身の更新」であって
            //  「選択の変更」ではない。以前はここで毎回 native の選択を取り込んでいたため、内容を
            //  変えた拍子に別の計画へ飛んだ。新規計画は**最初の保存でidが確定する**ので、その最中に
            //  読むと別のidが返る(だから「移ることも移らないこともある」)。
            //  選択を動かすのは、ユーザーのタップ・新規作成・複製・削除だけ。ここでは逆に、
            //  native がずれていたらUIの選択へ**戻す**(食い違ったまま編集して別計画を壊さない)。
            val sel = currentPlanId
            if (sel.isNotEmpty() && planIdsIn(js).contains(sel) && HgeNative.nativeCurrentPlanId() != sel) {
                HgeNative.nativeSelectPlan(sel)
                js = HgeNative.nativeListPlans()   // 編集中の計画は未保存の変更も載るので取り直す
            }
            val cur = HgeNative.nativeCurrentPlanId()
            // 項目5: 計画の新規作成/変更/削除いずれの経路も refreshPlanList を通るので、ここで予約表を
            //  全計画から作り直す(終了が過去の計画は buildReservations 内で除外)。削除した計画が予約表に
            //  残る・変更が反映されない不具合の根治。buildReservations/saveReservations はUI非依存。
            saveReservations(buildReservations())
            val ids = planIdsIn(js)
            runOnUiThread {
                // 選択が消えた(削除された)ときと、まだ何も選んでいない起動時だけ native に従う。
                if (currentPlanId.isEmpty() || !ids.contains(currentPlanId)) { currentPlanId = cur }
                buildPlanList(js); updateReadOnly(); refreshEdgeSpinner()
            }
        }
    }

    // 【ひな形画面に入る(2026-09-04 UI依頼)】撮影計画画面のまま、一覧をひな形に差し替える。
    //  ひな形が1件も無いときは入らない。入ると下の編集欄が「今の計画」を映したままになり、
    //  何を編集しているのか分からなくなるため。作り方を案内して戻す。
    private fun openTemplates() {
        planExec.execute {
            val ids = tplIdsSorted()
            runOnUiThread {
                if (ids.isEmpty()) {
                    Toast.makeText(this, "ひな形がありません。撮影計画の⋮から「ひな形に保存」で作れます",
                                   Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                planIdBeforeTpl = currentPlanId
                tplMode = true
                selectTplRow(ids.first())
                flipper.displayedChild = 0
            }
        }
    }

    // ひな形画面から出る。**元の計画へ戻してから**画面を切り替える(戻すのは非同期なので、
    //  終わる前に画面を出すと、撮影計画画面がひな形を映したまま出てしまう)。
    private fun leaveTemplates(then: () -> Unit) {
        val back = planIdBeforeTpl
        planExec.execute {
            if (back.isNotEmpty()) { HgeNative.nativeSelectPlan(back) }
            runOnUiThread {
                tplMode = false
                if (back.isNotEmpty()) { currentPlanId = back }
                refreshPlanList(); updateReadOnly(); applyTplMode()
                then()
            }
        }
    }

    private fun tplIdsSorted(): List<String> = planIdsIn(HgeNative.nativeListTemplates()).toList()

    // ひな形を1件選ぶ(編集対象にする)。撮影計画の selectPlanRow と同じ役目。
    private fun selectTplRow(id: String) {
        planExec.execute {
            HgeNative.nativeSelectTemplate(id)
            runOnUiThread { currentPlanId = id; refreshPlanList(); applyTplMode(); reloadExpoEditors() }
        }
    }

    // ひな形モードでできないことを画面から消す。題も差し替える。
    private fun applyTplMode() {
        findViewById<View>(R.id.plan_edgeRow)?.visibility = if (tplMode) View.GONE else View.VISIBLE
        // ホームは「撮影計画ひな形」のときだけ出す。撮影計画そのものがホームなので、
        //  そこでは押す意味がない(場所は空けたままにして題を中央に保つ)。
        findViewById<View>(R.id.plan_home)?.visibility = if (tplMode) View.VISIBLE else View.INVISIBLE
        updatePagerTitle()
    }

    private fun buildPlanList(js: String) {
        clearListFocus(planListContainer)   // 消す子にフォーカスが残っていると落ちる
        planListContainer.removeAllViews()
        // 行の区切りは薄い線(2026-09-02 UI依頼)。所持カメラ/レンズの一覧と同じ見え方にする。
        //  副行が付いて1行が2段になったので、線が無いとどこまでが1件か分かりにくい。
        //  ※点滅処理(planBlink)は子を LinearLayout に絞って走査するので、線が挟まっても素通しする。
        try {
            val arr = JSONArray(js)
            for (i in 0 until arr.length()) {
                planListContainer.addView(buildPlanRow(arr.getJSONObject(i)))
                planListContainer.addView(thinDivider())
            }
        } catch (_: Exception) {}
        // 【「＋ 新規◯◯」は一覧の一番下(2026-09-04 UI依頼)】以前ここだけ先頭にあった。
        //  画面によって上下すると、探す場所が毎回変わって使いにくい。他の一覧に揃える。
        //  ひな形画面には出さない。ひな形は撮影計画の⋮「ひな形に保存」から作るもので、
        //  まっさらな状態から作る意味がないため(コピーは行の⋮にある)。
        if (!tplMode) {
            planListContainer.addView(linkText("＋ 新規撮影計画") { commitPlanNameEdit(); doNewPlan() })
        }
        // 再構築直後に選択中行のEditTextが自動フォーカスしてキーボードが出るのを防ぐ(フォーカスをスクロールへ)。
        planListScroll.isFocusableInTouchMode = true
        planListScroll.requestFocus()
        // リスト件数が少なければ内容ぴったりまで縮める(item6: リスト最下段で止める)。
        setInitialSplit(R.id.plan_listContainer)
    }

    private fun buildPlanRow(p: JSONObject): View {
        val id = p.optString("id")
        val name = p.optString("planName")
        val capturable = p.optBoolean("capturable") && !tplMode   // ひな形は撮影しないので開始アイコンを出さない
        val capturing = capturingPlans.contains(id)            // 実撮影中=点滅
        val disconnected = disconnectedPlans.contains(id)      // カメラ未検出(NOCAMERA)=✖点灯
        val waiting = waitingPlans.contains(id)                // 撮影窓前で待機=点灯
        val active = capturing || disconnected || waiting      // 撮影要求済(タップで中止)
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(dp(4), dp(6), dp(4), dp(6))
        if (id == currentPlanId) row.setBackgroundColor(0xFFE3F2FD.toInt())
        // 左アイコン(指示1/2): 未検出=✖点灯 / 撮影中=点滅 / 待機(窓前)=点灯 / 撮影可=開始(ICO開始D) / 不可=空。
        val icon = ImageView(this)
        icon.layoutParams = LinearLayout.LayoutParams(dp(32), dp(32)).apply { rightMargin = dp(6) }
        when {
            disconnected -> {
                icon.setImageResource(R.drawable.ic_camera_ng)  // Phase4: ✖カメラ(ICOカメラNon.png)。色でなく×印で区別(色覚多様性)
                icon.clearColorFilter()
                icon.tag = "ng:$id"                             // 点灯(点滅しない)
                icon.setOnClickListener { confirmStop(id) }
            }
            capturing -> {
                icon.setImageResource(R.drawable.ic_camera_cap)
                icon.clearColorFilter()
                icon.tag = "cam:$id"                        // 緑点滅(planBlink が cam: を点滅)
                icon.setOnClickListener { confirmStop(id) }
            }
            waiting -> {
                icon.setImageResource(R.drawable.ic_camera_cap)
                icon.clearColorFilter()
                icon.tag = "wait:$id"                       // 点灯(点滅しない)
                icon.setOnClickListener { confirmStop(id) }
            }
            capturable -> {
                icon.setImageResource(R.drawable.ic_start_d)
                icon.setOnClickListener { startPlan(id) }
            }
            else -> icon.setImageDrawable(null)
        }
        row.addView(icon)
        // 計画名: 1タップ目=選択(下に詳細表示、キーボード無し)。選択中の行の名前タップ=編集(キーボード→確定で改名)。
        val tv = EditText(this)
        tv.setText(name)
        tv.textSize = 15f
        tv.isSingleLine = true
        tv.setPadding(dp(4), dp(2), dp(4), dp(2))
        tv.background = null   // ラベル風(下線なし)
        tv.inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        tv.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
        if (id == currentPlanId) tv.setTypeface(null, Typeface.BOLD)
        tv.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        if (!active && id == currentPlanId) {
            // 選択中かつ非実行中の計画 → 名前タップで編集可能(キーボード)。
            //  確定は「完了(Done)」だけでなく、フォーカスが外れた時(別の場所をタップ)にも行う。
            //  従来は Done を押さずにタップで抜けると改名されなかった。
            tv.isFocusableInTouchMode = true; tv.isFocusable = true
            var committed = false   // 二重発火(Done→フォーカス喪失, refreshでの破棄)を防ぐ
            val commit = {
                val nm = tv.text.toString().trim()
                if (!committed && nm.isNotEmpty() && nm != name) {
                    committed = true
                    planExec.execute {
                        val r = if (tplMode) HgeNative.nativeRenameTemplate(id, nm)
                                else HgeNative.nativeRenamePlan(id, nm)   // item5: 重複なら ERR_NAME_DUP
                        runOnUiThread { if (r == HgeNative.ERR_NAME_DUP) showNameInUse(nm); refreshPlanList() }
                    }
                }
            }
            // 【確定処理を持っておく(2026-09-04 UI依頼)】ダイアログはアクティビティのフォーカスを
            //  奪わないので、フォーカス喪失だけに頼ると「名前を打った直後に撮影場所を開く」流れで
            //  改名を取りこぼす(入れた名前が消える)。書き換える前に commitPlanNameEdit() で呼ぶ。
            pendingRename[R.id.plan_listContainer] = commit
            tv.setOnEditorActionListener { v, actionId, _ ->
                if (actionId == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) {
                    planListScroll.isFocusableInTouchMode = true
                    planListScroll.requestFocus()   // 隣行へ飛ばずキーボードを閉じる(→OnFocusChangeで確定)
                    (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
                        .hideSoftInputFromWindow(v.windowToken, 0)
                    commit()
                    true
                } else false
            }
            tv.setOnFocusChangeListener { _, hasFocus -> if (!hasFocus) commit() }   // 他のEditTextへ移ったら確定
            // 他の計画選択/メニュー/新規作成などでリストが再構築(refreshPlanList)されると、この行の
            // EditText が破棄される。その時に保留中の編集を確定する(タップで抜けても保存される)。
            tv.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
                override fun onViewAttachedToWindow(v: View) {}
                override fun onViewDetachedFromWindow(v: View) {
                    commit()
                    if (pendingRename[R.id.plan_listContainer] === commit) { pendingRename.remove(R.id.plan_listContainer) }
                }
            })
        } else {
            // 未選択(または撮影中)の行 → タップで選択のみ(キーボードは出さない)。
            tv.isFocusable = false; tv.isFocusableInTouchMode = false
            tv.setOnClickListener { if (tplMode) selectTplRow(id) else selectPlanRow(id) }
        }
        // 名前の下に副行を出す(2026-09-02 UI依頼)。所持カメラ/レンズの一覧と同じ「見出し＋副行」。
        //  一覧だけ見て「どれがどの端末・どのカメラか」が分かるようにする。開いて確かめなくて済む。
        //  見出しは付けない(2026-09-02 UI依頼)。端末名と愛称だけを並べる。
        //  愛称はカメラがオンラインになってから入る。未取得のうちは**機種名**を出す
        //  (2026-09-02 UI依頼)。「未定義」より、どのカメラの計画かが分かる方が役に立つ。
        //  機種名も無い(カメラ未設定)ときだけ「未定義」と出す。空欄だと不具合に見える。
        val txt = LinearLayout(this)
        txt.orientation = LinearLayout.VERTICAL
        txt.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        txt.addView(tv)
        val subTv = TextView(this)
        // ひな形は端末を持たないので、代わりにレンズを出す(カメラとレンズでシミュレーションが決まる)。
        val camTxt = p.optString("camAssignedName").ifEmpty { p.optString("camModel") }.ifEmpty { "未定義" }
        subTv.text = if (tplMode) camTxt + "  " + p.optString("lens").ifEmpty { "レンズ未定義" }
                     else planEdgeName(id).ifEmpty { kPhoneEdgeLabel } + "  " + camTxt
        subTv.textSize = 12f
        subTv.setTextColor(Color.GRAY)
        subTv.isSingleLine = true
        subTv.setPadding(dp(4), 0, dp(4), 0)
        subTv.setOnClickListener { if (tplMode) selectTplRow(id) else selectPlanRow(id) }   // 副行のタップでも行を選べる
        txt.addView(subTv)
        row.addView(txt)
        // ⋮ コンテキストメニュー(他画面=撮影場所/撮影制御方法リストと同じ緑ピル。項目5)
        //  項目6: エッジ端末に送信済み(=どこかのエッジが保有)の計画は削除できない → 「削除」項目を出さない。
        //  スマホで停止すればエッジからも消え、ロックが解けて再び削除可能になる。
        // 項目2(再修正2): メニューは「タップした瞬間」に組み立てる。行を作った時点で固定すると、
        //  停止直後にどのタイミングで行が再構築されたかで内容が変わり、「出るときと出ないときがある」
        //  状態になる(ポーリングが一時的に撮影中へ戻すこともある)。常に最新状態で判断する。
        row.addView(ctxMenuButtonDynamic { buildPlanRowMenu(id, name) })
        return row
    }

    // 計画行の ⋮ メニュー内容。タップのたびに最新の状態で組み立てる(項目2)。
    private fun buildPlanRowMenu(id: String, name: String): List<Pair<String, () -> Unit>> {
        // ひな形の ⋮。撮影しないので開始/停止・エッジ関係の項目は無い。
        if (tplMode) {
            return listOf(
                "このひな形で撮影計画を作る" to { newPlanFromTemplate(id) },
                "コピーを追加" to {
                    planExec.execute { HgeNative.nativeCopyTemplate(id); runOnUiThread { refreshPlanList() } }
                },
                "削除" to { confirmDeleteTemplate(id, name) })
        }
        val onEdge = isPlanOnEdge(id)
        // 「実撮影中」だけを撮影中とみなす。中止操作直後は stoppingPlans に数秒残るが、
        //  中止済みならもう撮影していないので「エッジ端末から削除」を出してよい。
        val capturingNow = capturingPlans.contains(id)
        val menu = mutableListOf<Pair<String, () -> Unit>>("コピーを追加" to { copyPlanRow(id) })
        menu.add("ひな形に保存" to { saveTemplateFrom(id, name) })
        if (!onEdge) { menu.add("ひな形で更新" to { chooseTemplateForUpdate(id) }) }
        if (!onEdge) menu.add("削除" to { confirmDeletePlan(id, name) })
        // 項目2: エッジ送信済み(ロック中)かつ未撮影なら「エッジ端末から削除」で外して編集可能にする。
        //  停止しただけの計画はエッジが保有し続けロックされたまま(項目4)なので、これで明示的に外す。
        // スマホで撮る計画(エッジ端末=「無し」)には出さない(2026-09-01 UI依頼)。
        //  isPlanOnEdge は「今カメラを使っている」だけでも true になるので、それだけでは
        //  スマホ直結の撮影中にもこの項目が出てしまう。エッジが持ち主のときだけ出す。
        val heldByEdge = planEdgeName(id).isNotEmpty() || edgeHeldByEdge.values.any { it.contains(id) }
        if (onEdge && !capturingNow && heldByEdge) menu.add("外部端末から削除" to { confirmRemoveFromEdge(id, name) })
        menu.add("過去の計画削除" to { confirmDeletePastPlans() })   // 終了日が過去の計画を一括削除(エッジ保有分は対象外)
        return menu
    }

    private fun selectPlanRow(id: String) {
        if (id == currentPlanId) return
        planExec.execute {
            HgeNative.nativeSelectPlan(id)   // EV_SCHEDULE で詳細表示が更新される
            runOnUiThread { currentPlanId = id; refreshPlanList(); reloadExpoEditors() }  // item3: 計画のカメラ/レンズに合わせ露出スライダ範囲を更新
        }
    }

    // ひな形から撮影計画を作り、そのまま撮影計画画面へ移る(画面は同じなのでモードを戻すだけ)。
    private fun newPlanFromTemplate(tplId: String) {
        planExec.execute {
            val r = HgeNative.nativeNewPlanFromTemplate(tplId)
            val cur = HgeNative.nativeCurrentPlanId()
            runOnUiThread {
                if (r != 0) { Toast.makeText(this, "作成に失敗しました (code=$r)", Toast.LENGTH_LONG).show(); return@runOnUiThread }
                tplMode = false                    // ひな形画面 → 撮影計画画面(同じページ)
                planIdBeforeTpl = ""
                currentPlanId = cur
                setPlanEdgeName(cur, "")           // 端末は「スマホ」から始める(ひな形は端末を持たない)
                refreshPlanList(); updateReadOnly(); applyTplMode(); reloadExpoEditors()
                Toast.makeText(this, "撮影計画を作りました", Toast.LENGTH_SHORT).show()
            }
        }
    }

    // 今の計画をひな形として保存する。名前を先に聞く(ひな形は一覧に出ないので後で直せない)。
    private fun saveTemplateFrom(planId: String, planName: String) {
        val et = EditText(this); et.setText(planName); et.isSingleLine = true
        val wrap = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(20), dp(8), dp(20), 0); addView(et) }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("ひな形に保存")
            .setView(wrap)
            .setPositiveButton("保存") { _, _ ->
                val nm = et.text.toString().trim()
                planExec.execute {
                    // 保存するのは「今の計画」なので、対象がいま開いている計画でなければ開いてから。
                    if (HgeNative.nativeCurrentPlanId() != planId) { HgeNative.nativeSelectPlan(planId) }
                    val r = HgeNative.nativeSaveTemplateFromPlan(nm)
                    runOnUiThread {
                        Toast.makeText(this, if (r == 0) "ひな形に保存しました" else "保存に失敗しました (code=$r)",
                                       Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    // ひな形を選んで、今の計画をその内容で更新する。名前・開始/終了時刻・端末は変わらない。
    private fun chooseTemplateForUpdate(planId: String) {
        planExec.execute {
            val arr = try { JSONArray(HgeNative.nativeListTemplates()) } catch (_: Exception) { JSONArray() }
            val ids = ArrayList<String>(); val labels = ArrayList<String>()
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                ids.add(o.optString("id")); labels.add(o.optString("planName"))
            }
            runOnUiThread {
                if (ids.isEmpty()) {
                    Toast.makeText(this, "ひな形がありません。⋮の「ひな形に保存」で作れます", Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                androidx.appcompat.app.AlertDialog.Builder(this)
                    .setTitle("ひな形で更新")
                    .setItems(labels.toTypedArray()) { _, w ->
                        planExec.execute {
                            val r = HgeNative.nativeUpdatePlanFromTemplate(planId, ids[w])
                            runOnUiThread {
                                if (r == 0) { refreshPlanList(); reloadExpoEditors() }
                                Toast.makeText(this, if (r == 0) "「" + labels[w] + "」で更新しました" else "更新に失敗しました (code=$r)",
                                               Toast.LENGTH_SHORT).show()
                            }
                        }
                    }
                    .setNegativeButton("やめる", null)
                    .show()
            }
        }
    }

    private fun confirmDeleteTemplate(id: String, name: String) {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("ひな形の削除")
            .setMessage("「" + name + "」を削除しますか？")
            .setPositiveButton("削除する") { _, _ ->
                planExec.execute {
                    HgeNative.nativeDeleteTemplate(id)
                    val cur = HgeNative.nativeCurrentPlanId()
                    val left = planIdsIn(HgeNative.nativeListTemplates())
                    runOnUiThread {
                        if (left.isEmpty()) { leaveTemplates { openGearMenu() } }   // 空になったら画面から出る
                        else { currentPlanId = cur; refreshPlanList(); reloadExpoEditors() }
                    }
                }
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    private fun doNewPlan() {
        planExec.execute {
            HgeNative.nativeNewPlan("")
            val cur = HgeNative.nativeCurrentPlanId()
            val list = buildReservations(); saveReservations(list)   // 項目17: 新規作成時に予約表を作り直す
            runOnUiThread { currentPlanId = cur; refreshPlanList() }
        }
    }

    // 2026-08-22 に廃止した restoreViewingSelection() の跡地。
    //  native の「選択中の計画」を操作の都合で動かしてから元へ戻していたが、戻すのが非同期なので
    //  ユーザーが一覧で計画を選ぶ操作と競り、詳細シートだけ前の計画に化けることがあった。
    //  操作側が id 指定で計画を扱うようにしたので(nativeCaptureStartPlan / nativeGetPlanJsonById)、
    //  選択は「ユーザーが一覧でタップしたとき」と「画面再入」だけが動かす。戻す処理は不要。

    // 計画のタイムゾーン(撮影場所が持つ)。分からなければ端末の値を返す。
    private fun planTzOffMin(id: String): Int {
        try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            for (k in 0 until pa.length()) {
                val po = pa.optJSONObject(k) ?: continue
                if (po.optString("id") == id) return po.optInt("tzOffMin", nowOffMin())
            }
        } catch (_: Exception) {}
        return nowOffMin()
    }

    // 【開始を指示したときだけ知らせる(2026-09-03 UI依頼)】計画の時刻は撮影場所の現地時刻なので、
    //  端末のタイムゾーンと違うと端末の時計とはずれた時刻に動く。**止めはしない**。
    //  画面に出しっぱなしにすると、タイムゾーンを直しても作り直すまで残って格好が悪い。
    //  一瞬出て消えるトーストなら、その場の状態だけを伝えられる。
    private fun warnPlanTzIfDiffers(id: String) {
        if (planTzOffMin(id) == nowOffMin()) { return }
        Toast.makeText(this, "スマホのタイムゾーンと異なるので実際の時刻とずれます。", Toast.LENGTH_LONG).show()
    }

    private fun startPlan(id: String) {
        warnPlanTzIfDiffers(id)
        // 項目A/17: 開始アイコンをタップした瞬間に予約表で二重使用を確かめる。同じカメラを別の計画が
        // 重なる時間で使うなら、開始も「エッジ端末への送信」も一切行わない(1件目は可・2件目以降は不可)。
        //  ・一度エッジに計画が入ると端末側で開始できてしまうため、送る前にここで確実に弾く。
        //  ・カメラ名はアプリに登録した「名称」(camName)を使う。
        reserveBlockedBy(id)?.let { _ ->
            val cam = try {
                val arr = JSONArray(HgeNative.nativeListPlans())
                (0 until arr.length()).asSequence().map { arr.optJSONObject(it) }
                    .firstOrNull { it?.optString("id") == id }
                    ?.let { it.optString("camName").ifEmpty { it.optString("camModel") } } ?: "カメラ"
            } catch (_: Exception) { "カメラ" }
            AlertDialog.Builder(this)
                .setTitle("開始できません")
                .setMessage("$cam は他の計画で使用中なので開始できません。")
                .setPositiveButton("OK", null)
                .show()
            return
        }
        stoppingPlans.remove(id)   // 再開する計画は「中止確定待ち」を解除(NOCAMERA抑止をリセット)
        // 即時フィードバック: タップの瞬間に待機(カメラ点灯)へ変える。「押したのが効いたか分からない」対策。
        // 発見/開始要求は後追いで行い、失敗したらここで足した待機を取り消してトーストで知らせる。
        // startingPlans: 開始要求がエッジへ届く前のポーリングが IDLE を拾って集合から外すレースを防ぐ。
        waitingPlans.add(id); startingPlans.add(id)
        refreshPlanList(); updateReadOnly()
        // この計画に指定されたエッジ端末の"名称"(無ければスマホで撮影)。
        val name = planEdgeName(id)
        if (name.isEmpty()) {
            // スマホで撮影。撮影開始要求(§7.4): 重なり2件超/受付100件超は受付時にエラー。
            planExec.execute {
                // 表示中の計画は動かさない。開始は id 指定でそのまま通る(hge_captureStartPlan)。
                val r = HgeNative.nativeCaptureStartPlan(id)
                runOnUiThread {
                    startingPlans.remove(id)   // 開始要求の結果確定
                    when (r) {
                        // 理由コードが付いていればそれを出す(同期撮影の単独実行など)。
                        // 判定と文言の対応は Entity 側が決めるので、ここは分岐しない。
                        HgeNative.ERR_OVERLAP_LIMIT -> {
                            waitingPlans.remove(id)
                            val nc = try { HgeNative.nativeLastStartNotice() } catch (_: Exception) { 0 }
                            val nn = try { HgeNative.nativeLastStartNoticeN1() } catch (_: Exception) { 0 }
                            val msg = if (nc != 0) noticeText(nc, nn.toLong())
                                      else "撮影期間が重なる計画は3件までです。時間をずらすか他の計画を停止してください"
                            Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                        }
                        HgeNative.ERR_QUEUE_FULL   -> { waitingPlans.remove(id); Toast.makeText(this, "撮影開始要求が上限(100件)に達しました", Toast.LENGTH_LONG).show() }
                        // 同期撮影の台数超過。上限は Entity(=撮影する端末)が持つので、
                        // ここは理由コードと台数を受け取って文章にするだけ。
                        HgeNative.ERR_SYNC_SHOT_LIMIT -> {
                            waitingPlans.remove(id)
                            val nn = try { HgeNative.nativeLastStartNoticeN1() } catch (_: Exception) { 0 }
                            Toast.makeText(this, noticeText(61, nn.toLong()), Toast.LENGTH_LONG).show()
                        }
                        else -> {}   // 待機はタップ時に反映済み。実状態はEV_STATEで即補正される
                    }
                    refreshPlanList(); updateReadOnly()
                }
            }
            return
        }
        // エッジで撮影。項目9: IPは記憶せず、開始時に必ずブロードキャストで端末名一致の個体を解決する。
        //  同名端末が見つからなければ開始しない(繋ぎ直しでのIP誤認を防ぐ)。見つからないときはポップアップで中止。
        Thread {
            val e = discoverEdgeByName(name)
            runOnUiThread {
                if (e == null) {
                    waitingPlans.remove(id); startingPlans.remove(id); refreshPlanList(); updateReadOnly()   // タップ時の即時反映を取り消す
                    AlertDialog.Builder(this)
                        .setTitle("外部端末が見つかりません")
                        .setMessage("外部端末「${name}」が見つからないため撮影を開始できません。\n端末の電源・ネットワーク接続を確認してください。")
                        .setPositiveButton("OK", null)
                        .show()
                    return@runOnUiThread
                }
                updateEdgeIp(name, e.ip, e.port)   // 解決したIPを保持(停止/状態確認に使う)
                // 項目5: planExec では計画JSONの取得だけを素早く行い、遅いネットワーク送信は edgeExec へ
                //  逃がす。これで送信中も計画選択(planExec)が詰まらず UI が固まらない。
                // 2026-08-22: 取得を id 指定にした。表示中の計画を一度も動かさないので、後から元へ
                //  戻す処理(旧 restoreViewingSelection)が要らなくなり、戻しがユーザーの選択と競って
                //  詳細だけ前の計画に化ける不具合が原理的に起きなくなる。
                planExec.execute {
                    val planJson = try { HgeNative.nativeGetPlanJsonById(id) } catch (_: Exception) { "" }
                    runOnUiThread { refreshPlanList(); updateReadOnly() }
                    edgeExec.execute { startOnEdge(e, id, planJson) }   // 遅いI/Oは専用スレッドで(planExecを塞がない)
                }
            }
        }.start()
    }

    // Phase3c: カメラ未検出(NOCAMERA)時の「継続/中止」ポップアップ。○○=計画カメラの愛称/名称。
    // 継続=即再探索(スマホ=pokeAcquire / エッジ=C_RESEARCH)、見つからなければ猶予後に再表示。
    // 中止=撮影停止。nocamDialogShown で多重表示を抑止する。UIスレッドから呼ぶこと。
    // 【選択中かどうかに関係なく名前を出す(2026-08-28 修正)】
    //  以前は「id == currentPlanId のときだけ」計画を読んでいた。currentPlanId は
    //  一覧で今選んでいる計画でしかないので、**選んでいない計画がカメラを見失うと
    //  名前の無い「カメラが見つかりません」になっていた**。2つ動かしていれば必ず
    //  片方はこれに当たる。計画は id で読めるので(表示中の選択は動かない)、常にそちらを引く。
    private fun planCameraLabel(id: String): String {
        val js = try {
            // 表示中の計画は編集途中の内容が正しいので、そのまま使う(ファイルを読まない)。
            if (id == currentPlanId) HgeNative.nativeGetPlanJson() else HgeNative.nativeGetPlanJsonById(id)
        } catch (_: Exception) { "" }
        try {
            val o = JSONObject(js)
            val cam = o.optJSONObject("camera")
            val label = listOf(cam?.optString("assignedName") ?: "", cam?.optString("name") ?: "", cam?.optString("model") ?: "")
                .firstOrNull { it.isNotEmpty() }
            if (!label.isNullOrEmpty()) return label
            // カメラ側に名前が1つも無いとき(機種名すら空)は、せめてどの計画かを言う。
            val pn = o.optString("planName")
            if (pn.isNotEmpty()) return "計画「$pn」のカメラ"
        } catch (_: Exception) {}
        return "カメラ"
    }

    // NOCAMERAダイアログの抑止フラグ解除＋表示中なら閉じる。状態が「未検出以外」(復帰/待機/撮影/IDLE)へ
    // 移ったとき、および停止確定時に呼ぶ。これで復帰しても閉じない/中止しても再表示される問題を根絶する。
    private fun clearNoCam(id: String) {
        nocamDialogShown.remove(id)
        nocamDialogs.remove(id)?.let { runCatching { if (it.isShowing) it.dismiss() } }
    }

    // 「中止」操作の共通処理。タップの瞬間にアイコンを撮影中→開始前へ戻し(即時フィードバック。
    // 「押したのが効いたか分からない」対策)、停止は非同期送信。停止(IDLE)が確定するまでは
    // stoppingPlans で NOCAMERA ダイアログとポーリングの再追加を抑止する(確定は reconcileEdgePlan /
    // EV_STATE の IDLE 到達。保険で一定時間後に強制掃除)。
    // 項目6: 進捗ステータス行(分割バー下)は「選択中の計画」に紐付ける。選択計画の状態で表示/非表示を決める。
    //  ・撮影中/待機/未検出 → その状態を表示(具体的な枚数は各ポーリングが currentPlanId 限定で上書きする)。
    //  ・それ以外(非稼働) → 非表示(前の計画や別カメラの表示を残さない)。2台撮影でも選択した方が出る。
    //  ステータス行の文言はここだけで作る。ポーリングやイベントは「集合と planProgress を更新して
    //  この関数を呼ぶ」だけにする(受信した場所ごとに文字列を組み立てると、経路によって出る内容が
    //  食い違い、切り替え直後だけ枚数が消える、といったことが起きる)。
    private fun refreshCaptureStatusForCurrent() {
        if (!::captureStatus.isInitialized) return
        val id = currentPlanId
        val onEdge = id.isNotEmpty() && planEdgeName(id).isNotEmpty()
        val sfx = if (onEdge) "(外部端末)" else ""
        fun show(text: String, color: Int) {
            captureStatus.text = text; captureStatus.setTextColor(color)
        }
        when {
            id.isEmpty() -> captureStatus.text = ""
            disconnectedPlans.contains(id) -> show("● カメラが見つかりません$sfx", 0xFFD32F2F.toInt())
            capturingPlans.contains(id)    -> {
                val head = if (onEdge) "● 外部端末で撮影中" else "● 撮影中"
                val p = planProgress[id]
                show(if (p != null) "$head  ${p.frame}/${p.total}枚  残り${p.remainSec}秒" else head, 0xFF2E7D32.toInt())
            }
            waitingPlans.contains(id) || startingPlans.contains(id) -> show("● 撮影開始待ち$sfx", 0xFF1565C0.toInt())
            else -> captureStatus.text = ""
        }
    }

    private fun beginStop(id: String, doStop: () -> Unit) {
        addHistory("user break", id)   // 項目9: ユーザー操作での中止(この後のIDLEを auto end にしない)
        histUserStop.add(id)
        nocamShownWaiting.remove(id)   // 項目11: 中止したら「待機中1回だけ」をリセット(次の開始要求で再び1回出す)
        stoppingPlans.add(id)
        capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id)   // 即時にアイコンを戻す
        clearNoCam(id)
        planProgress.remove(id)   // 中止した計画の枚数は捨てる
        if (capturingPlans.isEmpty()) stopBlink()
        if (currentPlanId == id) captureStatus.text = ""
        refreshPlanList(); updateReadOnly()
        Thread { runCatching { doStop() } }.start()
        // 保険: 一定時間内に IDLE を検知できなくても抑止/集合を掃除し、UIとポーリングを正常化する。
        handler.postDelayed({
            if (stoppingPlans.remove(id)) {
                capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); clearNoCam(id)
                if (capturingPlans.isEmpty()) stopBlink()
                if (currentPlanId == id) captureStatus.text = ""
                refreshPlanList(); updateReadOnly()
                if (activeEdgePlans().isEmpty()) handler.removeCallbacks(edgePoll)
            }
        }, 20000)
    }

    private fun showNoCameraDialog(id: String) {
        if (stoppingPlans.contains(id)) return      // 中止操作済み(停止確定待ち)は出さない
        if (nocamDialogShown.contains(id)) return   // 既に表示中/継続中は出さない
        // 項目11: 撮影開始前(待機中)のポップアップは最初の1回だけ。アイコンには×を出し続ける。
        //  待機中はカメラの状態が NOCAMERA↔SEARCHING を往復し、そのたび clearNoCam で抑止が
        //  解除されて繰り返し出ていた。撮影開始時刻を過ぎたら(実際にカメラが要る局面)再び毎回出す。
        run {
            val st = planStartMillis(id)
            if (st > 0L && System.currentTimeMillis() < st) {
                if (!nocamShownWaiting.add(id)) { return }   // 待機中は初回のみ
            }
        }
        // 項目8: 遠い未来の待機中(撮影窓がまだ先)は ×アイコンだけ出し、「見つかりません」ダイアログは出さない。
        //  ダイアログは撮影窓が近い/実行中(=実際にカメラが要る局面)でのみ。閾値=窓開始の120秒前。
        val sMs = planStartMillis(id)
        if (sMs > 0L && sMs - System.currentTimeMillis() > 120_000L) return
        nocamDialogShown.add(id)
        val cam = planCameraLabel(id)
        val e = planEdge(id)   // null=スマホ直接
        // 【理由が分かっているなら、そちらを出す】カメラは見つかっていて認証で弾かれている
        //  だけ、という場合がある。「見つかりません」と言うと電源やWi-Fiを疑って堂々巡りに
        //  なるので、分かっている理由を優先する(63=締め出し / 64=未登録 / 65=誤り)。
        val an = planAuthNotice[id] ?: 0
        val title = if (an != 0) "カメラに接続できません" else "カメラが見つかりません"
        val body  = if (an != 0) "${cam}: " + noticeText(an, 0)
                    else "${cam}が見つかりません。オンラインにしてください。"
        val dlg = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(body)
            .setCancelable(false)
            .setPositiveButton("継続") { d, _ ->
                d.dismiss(); nocamDialogs.remove(id)
                // 即再探索(取得フェーズの60秒待ちを前倒し)。ネットワークI/Oは別スレッド。
                Thread { if (e == null) HgeNative.nativePokeAcquire(id) else HgeNative.nativeEdgeResearch(e.addr(), e.port, id) }.start()
                // 猶予後もまだ未検出なら再度ポップアップ(継続の間は nocamDialogShown を維持して多重表示を防ぐ)。
                handler.postDelayed({
                    if (disconnectedPlans.contains(id) && !stoppingPlans.contains(id)) { nocamDialogShown.remove(id); showNoCameraDialog(id) }
                }, 12000)
            }
            .setNegativeButton("中止") { d, _ ->
                d.dismiss(); nocamDialogs.remove(id)
                stopPlanFromPhone(id)   // 項目6c: 停止+エッジ削除+ロック解除
            }
            .create()
        nocamDialogs[id] = dlg
        dlg.show()
    }

    // 項目6c: スマホから撮影を停止する。エッジ担当の計画は停止に加えてエッジからも削除し(=C_DELETE_PLAN)、
    //  エッジ担当(ロック)を解除する。これでスマホ側は再び編集/削除できる(計画はスマホに残る)。
    //  ※エッジ本体で停止した場合はエッジが計画を保持し続ける(再開可能)ので、この経路は通らない。
    private fun stopPlanFromPhone(id: String) {
        val name = planEdgeName(id)
        if (name.isEmpty()) { beginStop(id) { HgeNative.nativeCaptureStopPlan(id) }; return }
        // エッジ停止(再修正): 以前は planEdge() のキャッシュIPへ投げっぱなしで、戻り値も例外も捨てていた。
        //  IPが空/古いと停止はどこにも届かないのに UI だけ「停止した」表示になり、エッジは撮影を続ける
        //  (実機で発生: スマホは停止表示・Stick01 は waiting のまま)。開始時と同じくブロードキャストで
        //  端末名一致の個体を解決してから送り、結果を確認する。失敗したらユーザーに知らせる。
        beginStop(id) {
            val e = discoverEdgeByName(name)
            if (e == null) {
                runOnUiThread { onEdgeStopFailed(id, "外部端末「${name}」が見つかりません。") }
                return@beginStop
            }
            runOnUiThread { updateEdgeIp(name, e.ip, e.port) }
            // #4: 停止は撮影を止めるだけ。エッジからは削除せず、計画のエッジ選択も勝手に変えない。
            //  → エッジは計画を保有し続ける=ロック維持。編集したいときは「エッジ端末から削除」(#2)で明示的に外す。
            val r = HgeNative.nativeEdgeStop(e.addr(), e.port, id)
            if (r != 0) { runOnUiThread { onEdgeStopFailed(id, "停止を外部端末へ送れませんでした (code=${r})。") } }
        }
    }

    // エッジへの停止が届かなかったとき。UIの「停止した」表示を取り消し、ポーリングに実状態を拾わせる。
    //  勝手に停止済みへ倒すと、エッジで撮影が続いているのに画面上は止まって見える(最悪の状態)。
    private fun onEdgeStopFailed(id: String, why: String) {
        stoppingPlans.remove(id)   // 抑止を解除 → ポーリングがエッジの実状態(撮影中/待機)を反映する
        histUserStop.remove(id)
        refreshPlanList(); updateReadOnly()
        if (activeEdgePlans().isNotEmpty()) ensureEdgePoll()
        AlertDialog.Builder(this)
            .setTitle("停止できませんでした")
            .setMessage("${why}\n外部端末では撮影が続いている可能性があります。端末の電源とネットワークを確認して、もう一度お試しください。")
            .setPositiveButton("OK", null)
            .show()
    }

    // 項目2: エッジ端末から計画を外す(編集可能にする)。停止しただけの計画はエッジが保有し続けロックされている。
    //  エッジのその計画を停止(念のため)+削除する。次スイープでロスターから消え、即時にも保有台帳から外してロック解除。
    //  計画のエッジ選択(planEdgeName)は項目4に従い勝手に変えない(再送信できるよう保持)。
    private fun removeFromEdge(id: String) {
        val name = planEdgeName(id)
        if (name.isEmpty()) return
        // 停止と同じく、キャッシュIPではなくブロードキャストで解決してから送る(届かない削除を防ぐ)。
        Thread {
            val e = discoverEdgeByName(name)
            if (e == null) {
                runOnUiThread {
                    AlertDialog.Builder(this)
                        .setTitle("削除できませんでした")
                        .setMessage("外部端末「${name}」が見つかりません。\n端末の電源とネットワークを確認して、もう一度お試しください。")
                        .setPositiveButton("OK", null).show()
                    refreshPlanList(); updateReadOnly()   // 楽観的に外したロックを実状態へ戻す
                }
                return@Thread
            }
            runOnUiThread { updateEdgeIp(name, e.ip, e.port) }
            runCatching {
                HgeNative.nativeEdgeStop(e.addr(), e.port, id)          // 走っていなければ無害
                HgeNative.nativeEdgeDeletePlan(e.addr(), e.port, id)    // エッジから削除
            }
        }.start()
        // 即時ロック解除(保有台帳から外す)+過渡集合の掃除。削除失敗なら次スイープで再出現し自己修復。
        edgeHeldByEdge[name]?.remove(id)
        startingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); stoppingPlans.remove(id); clearNoCam(id)
        nocamShownWaiting.remove(id)   // 項目11: エッジから外したら「待機中1回だけ」もリセット
        refreshPlanList(); updateReadOnly()
        Toast.makeText(this, "外部端末から削除しました(編集できます)", Toast.LENGTH_SHORT).show()
    }

    private fun confirmRemoveFromEdge(id: String, name: String) {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("外部端末から削除")
            .setMessage("「$name」を外部端末から削除しますか？\n(待機中なら停止し、編集できるようになります。計画自体は残ります)")
            .setPositiveButton("削除") { _, _ -> removeFromEdge(id) }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    private fun confirmStop(id: String) {
        // 338.撮影中止 ダイアログ
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("撮影中止")
            .setMessage("撮影を中止しますか？")
            .setPositiveButton("中止する") { dlg, _ ->
                dlg.dismiss()   // 中止選択で即ダイアログを閉じる(停止処理の完了は待たない。指示4)
                stopPlanFromPhone(id)   // 項目6c: 停止+エッジ削除+ロック解除
            }
            .setNegativeButton("続ける", null)
            .show()
    }

    private fun copyPlanRow(id: String) {
        planExec.execute {
            HgeNative.nativeCopyPlan(id)
            val c = HgeNative.nativeCurrentPlanId()
            runOnUiThread {
                // エッジ端末の指定(Android prefs管理)はコピー元から引き継ぐ。他項目はentityが複製済み。
                if (c.isNotEmpty()) setPlanEdgeName(c, planEdgeName(id))
                currentPlanId = c; refreshPlanList()
            }
        }
    }

    // 項目6: 終了日が過去の撮影計画をすべて削除する(確認付き)。動作中(撮影/待機/操作過渡)の計画は対象外。
    private fun confirmDeletePastPlans() {
        val past = ArrayList<Pair<String, String>>()   // (id, name)
        try {
            val arr = JSONArray(HgeNative.nativeListPlans())
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = o.optString("id"); if (id.isEmpty()) continue
                if (o.optBoolean("capturable", true)) continue   // 終了が未来の計画は残す
                if (capturingPlans.contains(id) || waitingPlans.contains(id) || disconnectedPlans.contains(id) ||
                    startingPlans.contains(id) || stoppingPlans.contains(id)) continue   // 念のため動作中は残す
                if (isPlanOnEdge(id)) continue   // 項目6: エッジ保有(送信済み)の計画は一括削除の対象外
                past.add(id to o.optString("planName"))
            }
        } catch (_: Exception) {}
        if (past.isEmpty()) { Toast.makeText(this, "終了日が過去の計画はありません", Toast.LENGTH_SHORT).show(); return }
        AlertDialog.Builder(this)
            .setTitle("過去の計画削除")
            .setMessage("終了日が過去の撮影計画 ${past.size} 件をすべて削除しますか？")
            .setPositiveButton("削除する") { _, _ ->
                planExec.execute {
                    for ((id, _) in past) { HgeNative.nativeDeletePlan(id) }
                    val cur = HgeNative.nativeCurrentPlanId()
                    runOnUiThread { currentPlanId = cur; refreshPlanList(); reloadExpoEditors() }
                }
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    private fun confirmDeletePlan(id: String, name: String) {
        if (capturingPlans.contains(id) || waitingPlans.contains(id) || disconnectedPlans.contains(id)) { Toast.makeText(this, "撮影要求中は削除できません", Toast.LENGTH_SHORT).show(); return }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("削除")
            .setMessage("「$name」を削除しますか？")
            .setPositiveButton("削除") { _, _ ->
                planExec.execute {
                    HgeNative.nativeDeletePlan(id)
                    val c = HgeNative.nativeCurrentPlanId()
                    runOnUiThread { currentPlanId = c; refreshPlanList() }
                }
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // 撮影中カメラアイコンの点滅。
    private val planBlink = object : Runnable {
        override fun run() {
            blinkOn = !blinkOn
            for (i in 0 until planListContainer.childCount) {
                val row = planListContainer.getChildAt(i) as? LinearLayout ?: continue
                val ic = row.getChildAt(0) as? ImageView ?: continue
                val tag = ic.tag as? String
                if (tag != null && tag.startsWith("cam:")) ic.alpha = if (blinkOn) 1f else 0.25f
            }
            if (capturingPlans.isNotEmpty()) handler.postDelayed(this, 500)
        }
    }
    private fun startBlink() { handler.removeCallbacks(planBlink); blinkOn = true; handler.postDelayed(planBlink, 500) }
    private fun stopBlink() { handler.removeCallbacks(planBlink) }

    override fun onDestroy() {
        handler.removeCallbacks(edgePoll)
        handler.removeCallbacks(edgeSweep)
        handler.removeCallbacks(edgeTimeSync)
        handler.removeCallbacks(hgePump)
        stopEdgeApBinding()   // プロセスのネットワークバインドを解除(次アプリ/他通信のため)
        Thread { try { HgeNative.nativePresenceStop() } catch (_: Exception) {} }.start()  // P4: 常駐プレゼンスマップ停止
        HgeNative.nativeSetListener(null)
        HgeNative.nativeCaptureStop()
        HgeNative.nativeTerm()
        releaseMulticastLock()
        super.onDestroy()
    }

    // 【2026-08-06 廃止】カメラのIPをエッジへプッシュするのはやめた。
    //  ・エッジは自分でカメラを見つける: 起動時からの定期M-SEARCH(60秒)、撮影要求中のNOTIFY、
    //    そして接続に成功したIPの永続化(/asset/knownCams.json)。スマホの情報は要らない。
    //  ・複数エッジをそれぞれAPにして離れた場所のカメラを撮る構成では、スマホはカメラと同じ
    //    ネットワークに居ない。しかもESP32のSoftAPは既定でどのエッジも 192.168.4.x なので、
    //    別の場所のカメラのIPを配ることになり有害。将来スマホ⇄エッジをBLEにすると更に無意味。
    //  ・実測でもエッジのSSDP発見は1秒台で、ヒント無しで開始が遅くなることはない。
    // ETPの C_CAMERA_INFO とエッジ側の受信処理は残す(古いスマホと組み合わせても壊れないように)。

    // ② 未登録カメラの発見 → 所持カメラへ反映。既存(serial一致)は assignedName 更新、未定義の同機種枠は serial+assignedName を確定
    //  (どちらも自動)。いずれにも該当しない新規個体は「登録しますか？」を出す(拒否済みserialは自動プロンプトしない)。
    private fun reconcileDiscoveredCameras(presenceJson: String) {
        val arr = try { JSONArray(presenceJson) } catch (_: Exception) { return }
        Thread {
            val toPrompt = ArrayList<Triple<String, String, String>>()   // model, serial, assignedName
            for (i in 0 until arr.length()) {
                val c = arr.optJSONObject(i) ?: continue
                if (!c.optBoolean("online")) continue
                val serial = c.optString("serial"); if (serial.isEmpty()) continue
                if (declinedCamSerials.contains(serial)) continue        // 「いいえ」済みは自動では聞かない(手動登録は可)
                val model = c.optString("model"); val assignedName = c.optString("assignedName")
                val r = try { HgeNative.nativeRecordCameraIdentity(model, serial, assignedName, false) } catch (_: Exception) { -1 }
                if (r == 2) { toPrompt.add(Triple(model, serial, assignedName)) }   // 2=新規個体(未追加) → 登録可否を問う
            }
            if (toPrompt.isNotEmpty()) runOnUiThread { promptRegisterCameras(toPrompt) }
        }.start()
    }

    // 新規個体ごとに「登録しますか？」ダイアログを出す(どの画面でも表示)。登録=所持へ追加、いいえ=以後自動プロンプト抑止。
    private fun promptRegisterCameras(list: List<Triple<String, String, String>>) {
        for ((model, serial, assignedName) in list) {
            if (!promptingCamSerials.add(serial)) continue               // 既に表示中のserialは二重に出さない
            val label = if (assignedName.isNotEmpty()) assignedName else if (model.isNotEmpty()) model else serial
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle("カメラの登録")
                .setMessage("未登録のカメラ「$label」が見つかりました。所持カメラに登録しますか？")
                .setCancelable(false)
                .setPositiveButton("登録") { _, _ ->
                    promptingCamSerials.remove(serial)
                    Thread {
                        try { HgeNative.nativeRecordCameraIdentity(model, serial, assignedName, true) } catch (_: Exception) {}
                        runOnUiThread { if (flipper.displayedChild == 6) buildCameraList() }   // 6=所持カメラ一覧(openCameraList)
                    }.start()
                }
                .setNegativeButton("いいえ") { _, _ ->
                    promptingCamSerials.remove(serial)
                    declinedCamSerials.add(serial); saveDeclinedCamSerials()
                }
                .show()
        }
    }

    private fun loadDeclinedCamSerials(): MutableSet<String> =
        (hgcPrefs().getStringSet("declinedCamSerials", emptySet()) ?: emptySet()).toMutableSet()
    private fun saveDeclinedCamSerials() {
        hgcPrefs().edit().putStringSet("declinedCamSerials", declinedCamSerials).apply()
    }

    // 3b: SSDP受動待ち受け用の MulticastLock。Wi-Fi ドライバの受信フィルタを緩め、239.255.255.250:1900
    // への NOTIFY をネイティブ(ssdpListen*)が受信できるようにする。権限 CHANGE_WIFI_MULTICAST_STATE 必須。
    private var multicastLock: android.net.wifi.WifiManager.MulticastLock? = null
    private fun acquireMulticastLock() {
        try {
            if (multicastLock != null) return
            val wifi = applicationContext.getSystemService(android.content.Context.WIFI_SERVICE) as? android.net.wifi.WifiManager ?: return
            multicastLock = wifi.createMulticastLock("hgc-ssdp").apply { setReferenceCounted(false); acquire() }
        } catch (e: Exception) { /* 取得失敗時は受動待ち受け無し(60秒能動再探索で復帰) */ }
    }
    private fun releaseMulticastLock() {
        try { multicastLock?.let { if (it.isHeld) it.release() } } catch (e: Exception) {}
        multicastLock = null
    }

    // --- Entity通知 ---
    override fun onHgeEvent(event: Int, json: String) {
        // イベント1件の不正(エスケープ漏れ等で壊れたJSON)でアプリを道連れにしない。
        // 2026-07-26 18:15 実事故: EV_ERROR のJSONが壊れて JSONException→未捕捉→アプリ死亡→全撮影停止。
        // ネイティブ側(notifyError)もエスケープを直したが、ここは最後の防壁として残す。
        runOnUiThread {
            try { onHgeEventUi(event, json) }
            catch (e: Exception) { android.util.Log.e("HGC", "onHgeEvent failed ev=$event json=$json", e) }
        }
    }

    private fun onHgeEventUi(event: Int, json: String) {
            when (event) {
                HgeNative.EV_STATE -> {
                    val o = JSONObject(json)
                    val st = o.optInt("state", HgeNative.ST_IDLE)
                    val pid = o.optString("planId")
                    capState.text = "state: ${HgeNative.stateName(st)}"
                    if (pid.isNotEmpty()) histOnState(pid, st)   // 項目9: スマホ直接撮影の履歴
                    if (pid.isNotEmpty()) {
                        // 「中止」操作済みは停止(IDLE/ERROR)確定までNOCAMERAを無視(無限再表示を防ぐ)。
                        if (stoppingPlans.contains(pid) && st != HgeNative.ST_IDLE && st != HgeNative.ST_ERROR) {
                            // 抑止中: 何も表示更新しない(確定は下の IDLE/ERROR 分岐で行う)。
                        } else
                        // 状態→3集合(撮影中=点滅 / 待機=点灯 / 未検出=✖)。各計画はいずれか一つ。
                        when (st) {
                            HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                                capturingPlans.add(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
                                startBlink()
                            }
                            HgeNative.ST_WAITING, HgeNative.ST_SEARCHING -> {
                                waitingPlans.add(pid); capturingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                            }
                            HgeNative.ST_NOCAMERA, HgeNative.ST_DISCONNECTED -> {
                                disconnectedPlans.add(pid); capturingPlans.remove(pid); waitingPlans.remove(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                                showNoCameraDialog(pid)   // Phase3c: 継続/中止ポップアップ(多重抑止あり)
                            }
                            HgeNative.ST_IDLE, HgeNative.ST_ERROR -> {
                                stoppingPlans.remove(pid)
                                capturingPlans.remove(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid)
                                clearNoCam(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                            }
                        }
                        refreshPlanList(); updateReadOnly()
                    }
                    // 表示中の計画の状態をステータス表示(項目6: 選択中の計画に紐付く)。
                    refreshCaptureStatusForCurrent()
                }
                HgeNative.EV_PROGRESS -> {
                    val o = JSONObject(json)
                    capProgress.text = "frame ${o.optInt("frame")}/${o.optInt("total")}  " +
                        "elapsed ${o.optInt("elapsedSec")}s  remain ${o.optInt("remainSec")}s"
                    // 表示中かどうかに関わらず控える。描くのは refreshCaptureStatusForCurrent に任せる。
                    rememberProgress(o.optString("planId"), o)
                    refreshCaptureStatusForCurrent()
                }
                HgeNative.EV_CAPTURED -> {
                    val o = JSONObject(json)
                    capCaptured.text = "iso ${o.optString("iso")}  ss ${o.optString("ss")}  f ${o.optString("fn")}"
                    android.util.Log.i("HGCapture", "CAPTURED $json")
                }
                HgeNative.EV_SCHEDULE -> { latestSchedule = json; updatePlanDisplay(json) }
                HgeNative.EV_DEVICE -> {}
                HgeNative.EV_PRESENCE -> { reconcileDiscoveredCameras(json) }   // ②未登録カメラの反映/登録プロンプト(エッジへのIP pushは廃止)
                HgeNative.EV_ERROR -> {
                    val o = JSONObject(json)
                    // お知らせ(notice)は番号だけが届く。文言はこちら(UI)が持つ。
                    //  Entity と通信路に日本語を置かないため(2026-08-19 方針)。
                    val nt = o.optInt("notice", 0)
                    val msg = if (nt != 0) noticeText(nt, o.optLong("n1", 0)) else o.optString("msg")
                    capState.text = "ERROR $msg"
                    // カメラ未検出・カメラ使用中など、撮影開始の失敗をユーザーへ通知する。
                    if (msg.isNotEmpty()) Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                }
            }
    }

    // お知らせ番号(Entity の hgc::notice)を人が読む文にする。**文言はここだけが持つ**。
    //  Entity と通信路には番号しか流れないので、言い換えも多言語化もここの差し替えで済む。
    //  知らない番号が届いたら番号のまま出す(古いアプリに新しい通知が来ても壊れない)。
    private fun noticeText(code: Int, n1: Long): String = when (code) {
        10 -> "カメラがカードに記録できません。カードの残量と書き込み保護を確認してください"
        11 -> "カメラにカードが入っていません"
        12 -> "カメラのカードが書き込み禁止になっています"
        13 -> "カメラのカード残量が少なくなっています(あと約${n1}枚)"
        20 -> "カメラの電池が残りわずかです"
        21 -> "カメラが高温になっています"
        22 -> "カメラが高温のため撮影できません。冷めるまで待ってください"
        40 -> "外部端末に保存できません。SDカードが入っているか確認してください"
        30 -> "カメラの状態が元に戻りました"
        50 -> "カメラの撮影が復帰しました"
        51 -> "カメラが撮影を完了しません(シャッターは通るのに画像が記録されません)。オフラインとして表示します"
        52 -> "撮影中にカメラとの接続が切れました。中止するまで再接続を試み続けます"
        53 -> "待機中に接続が切れたため再接続しました"
        54 -> "1枚目の露出をカメラへ設定できませんでした(${n1}回試行)。撮影は続けます"
        55 -> "撮影開始前の露出合わせに失敗しました。ログに内訳が残っています"
        60 -> "このカメラは別の撮影で使用中です"
        // 台数の上限は端末(エッジ/スマホ)が決めて n1 で送ってくる。ここでは埋めるだけで、
        // 数字をアプリに持たない(端末の仕様が変わってもアプリを直さずに済む)。
        61 -> "同期撮影のカメラが多すぎます。この端末で撮れるのは${n1}台までです"
        62 -> "同期撮影は単独で行います。時間が重なる撮影を止めるか、時間をずらしてください"
        63 -> "カメラが接続を拒否しています。カメラ本体のWi-Fi設定を一度削除して入れ直してください(認証情報の登録漏れが原因のことがあります)"
        64 -> "カメラの認証情報が登録されていません。機材のカメラ設定にユーザーIDとパスワードを入れてください"
        65 -> "カメラの認証情報が正しくありません。機材のカメラ設定のユーザーIDとパスワードを確認してください"
        else -> "カメラからのお知らせ($code)"
    }

    // スケジュールJSON(静的フィールド+events+windows)から両画面の表示を更新する。
    private fun updatePlanDisplay(json: String) {
        try {
            val o = JSONObject(json)
            capName.text = o.optString("planName")
            // 選択中計画の開始/終了をピッカー用カレンダーへ同期(計画切替時に時刻表示を追従)。
            try {
                fmtIso.parse(o.optString("start"))?.let { startCal.time = it }
                fmtIso.parse(o.optString("end"))?.let { endCal.time = it }
                updateTimeButtons()
            } catch (_: Exception) {}
            placeText.text = o.optString("place")
            latlngText.text = o.optString("latlng") + "  標高 " + o.optInt("altitude") + "m"
            // 同機種を複数台持つと名称だけでは区別できないので、カメラ本体で付けた名前を添える。
            val camAn = o.optString("cameraAssignedName")
            // 見出しはレイアウト側にあるので、ここは中身だけを入れる。
            cameraText.text = o.optString("camera") + (if (camAn.isNotEmpty()) "  ($camAn)" else "")
            lensText.text = o.optString("lens")
            // センサー/焦点距離/画角は撮影シミュレーション画面へ移した(2026-08-08 UI依頼)。
            // センサー寸法はマスターにある機種しか分からない。未登録のときに 0.0×0.0 と出すと
            // 値が入っているように見えるので、寸法も画角も出さない(2026-08-19)。
            val sw = o.optDouble("sensorW"); val sh = o.optDouble("sensorH")
            simGearText = if (sw > 0.0 && sh > 0.0)
                "センサー %.1f×%.1fmm  焦点距離 %d mm  画角 %.0f×%.0f°".format(
                    sw, sh, o.optInt("focalLength"), o.optDouble("fovH"), o.optDouble("fovV"))
            else
                "センサー 未登録  焦点距離 %d mm  画角 ---".format(o.optInt("focalLength"))
            simPage?.setGearText(simGearText)
            intervalText.text = fmtInterval(o.optDouble("interval", 15.0)) + "秒"
            suppressLandscape = true
            landscapeCheck.isChecked = o.optBoolean("landscape")
            suppressLandscape = false
            // 同期撮影と追加カメラ(2026-08-25)。
            val pano = o.optBoolean("syncShot")
            suppressSyncShot = true
            syncShotCheck.isChecked = pano
            suppressSyncShot = false
            subCamRow.visibility = if (pano) View.VISIBLE else View.GONE
            run {
                val arr = o.optJSONArray("subCameras")
                planSubCamNames = mutableListOf()
                val labels = mutableListOf<String>()
                if (arr != null) {
                    for (i in 0 until arr.length()) {
                        val c = arr.optJSONObject(i) ?: continue
                        val nm = c.optString("name")
                        if (nm.isEmpty()) continue
                        planSubCamNames.add(nm)
                        val an = c.optString("assignedName")
                        labels.add(if (an.isNotEmpty()) "$nm ($an)" else nm)
                    }
                }
                subCamText.text = if (labels.isEmpty()) "未選択" else labels.joinToString(", ")
            }
            // npf が負 = センサー寸法/画素数が未登録で算出できない(Entityが -1 を返す)。
            val npf = o.optDouble("npf", -1.0)
            npfText.text = if (npf >= 0.0) "NPF %.1f秒   最小周期 %d秒".format(npf, o.optInt("minInterval"))
                           else "NPF ---   最小周期 %d秒".format(o.optInt("minInterval"))
            // 項目11: 計画1ページ目の方向/仰角ウィジェットは廃止。撮影中画面の表示だけ残す。
            val az = o.optDouble("azimuth", 90.0)
            val el = o.optDouble("elevation", 10.0)
            capGear.text = o.optString("camera") + " / " + o.optString("lens")
            capDir.text = "撮影方向 %.1f°   仰角 %.1f°".format(az, el)
            renderOverview(o)                      // 先頭ページ: 概要スケジュール(表示専用)
            rebuildTwilightPages(o)                // 薄明ページ(横スライド)を再構築し、ページ番号を更新
            renderSchedule(capSchedule, o, true)   // 撮影画面は従来のイベント時系列
            refreshReservations()                  // 項目17: 計画を更新したら予約表を作り直す(過去分は落ちる)
        } catch (_: Exception) {}
    }

    // 一覧で計画名を編集中なら、その場で確定させる。改名は「入力欄のフォーカスが外れた時」に
    //  走るが、**ダイアログはアクティビティのフォーカスを奪わない**ので、名前を打った直後に
    //  ダイアログを開く操作では確定しないまま残る(入れたはずの名前が消えたように見える)。
    //  計画を書き換えるダイアログを出す前に呼ぶ。
    private fun commitPlanNameEdit() = commitListNameEdit(R.id.plan_listContainer)

    // --- 撮影場所の入力(緯度経度)。登録済みから選択 / テキスト貼り付け / 地図から選択(osmdroid) ---
    private fun showPlaceEditChooser() {
        commitPlanNameEdit()   // 名前を打った直後でも改名を取りこぼさない
        val cur = try { JSONObject(latestSchedule).optString("latlng") } catch (_: Exception) { "" }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("撮影場所を設定")
            .setItems(arrayOf("登録済みの場所から選択", "テキストで貼り付け", "地図から選択")) { _, which ->
                when (which) {
                    0 -> choosePlanFromRegistered()
                    1 -> showPlacePasteDialog(cur) { lat, lng -> applyPlace(lat, lng, "") }
                    2 -> { val s = parseLatLng(cur); openMapPicker(s?.first ?: 35.681, s?.second ?: 139.767) { lat, lng -> applyPlace(lat, lng, "") } }
                }
            }
            .show()
    }

    // 登録済み撮影場所(§7.9)から選んで撮影計画へ反映する。
    private fun choosePlanFromRegistered() {
        val arr = placeArray(HgeNative.nativeGetPlaces())
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }.filter { it.isNotEmpty() }
        if (names.isEmpty()) { Toast.makeText(this, "登録された場所がありません。メニューの「撮影場所」で追加してください", Toast.LENGTH_LONG).show(); return }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("登録済みの場所")
            .setItems(names.toTypedArray()) { _, which ->
                val nm = names[which]
                planExec.execute {   // 計画への書き込みは必ず単一スレッドで(他の計画操作と直列化)
                    val r = HgeNative.nativeSetPlanPlace(nm)
                    runOnUiThread {
                        // 詳細の表示は EV_SCHEDULE が更新する(ここで二重に出さない)。場所が変わると
                        //  タイムゾーンも変わりうるので一覧だけ作り直す(選択は動かない)。
                        if (r == 0) { refreshPlanList(); Toast.makeText(this, "撮影場所: $nm", Toast.LENGTH_SHORT).show() }
                        else Toast.makeText(this, "撮影場所の設定に失敗 (code=$r)", Toast.LENGTH_LONG).show()
                    }
                }
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // "35.6810, 139.7670" 等から緯度・経度を取り出す(カンマ/空白/余分な文字に寛容)。範囲外は null。
    private fun parseLatLng(s: String): Pair<Double, Double>? {
        // 全角の数字/符号/小数点(IMEやコピー由来)を半角へ正規化してから抽出。
        val norm = buildString {
            for (c in s) append(when (c) {
                in '０'..'９' -> '0' + (c - '０')
                '．', '。' -> '.'
                '＋' -> '+'
                '－', '−', 'ー' -> '-'
                '，' -> ','
                else -> c
            })
        }
        val nums = Regex("[-+]?\\d+(?:\\.\\d+)?").findAll(norm).mapNotNull { it.value.toDoubleOrNull() }.toList()
        if (nums.size < 2) return null
        val lat = nums[0]; val lng = nums[1]
        if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0) return null
        return Pair(lat, lng)
    }

    private fun showPlacePasteDialog(initial: String, onParsed: (Double, Double) -> Unit) {
        val et = EditText(this).apply { hint = "例: 35.6810, 139.7670"; setText(initial) }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL; setPadding(dp(24), dp(8), dp(24), dp(4))
            addView(TextView(this@MainActivity).apply {
                text = "緯度, 経度 を貼り付け（Googleマップの座標をそのまま貼れます）"; textSize = 12f; setTextColor(0xFF888888.toInt())
            })
            addView(et)
        }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("緯度経度を貼り付け")
            .setView(box)
            .setPositiveButton("設定") { _, _ ->
                val p = parseLatLng(et.text.toString())
                if (p == null) Toast.makeText(this, "緯度経度を認識できません（例: 35.681, 139.767）", Toast.LENGTH_LONG).show()
                else onParsed(p.first, p.second)
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // 緯度経度を Entity へ反映(スケジュール再生成)。name 空=地名は据え置き。
    // 【計画への書き込みは必ず planExec(2026-09-04 UI依頼)】計画の操作は単一スレッドで直列化する
    //  決まりなのに、撮影場所の2経路だけ生スレッドで走っていた。名前の確定(改名)と同時に走ると
    //  Entity の g_plan を二方向から触ることになり、一覧の選択が別の計画へ飛ぶ原因になっていた。
    //  詳細の表示は EV_SCHEDULE が受け持つ(ここで別途 updatePlanDisplay しない)。
    private fun applyPlace(lat: Double, lng: Double, name: String) {
        planExec.execute {
            val r = HgeNative.nativeSetPlanLocation(lat, lng, name)
            runOnUiThread {
                if (r == 0) Toast.makeText(this, "撮影場所を更新: %.4f, %.4f".format(lat, lng), Toast.LENGTH_SHORT).show()
                else Toast.makeText(this, "撮影場所の設定に失敗 (code=$r)", Toast.LENGTH_LONG).show()
            }
        }
    }

    // 地図から選択(osmdroid=OpenStreetMap。タイルは無料・APIキー不要)。タップで地点を選び「設定」で反映。
    private fun openMapPicker(startLat: Double, startLng: Double, onPick: (Double, Double) -> Unit) {
        org.osmdroid.config.Configuration.getInstance().apply {
            userAgentValue = packageName                 // OSMタイルサーバは UserAgent 必須
            osmdroidBasePath = java.io.File(cacheDir, "osmdroid")
            osmdroidTileCache = java.io.File(cacheDir, "osmdroid/tiles")
        }
        val map = org.osmdroid.views.MapView(this).apply {
            setTileSource(org.osmdroid.tileprovider.tilesource.TileSourceFactory.MAPNIK)
            setMultiTouchControls(true); minZoomLevel = 3.0
        }
        val start = org.osmdroid.util.GeoPoint(startLat, startLng)
        map.controller.setZoom(12.0); map.controller.setCenter(start)
        val marker = org.osmdroid.views.overlay.Marker(map).apply { position = start; setAnchor(0.5f, 1.0f) }
        map.overlays.add(marker)
        var picked = start
        val recv = object : org.osmdroid.events.MapEventsReceiver {
            override fun singleTapConfirmedHelper(p: org.osmdroid.util.GeoPoint): Boolean { picked = p; marker.position = p; map.invalidate(); return true }
            override fun longPressHelper(p: org.osmdroid.util.GeoPoint): Boolean { picked = p; marker.position = p; map.invalidate(); return true }
        }
        map.overlays.add(0, org.osmdroid.views.overlay.MapEventsOverlay(recv))
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(TextView(this@MainActivity).apply { text = "地図をタップして撮影場所を選択"; textSize = 12f; setPadding(dp(16), dp(8), dp(16), dp(4)); setTextColor(0xFF888888.toInt()) })
            // 地図は残り全部(高さ0＋weight)。ダイアログ自体を画面いっぱいにするので、
            //  題とボタンを除いた分を地図が使う(2026-09-04 UI依頼)。
            addView(map, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                                                 ViewGroup.LayoutParams.MATCH_PARENT)
        }
        val dlg = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("地図から選択")
            .setView(box)
            .setPositiveButton("この地点に設定") { _, _ -> onPick(picked.latitude, picked.longitude) }
            .setNegativeButton("キャンセル", null)
            .create()
        dlg.setOnDismissListener { map.onPause(); map.onDetach() }
        dlg.show()
        // 【地図は画面いっぱいに(2026-09-04 UI依頼)】狭いと目的の地点まで何度も動かすことになる。
        //  **枠外が無くなるので枠外タッチでは戻れない**。戻るのは「キャンセル」か端末の戻るキー。
        dlg.window?.apply {
            // ダイアログ既定の背景には余白(影と角丸)が入っていて、その分だけ地図が狭くなる。
            //  無地に差し替えて左右いっぱいまで使う。
            setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.WHITE))
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
        }
        map.onResume()
    }

    // ============================================================
    //  640 撮影場所リスト(§7.9)。登録した場所を撮影計画で選択する。
    // ============================================================
    // 【マスタデータの書き込みは単一スレッドで直列化する(2026-09-04 UI依頼)】
    //  撮影場所・所持カメラ・所持レンズは「名前」を宛先にして書く。改名と中身の保存が
    //  別スレッドで走ると、改名が先に通ったときに後から来た書き込みが**古い名前**を
    //  宛先にしてしまい、Entity がその名前のデータを新しく作る(一覧に行が増えて
    //  選択が別の項目へ移ったように見える)。1本の列に並べて順序を決める。
    private val dataExec = java.util.concurrent.Executors.newSingleThreadExecutor()

    // 一覧で編集中の名前を確定させる処理(選択行の入力欄を作るときに入れる)。
    //  改名は「入力欄のフォーカスが外れた時」に走るが、**ダイアログはアクティビティの
    //  フォーカスを奪わない**ので、名前を打った直後にダイアログを開く流れでは確定が
    //  後回しになる。名前を宛先にして書く前に、必ずここを通して先に確定させる。
    //  一覧ごとに1つ持つ(キー=一覧の入れ物のid)。画面ごとに変数を分けていたのをやめた。
    private val pendingRename = HashMap<Int, () -> Unit>()

    private fun commitListNameEdit(containerId: Int) {
        val f = pendingRename.remove(containerId) ?: return
        clearListFocus(findViewById(containerId)); f.invoke()
    }

    // 一覧の入力欄からフォーカスを外し、キーボードも閉じる。
    //  **removeAllViews() は、消す子にフォーカスが残っているとフォーカス探索が落ちる**
    //  (addFocusables で NPE。実機で確認)。作り直す前と、確定の前に必ず通す。
    // ================= 一覧(上)の共通部品(2026-09-04 UI依頼) =================
    // 「上=一覧 / 分割バー / 下=詳細」の画面は形が同じなのに、一覧を組み立てる糊だけが
    //  画面ごとに写し取られていた。同じ不具合を何度も直す元になっていたのでここへまとめる。
    //  枠(分割バー・横向きの左右分割・初期の高さ)は元から共通(setupDivider/applyMasterDetail)。
    //  **詳細の側は画面ごとに中身が別物なので共通化しない**。
    //  ※撮影計画の一覧は載せない: キーが id(他は名前)で、状態アイコン・点滅・撮影中の編集不可が
    //    あり、条件を持ち込むとこの部品が壊れやすくなる。枠だけ共有して一覧は独自のままにする。
    private class ListItem(
        val key: String,                                  // 選択・改名の宛先(いまはどれも名前)
        val title: String,
        val sub: String,
        val menu: List<Pair<String, () -> Unit>> = emptyList(),
        val mark: String = "")                            // 行頭の印(★など)

    private class ListPane(
        val containerId: Int,
        val rows: () -> List<ListItem>,
        val selected: () -> String?,
        val setSelected: (String?) -> Unit,               // 選択が消えたときの付け替え(保存は走らせない)
        val onSelect: (String) -> Unit,
        val onRename: ((String, String) -> Unit)? = null, // null=名前を直せない一覧
        val addLabel: String = "",
        val onAdd: (() -> Unit)? = null,                  // null=追加行を出さない
        val emptyText: String = "")

    // 一覧を組み立てる。行の見た目は listRow、区切りは thinDivider(他の一覧と同じ)。
    //  ・選択が消えていたら先頭へ付け替える(保存は走らせない)
    //  ・改名は実際に変わるときだけ一度だけ。確定処理は保持し、別の操作の前に必ず通す
    //  ・作り直す前にフォーカスを外す(消す子にフォーカスが残っていると Android が落ちる)
    //  ・「＋ 新規◯◯」は**必ず一覧の一番下**(画面によって上下していたのを揃えた)
    private fun renderList(p: ListPane) {
        val box = findViewById<LinearLayout>(p.containerId) ?: return
        clearListFocus(box)
        box.removeAllViews()
        pendingRename.remove(p.containerId)
        val items = p.rows()
        val keys = items.map { it.key }
        if (p.selected() == null || p.selected() !in keys) { p.setSelected(keys.firstOrNull()) }
        if (items.isEmpty() && p.emptyText.isNotEmpty()) {
            box.addView(TextView(this).apply {
                text = p.emptyText; setPadding(dp(8), dp(6), 0, dp(6)); setTextColor(Color.GRAY)
            })
        }
        for (it in items) {
            var renamed = false
            val doRename = { newName: String ->
                val nm = newName.trim()
                if (!renamed && nm.isNotEmpty() && nm != it.key) { renamed = true; p.onRename?.invoke(it.key, nm) }
            }
            box.addView(listRow(it.title, it.sub, it.key == p.selected(),
                onSelect = { commitListNameEdit(p.containerId); p.onSelect(it.key) },
                menuItems = it.menu,
                onRename = if (p.onRename == null) null else ({ newName -> doRename(newName) }),
                onEditor = { e -> pendingRename[p.containerId] = { doRename(e.text.toString()) } },
                mark = it.mark))
            box.addView(thinDivider())
        }
        if (p.onAdd != null) {
            box.addView(linkText(p.addLabel) { commitListNameEdit(p.containerId); p.onAdd.invoke() })
        }
    }

    private fun clearListFocus(box: View?) {
        if (box == null) { return }
        val f = currentFocus ?: return
        var v: View? = f
        while (v != null && v !== box) { v = v.parent as? View }
        if (v !== box) { return }                       // その一覧の入力欄ではない
        box.isFocusableInTouchMode = true
        box.requestFocus()
        (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
            .hideSoftInputFromWindow(box.windowToken, 0)
    }
    private var selPlace: String? = null
    private var placeLat = 0.0
    private var placeLng = 0.0
    private var placeCoordTv: TextView? = null
    private var placeAltEt: EditText? = null
    private var placeTzOffMin: Int = 0      // その場所のタイムゾーン(UTCからの分。東が正)
    private var placeMemoEt: EditText? = null
    private var placeAutoCb: CheckBox? = null

    private fun placeArray(json: String): JSONArray = try { JSONArray(json) } catch (e: Exception) { JSONArray() }
    private fun placeNames(): List<String> { val a = placeArray(HgeNative.nativeGetPlaces()); return (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optString("name") } }

    private fun openPlacesList() {
        buildPlacesList(); buildPlaceDetail()
        setInitialSplit(R.id.places_container)
        flipper.displayedChild = 11
    }
    private fun leavePlacesList(dest: Int = kScreenMenu) { stopDirtyWatch(); persistPlaceDetail(false); gotoScreen(dest) }

    // ============================================================
    //  650 カメラ予約表(項目17)
    // ============================================================
    // 同じカメラを複数の端末(エッジ/スマホ)で同時に使ってしまう事故を防ぐための一覧。
    //  ・対象: 撮影終了が未来の計画だけ(過去のものは作り直しのたびに捨てる)。
    //  ・並び: カメラごとにまとめ、その中は開始時刻の昇順。
    //  ・重なり: 同じカメラで使用時刻が重なる計画は色を変え、行頭に ✕ を付ける。
    //  ・開始時: 重なる相手が「別の端末」なら開始もエッジへの送信も行わない(警告を出す)。
    //  ・保存: 撮影計画の新規作成/更新のたびに camReserve.json へ作り直す(§7.6 の /asset 配下)。
    private data class Reservation(
        val planId: String, val planName: String,
        val camKey: String, val camLabel: String,   // camKey=保存/表示用の識別文字列 / camLabel=帯の表示
        val camModel: String, val camSerial: String, // 同一機体の判定はこの2つで行う(下 sameCamera)
        val edge: String,                            // 端末名(空=スマホで撮影)
        val startMs: Long, val endMs: Long,
        var conflict: Boolean = false)

    // 2つの予約が「同じ機体」を指すか。
    //  シリアルが両方とも分かっている時だけシリアルで判定し(同機種の別ボディを区別)、
    //  片方でも未確定なら機種一致で同一とみなす。
    //  ※camKey の文字列一致で判定すると、一度も接続していない計画(シリアル空)が
    //    接続済みの計画(シリアル有り)と別カメラ扱いになり、同じカメラの重複を見逃す。
    private fun sameCamera(a: Reservation, b: Reservation): Boolean =
        if (a.camSerial.isNotEmpty() && b.camSerial.isNotEmpty()) a.camSerial == b.camSerial
        else a.camModel.isNotEmpty() && a.camModel == b.camModel

    private val reserveFmt = SimpleDateFormat("yyyy.MM.dd HH:mm", Locale.JAPAN)
    // 計画一覧の start/end は Entity の dtToStr 形式("yyyy-MM-ddTHH:mm:ss"。T区切り)。
    private val planDtFmt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", Locale.JAPAN)

    // 項目8: 計画の撮影窓開始時刻(ms)。無ければ0。ダイアログ抑止判定に使う。

    // ロック判定に使う計画の JSON。**編集中の計画はファイルが古い**ので、そのときだけ
    //  メモリ上の編集内容を使う(周期や終了時刻を変えた直後に古い値で判定しないため)。
    //  planCameraLabel と同じ使い分け。
    private fun planJsonForLock(id: String): String =
        try {
            if (id.isNotEmpty() && id == currentPlanId) HgeNative.nativeGetPlanJson()
            else HgeNative.nativeGetPlanJsonById(id)
        } catch (_: Exception) { "" }

    // 計画JSONの日時({"year":…,"month":…,…}。Entity の dtToJson 形式)を ms へ。
    //  一覧JSONの "yyyy-MM-ddTHH:mm:ss" とは別形式なので planDtFmt は使えない。
    //  端末のタイムゾーンで解釈する(計画の日時は現地時刻。仕様どおり Android は端末TZ)。
    //  1つでも欠けている/おかしい値があれば 0 を返し、呼ぶ側は安全側へ倒す。
    private fun planDtMillis(o: JSONObject?): Long {
        if (o == null) return 0L
        val y  = o.optInt("year", 0)
        val mo = o.optInt("month", 0)
        val d  = o.optInt("day", 0)
        val h  = o.optInt("hour", -1)
        val mi = o.optInt("min", -1)
        val se = o.optInt("sec", -1)
        if (y < 1970 || mo !in 1..12 || d !in 1..31) return 0L
        if (h !in 0..23 || mi !in 0..59 || se !in 0..60) return 0L
        val c = Calendar.getInstance()
        c.clear()
        c.set(y, mo - 1, d, h, mi, se)
        return c.timeInMillis
    }

    // その計画のロックがまだ効いているか。窓の終了から猶予を過ぎたものは、エッジから
    //  何も聞けていなくても解除する(エッジが壊れた/持ち出したまま等で詰まないため)。
    //  終了時刻も撮影周期も**計画そのもの**から読む(同じ値を一覧JSONへ複製しない)。
    private fun planLockActive(id: String): Boolean {
        val o = try { JSONObject(planJsonForLock(id)) } catch (_: Exception) { null }
            ?: return true                           // 計画が読めないなら安全側=ロック継続
        val end = planDtMillis(o.optJSONObject("end"))
        if (end <= 0L) return true                   // 期限が読めないなら安全側=ロック継続
        val iv = o.optDouble("interval", 0.0)
        val grace = if (iv.isFinite() && iv > 0.0) (iv * 2.0 * 1000.0).toLong()
                    else kLockGraceFallbackMs        // 周期が読めないときだけ既定値
        return System.currentTimeMillis() < end + grace
    }

    private fun planStartMillis(id: String): Long {
        return try {
            val arr = JSONArray(HgeNative.nativeListPlans())
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                if (o.optString("id") == id) return planDtFmt.parse(o.optString("start"))?.time ?: 0L
            }
            0L
        } catch (_: Exception) { 0L }
    }

    // 計画一覧(nativeListPlans)から予約表を作る。終了が過去の計画は除外し、重なりを判定して印を付ける。
    private fun buildReservations(): List<Reservation> {
        val now = System.currentTimeMillis()
        val list = ArrayList<Reservation>()
        try {
            val arr = JSONArray(HgeNative.nativeListPlans())
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = o.optString("id"); if (id.isEmpty()) continue
                val s = planDtFmt.parse(o.optString("start"))?.time ?: continue
                val e = planDtFmt.parse(o.optString("end"))?.time ?: continue
                if (e <= now) continue                      // 終了が過去 → 予約表に入れない
                val model = o.optString("camModel").ifEmpty { o.optString("camName") }
                val serial = o.optString("camSerial")
                val assignedName = o.optString("camAssignedName")
                if (model.isEmpty() && serial.isEmpty()) continue   // カメラ未指定の計画は対象外
                // 同一機体の判定: シリアルがあればそれが最優先(機種違いの同シリアル衝突は model と併用)。
                val key = if (serial.isNotEmpty()) "$model#$serial" else model
                val label = buildString {
                    append(model.ifEmpty { "(カメラ未設定)" })
                    if (assignedName.isNotEmpty()) append("  $assignedName")
                    if (serial.isNotEmpty()) append("  Sn:$serial")
                }
                list.add(Reservation(id, o.optString("planName"), key, label, model, serial, planEdgeName(id), s, e))
            }
        } catch (_: Exception) {}
        list.sortWith(compareBy({ it.camLabel }, { it.startMs }))
        // 同一カメラ内で時間が重なる組に印を付ける(半開区間[start,end)で判定)。
        for (i in list.indices) {
            for (j in i + 1 until list.size) {
                if (!sameCamera(list[i], list[j])) continue
                if (list[i].startMs < list[j].endMs && list[j].startMs < list[i].endMs) {
                    list[i].conflict = true; list[j].conflict = true
                }
            }
        }
        return list
    }

    // 予約表をファイルへ保存する(仕様: ファイルに作成された予約表を表示する)。
    // 表示自体は常に最新の計画から作り直すので、この保存は記録・外部確認用。
    private fun saveReservations(list: List<Reservation>) {
        try {
            val arr = JSONArray()
            for (r in list) {
                arr.put(JSONObject().apply {
                    put("planId", r.planId); put("plan", r.planName)
                    put("camera", r.camLabel); put("camKey", r.camKey)
                    put("edge", r.edge)
                    put("start", reserveFmt.format(java.util.Date(r.startMs)))
                    put("end", reserveFmt.format(java.util.Date(r.endMs)))
                    put("conflict", r.conflict)
                })
            }
            val dir = java.io.File(getExternalFilesDir(null), "asset").apply { mkdirs() }
            java.io.File(dir, "camReserve.json").writeText(arr.toString())
        } catch (_: Exception) {}
    }

    // 撮影計画を新規作成/更新したときに呼ぶ(仕様の更新タイミング)。過去の物はここで落ちる。
    private fun refreshReservations() {
        planExec.execute {
            val list = buildReservations()
            saveReservations(list)
        }
    }

    private fun openReserveTable() {
        buildReserveTable()
        flipper.displayedChild = 12
    }

    private fun buildReserveTable() {
        val box = findViewById<LinearLayout>(R.id.reserve_container)
        box.removeAllViews()
        val list = buildReservations()
        saveReservations(list)
        if (list.isEmpty()) {
            box.addView(TextView(this).apply { text = "(予約はありません)"; setTextColor(Color.GRAY) })
            return
        }
        var curCam = ""
        for (r in list) {
            if (r.camLabel != curCam) {                  // カメラの帯(見出し)
                curCam = r.camLabel
                box.addView(TextView(this).apply {
                    text = curCam
                    setTypeface(null, Typeface.BOLD)
                    setTextColor(Color.WHITE)
                    setBackgroundColor(0xFF455A64.toInt())
                    setPadding(dp(8), dp(6), dp(8), dp(6))
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                    ).apply { setMargins(0, dp(10), 0, dp(2)) }
                })
            }
            val edge = if (r.edge.isEmpty()) "スマホ" else r.edge
            val line = "%s - %s  %s  %s".format(
                reserveFmt.format(java.util.Date(r.startMs)),
                reserveFmt.format(java.util.Date(r.endMs)),
                edge, r.planName)
            box.addView(TextView(this).apply {
                text = (if (r.conflict) "✕ " else "　") + line   // 重なりは行頭に ✕
                textSize = 13f
                setPadding(dp(8), dp(4), dp(8), dp(4))
                if (r.conflict) { setTextColor(0xFFC62828.toInt()); setBackgroundColor(0xFFFFEBEE.toInt()) }
                else            { setTextColor(Color.DKGRAY) }
            })
        }
    }

    // ============================================================
    //  660 操作履歴(項目9)
    // ============================================================
    // 撮影に関する操作を日時つきで残す。記録はスマホ(opHistory.json)。エッジ端末側で起きたことは、
    // 既存の進捗ポーリング(10秒・計画別C_PROGRESS)で受け取る「状態」の変化から拾うので、
    // 新しい通信は増やさない。スマホ直接撮影は EV_STATE の遷移から同じように拾う。
    //   send plan      … 撮影計画をエッジへ送った
    //   auto start     … 撮影が始まった(撮影窓に入った)
    //   user break     … ユーザー操作で中止した
    //   auto end       … 終了時刻になり自動的に停止した
    //   lost camera    … カメラがオフラインになった(撮影が止まった)
    //   connect camera … カメラがオンラインになった
    private val histFmt = SimpleDateFormat("yyyy.MM.dd HH:mm", Locale.JAPAN)
    private val histLastState = HashMap<String, Int>()   // planId → 直近の状態(遷移の判定用)
    private val histUserStop = HashSet<String>()          // ユーザー中止済み(後続のIDLEを auto end にしない)

    private fun histFile() = java.io.File(
        java.io.File(getExternalFilesDir(null), "asset").apply { mkdirs() }, "opHistory.json")

    private fun histLoad(): JSONArray =
        try { JSONArray(histFile().readText()) } catch (_: Exception) { JSONArray() }

    // 履歴を1件足す(最新が先頭)。計画名・端末名・カメラ名はその時点の計画から解決する。
    private fun addHistory(op: String, planId: String) {
        try {
            var plan = ""; var cam = ""
            val arr = JSONArray(HgeNative.nativeListPlans())
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                if (o.optString("id") != planId) continue
                plan = o.optString("planName")
                cam = o.optString("camName").ifEmpty { o.optString("camModel") }   // 項目B/C: アプリ登録の名称
                break
            }
            val edge = planEdgeName(planId).ifEmpty { "スマホ" }
            val rec = JSONObject().apply {
                put("t", System.currentTimeMillis())
                put("op", op); put("plan", plan); put("edge", edge); put("cam", cam)
            }
            val cur = histLoad()
            val out = JSONArray().put(rec)                       // 最新が先頭
            for (i in 0 until minOf(cur.length(), 499)) out.put(cur.get(i))   // 上限500件
            histFile().writeText(out.toString())
        } catch (_: Exception) {}
    }

    // 状態の変化から履歴を起こす。エッジのポーリング(reconcileEdgePlan)とスマホ直撮影(EV_STATE)の両方から呼ぶ。
    private fun histOnState(planId: String, st: Int) {
        val prev = histLastState[planId]
        if (prev == st) return
        histLastState[planId] = st
        if (prev == null) return    // 初回観測(アプリ起動直後など)は遷移として扱わない
        val wasActive = prev == HgeNative.ST_CAPTURING || prev == HgeNative.ST_WAITING ||
                        prev == HgeNative.ST_SEARCHING || prev == HgeNative.ST_NOCAMERA ||
                        prev == HgeNative.ST_DISCONNECTED || prev == HgeNative.ST_STOPPING
        val lostPrev = prev == HgeNative.ST_NOCAMERA || prev == HgeNative.ST_DISCONNECTED
        when {
            // カメラが復帰した(未検出 → 撮影/待機)
            lostPrev && (st == HgeNative.ST_CAPTURING || st == HgeNative.ST_WAITING) ->
                addHistory("connect camera", planId)
            // カメラを見失った(撮影/待機 → 未検出)
            !lostPrev && (st == HgeNative.ST_NOCAMERA || st == HgeNative.ST_DISCONNECTED) ->
                addHistory("lost camera", planId)
        }
        // 撮影が始まった
        if (prev != HgeNative.ST_CAPTURING && st == HgeNative.ST_CAPTURING) addHistory("auto start", planId)
        // 終了(実行中 → IDLE)。ユーザー中止は beginStop 側で記録済みなので二重に残さない。
        if (wasActive && st == HgeNative.ST_IDLE) {
            if (histUserStop.remove(planId)) { /* user break は記録済み */ }
            else addHistory("auto end", planId)
        }
    }

    private fun openHistory() { buildHistory(); flipper.displayedChild = 13 }

    private val histDateFmt = SimpleDateFormat("yyyy.MM.dd (E)", Locale.JAPAN)
    private val histTimeFmt = SimpleDateFormat("HH:mm", Locale.JAPAN)

    private fun buildHistory() {
        val box = findViewById<LinearLayout>(R.id.history_container)
        box.removeAllViews()
        val arr = histLoad()
        if (arr.length() == 0) {
            box.addView(TextView(this).apply { text = "(履歴はありません)"; setTextColor(Color.GRAY) })
            return
        }
        // 項目B: 日付ごとのブロックにする。日付見出しを1回だけ出し、同じ日付の行は時刻から表示する
        //  (全行に日付を入れると横が入りきらないため)。行=時刻 / 操作内容 / 計画 / 端末 / カメラ(登録名称)。
        var curDay = ""
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val d = java.util.Date(o.optLong("t"))
            val day = histDateFmt.format(d)
            if (day != curDay) {                       // 日付の見出し
                curDay = day
                box.addView(TextView(this).apply {
                    text = day
                    setTypeface(null, Typeface.BOLD)
                    setTextColor(Color.WHITE)
                    setBackgroundColor(0xFF455A64.toInt())
                    setPadding(dp(8), dp(6), dp(8), dp(6))
                    layoutParams = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                    ).apply { setMargins(0, dp(10), 0, dp(2)) }
                })
            }
            val line = "%s  %-13s %s  %s  %s".format(
                histTimeFmt.format(d),                 // 日付ブロック内は時刻のみ
                o.optString("op"), o.optString("plan"), o.optString("edge"), o.optString("cam"))
            box.addView(TextView(this).apply {
                text = line
                textSize = 12f
                typeface = Typeface.MONOSPACE          // 桁を揃えて読みやすくする
                setTextColor(Color.DKGRAY)
                setPadding(dp(8), dp(5), dp(8), dp(5))
            })
        }
    }

    // ============================================================
    //  670 撮影レポート(撮影1回=1件)
    // ============================================================
    // Entity が撮影終了時に /log へ書いた report_*.json を読んで見せるだけの画面。集計は
    // すべて Entity 側で済んでいる。所見は JSON にコード(dataManager::noteCode)で入っており、
    // 表示する文言はこの画面が持つ(文言を変えても保存ファイルの形式に影響しない)。
    private var selectedReport: String? = null

    private fun openReportList() {
        buildReportList(); buildReportDetail()
        setInitialSplit(R.id.report_container)
        flipper.displayedChild = 14
    }

    // 所見コード → 表示文言(dataManager::noteCode と対応)。
    private fun reportNoteText(code: Int): String = when (code) {
        1 -> "露出設定に失敗したコマがあります。カメラの露出がアプリの意図とズレている可能性があります。"
        2 -> "ライブビューの更新が撮影周期に追いついていません。撮影周期を長くすると安定します。"
        3 -> "撮影周期を守れないコマが多いです。シャッター速度に対して周期が短い可能性があります。"
        4 -> "測光できないコマが多いです。露出が据え置かれるため明るさが追従しません。"
        5 -> "(欠番)"
        6 -> "撮影周期に余裕がありません。目安の最短周期を下回ると、遅れやコマ落ちが出ます。"
        7 -> "撮影周期にはまだ余裕があります。周期を短くすると滑らかな微速度になります。"
        8 -> "カメラの記録が明けるまでの時間を1コマも測れませんでした。測光したコマがありません。"
        9 -> "撮影前の露出合わせで一度も測光できず、基準値のまま撮り始めました。最初の数コマは露出が大きく外れています。"
        10 -> "撮影前の露出合わせが終わりきらないまま撮り始めました。最初の数コマは露出が合っていない可能性があります。"
        else -> "(不明な所見 $code)"
    }

    private fun buildReportList() {
        val box = findViewById<LinearLayout>(R.id.report_container)
        box.removeAllViews()
        val arr = try { JSONArray(HgeNative.nativeReportList()) } catch (_: Exception) { JSONArray() }
        if (arr.length() == 0) {
            box.addView(TextView(this).apply { text = "(撮影レポートはありません)"; setTextColor(Color.GRAY) })
            selectedReport = null
            return
        }
        // 一覧は新しい順。選択が無い/消えたときは一番新しいものを開く。
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }.filter { it.isNotEmpty() }
        if (selectedReport == null || selectedReport !in names) { selectedReport = names.firstOrNull() }
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val name = o.optString("name")
            val notes = o.optInt("noteCount")
            val edge = o.optString("edge")
            val sub = "%s / %s  %d枚%s%s".format(
                o.optString("plan"), o.optString("camera"), o.optInt("frames"),
                if (edge.isNotEmpty()) "   $edge" else "",
                if (notes > 0) "   所見 ${notes}件" else "")
            val title = o.optString("shotAt").ifEmpty { name }
            // 一覧は新しい順なので「これより古い」= この行より下に並んでいるものすべて。
            // 読めなかったレポートは shotAt を持たず末尾へ落ちているので、ここにも入る(消せるようにしておく)。
            val idx = names.indexOf(name)
            val older = if (idx < 0) emptyList() else names.drop(idx + 1)
            val menu: MutableList<Pair<String, () -> Unit>> = mutableListOf()
            menu.add("削除" to { confirmDeleteReport(name) })
            if (older.isNotEmpty()) {
                menu.add("これより古いレポート削除" to {
                    confirmDeleteReports("これより古いレポートの削除",
                        "「$title」より古いレポート ${older.size}件を削除しますか？", older)
                })
            }
            menu.add("全レポート削除" to {
                confirmDeleteReports("全レポートの削除",
                    "撮影レポート ${names.size}件をすべて削除しますか？", names)
            })
            box.addView(listRow(title, sub, name == selectedReport,
                { selectedReport = name; buildReportList(); buildReportDetail() }, menu))
            box.addView(thinDivider())
        }
    }

    // エッジに溜まった撮影レポートを引き取る(edgeSweep のワーカースレッドから呼ぶ)。
    // 取得 → レポートとして読めることを確認 → スマホへ保存 → そこで初めてエッジへ削除を指示する。
    // 取得しただけで消すと、保存に失敗したぶんが永久に失われる。削除まで届かなかったものは
    // 次のスイープでまた拾う(同名で上書きするだけなので重複しない)。
    private fun collectEdgeReports(edge: Edge) {
        val arr = try { JSONArray(HgeNative.nativeEdgeReportList(edge.addr(), edge.port)) } catch (_: Exception) { JSONArray() }
        val dir = java.io.File(getExternalFilesDir(null), "log")
        if (!dir.exists() && !dir.mkdirs()) return
        var got = 0
        for (i in 0 until arr.length()) {
            val name = arr.optJSONObject(i)?.optString("name").orEmpty()
            if (!name.startsWith("report_") || !name.endsWith(".json")) continue
            val body = try { HgeNative.nativeEdgeReportRead(edge.addr(), edge.port, name) } catch (_: Exception) { "" }
            if (body.isEmpty()) continue
            // 途中で切れたものを保存して消させないため、中身を検査してから書く。
            val o = try { JSONObject(body) } catch (_: Exception) { null } ?: continue
            if (!o.has("capture")) continue
            o.put("edge", edge.name)   // どの端末で撮ったかを一覧と内容に出せるようにする
            try { java.io.File(dir, name).writeText(o.toString()) } catch (_: Exception) { continue }
            try { HgeNative.nativeEdgeReportDelete(edge.addr(), edge.port, name) } catch (_: Exception) {}
            got++
        }
        if (got > 0) runOnUiThread {
            Toast.makeText(this, "外部端末「${edge.name}」の撮影レポート ${got}件を取得しました", Toast.LENGTH_SHORT).show()
            if (flipper.displayedChild == 15) { buildReportList(); buildReportDetail() }   // 開いていれば即反映
        }
    }

    private fun confirmDeleteReport(name: String) {
        AlertDialog.Builder(this)
            .setTitle("撮影レポートの削除")
            .setMessage("このレポートを削除しますか？\n$name")
            .setPositiveButton("削除する") { _, _ ->
                HgeNative.nativeRemoveReport(name)
                if (selectedReport == name) { selectedReport = null }
                buildReportList(); buildReportDetail()
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    // まとめ削除(「これより古い」/「全部」)。渡す名前は一覧と同じ新しい順。
    // スマホに保存されているレポートだけを消す(エッジに残っているものは次のスイープでまた拾う)。
    private fun confirmDeleteReports(title: String, message: String, names: List<String>) {
        if (names.isEmpty()) { return }
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage("$message" + "\n" + "削除したレポートは戻せません。")
            .setPositiveButton("削除する") { _, _ ->
                var ok = 0
                for (n in names) { if (HgeNative.nativeRemoveReport(n) == 0) { ok++ } }
                if (selectedReport in names) { selectedReport = null }
                buildReportList(); buildReportDetail()
                val ng = names.size - ok
                Toast.makeText(this,
                    if (ng == 0) "撮影レポート ${ok}件を削除しました"
                    else "撮影レポート ${ok}件を削除しました(${ng}件は削除できませんでした)",
                    Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    private fun buildReportDetail() {
        val box = findViewById<LinearLayout>(R.id.report_detail)
        box.removeAllViews()
        val name = selectedReport
        if (name.isNullOrEmpty()) { return }
        val o = try { JSONObject(HgeNative.nativeReportJson(name)) } catch (_: Exception) { null }
        if (o == null) {
            box.addView(TextView(this).apply { text = "(このレポートは読めませんでした)"; setTextColor(Color.RED) })
            return
        }
        val win = o.optJSONObject("window") ?: JSONObject()
        val cap = o.optJSONObject("capture") ?: JSONObject()
        val exp = o.optJSONObject("exposure") ?: JSONObject()
        val itv = o.optJSONObject("interval") ?: JSONObject()
        val tim = o.optJSONObject("timing") ?: JSONObject()
        val bsy = o.optJSONObject("busy") ?: JSONObject()
        val met = o.optJSONObject("meter") ?: JSONObject()
        val apl = o.optJSONObject("apply") ?: JSONObject()
        val lvw = o.optJSONObject("liveview") ?: JSONObject()
        val lim = o.optJSONObject("limit") ?: JSONObject()

        repHead(box, o.optString("plan"))
        repRow(box, "カメラ", o.optString("camera"))
        repRow(box, "レンズ", o.optString("lens"))
        // エッジから回収したものだけ端末名が入る(空=スマホ直結で撮った)。
        o.optString("edge").takeIf { it.isNotEmpty() }?.let { repRow(box, "撮影した端末", it) }
        repRow(box, "撮影窓", win.optString("start") + " 〜 " + win.optString("end"))
        repRow(box, "出力日時", o.optString("shotAt"))

        repBand(box, "撮影")
        repRow(box, "コマ数", "${cap.optInt("frames")}")
        repRow(box, "シャッター失敗", "%d (%.1f%%)".format(cap.optInt("shootFail"), cap.optDouble("shootFailPct")))

        repBand(box, "露出")
        repRow(box, "測光したコマ", "${exp.optInt("meterTried")}", "夜間の固定露出は測光しないので含まない")
        repRow(box, "測光できなかった", "%d (%.1f%%)".format(exp.optInt("meterFail"), exp.optDouble("meterFailPct")),
               "測光したコマ中。露出は据え置き")
        repRow(box, "露出設定できず", "%d (%.1f%%)".format(exp.optInt("setFail"), exp.optDouble("setFailPct")),
               "0%でないとアプリとカメラの露出がズレる")
        // 何で測ったかの内訳。ライブビュー主体のカメラ(R10)では、サムネイルの回数が
        //  そのまま機種の取得回数予算の消費になるので、ここを見て間引きを調整する。
        val nLv = exp.optInt("lvFrames"); val nTh = exp.optInt("thumbFrames"); val nHd = exp.optInt("heldFrames")
        if (nLv + nTh + nHd > 0) {
            repRow(box, "ライブビューで測光", "$nLv コマ")
            repRow(box, "サムネイルで測光", "$nTh コマ",
                   if (nLv > 0) "ライブビューで足りず落ちたコマ" else "サムネイルだけの方式では全コマ")
            if (nHd > 0) repRow(box, "測らず据え置き", "$nHd コマ", "間引き。直近の測光値を使った")
        }
        repRow(box, "測光リトライ", "${exp.optInt("meterRetryFrames")} コマ")
        repRow(box, "露出設定リトライ", "${exp.optInt("applyRetryFrames")} コマ")

        repBand(box, "撮影周期")
        repRow(box, "設定 / 実測", "%.1f / %.2f 秒".format(itv.optDouble("setSec"), itv.optDouble("actualSec")))
        repRow(box, "周期どおり", "%d / %d (%.1f%%)".format(tim.optInt("lateOk"), tim.optInt("lateCnt"), tim.optDouble("lateOkPct")),
               "遅れ100ms以内")
        repRow(box, "遅れ 平均/最大", "%.0f / %d ms".format(tim.optDouble("lateAvgMs"), tim.optInt("lateMaxMs")))
        // 準備が周期に間に合わなかったコマは、終わり次第すぐシャッターを切る(=そのぶん遅れる)。
        // 次のコマはその遅れた時刻を起点に正確な周期へ戻るので、遅れは積み上がらない。
        repRow(box, "間に合わず遅れた", "%d コマ / 合計 %.1f 秒".format(
                   tim.optInt("lateFrames"), tim.optInt("lateOverSumMs") / 1000.0),
               "1コマあたり平均 %.0f ms。遅れた分は次コマへ持ち越さない".format(tim.optDouble("lateOverAvgMs")))
        repRow(box, "準備 平均/最大", "%.0f / %d ms".format(tim.optDouble("prepAvgMs"), tim.optInt("prepMaxMs")),
               "測光→露出計算→露出設定")
        repRow(box, "準備が間に合わず", "${tim.optInt("prepOver")} コマ",
               "リード %.1f 秒以内に終える必要がある".format(tim.optInt("leadMs") / 1000.0))

        // ここが「撮影周期をどこまでシャッター速度へ近づけられるか」の答えになる部分。
        repBand(box, "カメラの busy と周期の余裕")
        val busyCnt = bsy.optInt("cnt")
        if (busyCnt > 0) {
            repRow(box, "露光終了→撮影画像が取れる", "平均 %.2f / 最大 %.2f 秒".format(bsy.optDouble("avgMs") / 1000.0, bsy.optInt("maxMs") / 1000.0),
                   "${busyCnt}コマで実測。カメラが記録で塞がっている時間")
        } else {
            repRow(box, "露光終了→撮影画像が取れる", "計測なし", "測光したコマが無かった")
        }
        repRow(box, "測光の所要", "平均 %.2f / 最大 %.2f 秒".format(met.optDouble("avgMs") / 1000.0, met.optInt("maxMs") / 1000.0))
        repRow(box, "露出設定の所要", "平均 %.2f / 最大 %.2f 秒".format(apl.optDouble("avgMs") / 1000.0, apl.optInt("maxMs") / 1000.0))
        repRow(box, "この撮影の最長ss", "%.2f 秒".format(lim.optDouble("maxSsSec")))
        val minItv = lim.optDouble("minIntervalSec", -1.0)
        if (minItv >= 0.0) {
            repRow(box, "目安の最短周期", "%.1f 秒".format(minItv),
                   "最長ss + 準備最大 + 余裕1秒。設定周期との差 %.1f 秒".format(lim.optDouble("marginSec")))
        }

        // 撮影開始前の露出合わせ(初期収束)がうまくいったか。ここが済んでいないと1枚目から露出が外れる。
        repBand(box, "撮影前の露出合わせ")
        val cvw = o.optJSONObject("converge") ?: JSONObject()
        // 3=収束不要(固定露出で始まった/直前の撮影露出を引き継いだ)。古いレポートには outcome が
        //  無いか 2 が入っているので、既定は 3 にしつつ 2 の表示は残す。
        val cvOutcome = cvw.optInt("outcome", 3)
        repRow(box, "結果", when (cvOutcome) {
            0 -> "合わせられた"
            1 -> "合わせきれず開始"
            2 -> "測光できず開始"
            else -> "収束不要"
        }, when (cvOutcome) {
            0 -> "目標の範囲に入った(または露出限界に到達した)"
            1 -> "時間内に目標へ届かず、その時点の最良推定で撮り始めた"
            2 -> "一度も測光できず、撮影制御方法の基準値のまま撮り始めた"
            else -> "固定露出(星景/夜間)から始まったか、直前の撮影露出を引き継いだので、合わせる必要がなかった"
        })
        if (cvOutcome != 3) { repRow(box, "測れた回数", "${cvw.optInt("steps")} 回") }
        // ライブビューでは測れない暗さだったときだけ、実写を撮って合わせ直す。
        //  そのコマは frames に入らないので、カードの枚数との差をここで説明する。
        val cvShots = cvw.optInt("shots")
        if (cvShots > 0) {
            repRow(box, "調整用の撮影", "$cvShots 枚",
                   "暗くてライブビューでは測れないため、実際に撮って露出を合わせた。" +
                   "この枚数はコマ数に含まれないが、カードには残っている")
        }
        if (cvw.optInt("applyNg") > 0 || cvw.optInt("meterNg") > 0) {
            repRow(box, "やり直した回数", "露出設定 ${cvw.optInt("applyNg")} / 測光 ${cvw.optInt("meterNg")}",
                   "失敗した回の値は使わずやり直している")
        }

        repBand(box, "ライブビュー")
        repRow(box, "古い映像を破棄", "%d コマ (延べ %d 回)".format(lvw.optInt("staleFrames"), lvw.optInt("staleTotal")))

        val notes = o.optJSONArray("notes")
        if (notes != null && notes.length() > 0) {
            repBand(box, "所見")
            for (i in 0 until notes.length()) {
                box.addView(TextView(this).apply {
                    text = "・" + reportNoteText(notes.optInt(i))
                    textSize = 14f
                    setTextColor(0xFFB71C1C.toInt())
                    setPadding(dp(4), dp(6), dp(4), dp(6))
                })
            }
        }
    }

    private fun repHead(box: LinearLayout, title: String) {
        box.addView(TextView(this).apply {
            text = title.ifEmpty { "(名称なし)" }
            textSize = 19f; setTypeface(null, Typeface.BOLD); setTextColor(Color.BLACK)
            setPadding(0, 0, 0, dp(6))
        })
    }

    private fun repBand(box: LinearLayout, title: String) {
        box.addView(TextView(this).apply {
            text = title
            setTypeface(null, Typeface.BOLD); setTextColor(Color.WHITE)
            setBackgroundColor(0xFF455A64.toInt())
            setPadding(dp(8), dp(6), dp(8), dp(6))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, dp(12), 0, dp(4)) }
        })
    }

    // 1行 = 見出し / 値。数字の読み方(hint)は小さく灰色で値の下へ添える。
    private fun repRow(box: LinearLayout, label: String, value: String, hint: String = "") {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.setPadding(dp(4), dp(4), dp(4), dp(4))
        row.addView(TextView(this).apply {
            text = label; textSize = 14f; setTextColor(Color.DKGRAY)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        row.addView(TextView(this).apply {
            text = value; textSize = 14f; setTextColor(Color.BLACK)
            setTypeface(Typeface.MONOSPACE, Typeface.BOLD)
            gravity = Gravity.END
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.3f)
        })
        box.addView(row)
        if (hint.isNotEmpty()) {
            box.addView(TextView(this).apply {
                text = hint; textSize = 11f; setTextColor(Color.parseColor("#888888"))
                setPadding(dp(4), 0, dp(4), dp(6))
            })
        }
    }

    // 項目17: この計画を開始してよいか予約表で確かめる。同じカメラを「別の端末」が重なる時間で
    // 使う予約があれば、その端末名を返す(=開始不可)。問題なければ null。
    private fun reserveBlockedBy(planId: String): String? {
        val list = buildReservations()
        val me = list.firstOrNull { it.planId == planId } ?: return null
        for (r in list) {
            if (r.planId == me.planId) continue
            if (!sameCamera(r, me)) continue
            // 端末が同じでも弾く。以前は「同一端末なら Entity 側が二重撮影を防ぐ」として素通りさせていたが、
            // それでは同じカメラの重複計画を2つとも武装でき、要求が受け付けられたまま放置され得る(項目3再修正)。
            // 同じカメラは1計画しか使えないので、端末に関係なく要求時点で弾く。
            if (me.startMs < r.endMs && r.startMs < me.endMs) {   // 時間が重なる
                // 項目3: そのカメラを実際に使用中(=開始済み: 開始要求中/待機/撮影/未検出)の計画だけがブロックする。
                //  まだどちらも開始していない重複同士なら、先に開始する方を許可する(両方が開始不可にならない)。
                //  ※「エッジが保有しているだけ」(中止後の常駐)はカメラを使っていないのでブロックしない。
                if (isPlanUsingCamera(r.planId)) return if (r.edge.isEmpty()) "スマホ" else r.edge
            }
        }
        return null
    }

    private fun buildPlacesList(): Unit = renderList(ListPane(
        containerId = R.id.places_container,
        rows = {
            val arr = placeArray(HgeNative.nativeGetPlaces())
            (0 until arr.length()).mapNotNull { arr.optJSONObject(it) }.map { o ->
                val name = o.optString("name")
                ListItem(name, name,
                    "%.4f, %.4f  標高 %dm".format(o.optDouble("latitude", 0.0), o.optDouble("longitude", 0.0),
                                                  o.optDouble("altitude", 0.0).toInt()) +
                        (if (o.optString("memo").isNotEmpty()) "  ${o.optString("memo")}" else ""),
                    listOf("削除" to {
                        dataExec.execute { HgeNative.nativeRemovePlace(name)
                            runOnUiThread { if (selPlace == name) selPlace = null; buildPlacesList(); buildPlaceDetail() } }
                    }))
            }
        },
        selected = { selPlace }, setSelected = { selPlace = it },
        onSelect = { selectPlace(it) },
        onRename = { orig, nm -> commitPlaceRename(orig, nm) },
        addLabel = "＋ 新しい場所の追加", onAdd = { addPlace() }))

    private fun addPlace() {
        dataExec.execute {
            HgeNative.nativeAddPlace("")
            val names = placeNames()
            val added = names.lastOrNull()
            runOnUiThread {
                selPlace = added; buildPlacesList(); buildPlaceDetail()
                // 【新しい場所も現在地から作る(2026-09-04 UI依頼)】撮影地はたいてい手元なので、
                //  座標と標高を入れた状態から始める。取れなければ Entity が入れた出荷時の場所
                //  (Tokyo)のまま。取得は非同期なので、先に一覧と詳細を出してから追いかける。
                if (added != null) { fillPlaceFromCurrentLocation(added) }
            }
        }
    }

    private fun selectPlace(name: String) {
        if (name == selPlace) return
        persistPlaceDetail(false)
        selPlace = name; buildPlacesList(); buildPlaceDetail()
    }

    private fun commitPlaceRename(orig: String, newName: String) {
        val nm = newName.trim()
        if (nm.isEmpty() || nm == orig) return
        if (placeNames().any { it == nm }) { showNameInUse(nm); buildPlacesList(); return }
        selPlace = nm
        persistPlaceDetail(rebuild = true, origName = orig, newName = nm)
    }

    private fun buildPlaceDetail() {
        val box = findViewById<LinearLayout>(R.id.places_detail)
        box.removeAllViews(); placeCoordTv = null; placeAltEt = null; placeMemoEt = null; placeAutoCb = null
        val sel = selPlace
        if (sel == null) {
            box.addView(TextView(this).apply { text = "「＋ 新しい場所の追加」で場所を登録してください"; setPadding(dp(4), dp(16), dp(4), dp(16)) })
            return
        }
        val arr = placeArray(HgeNative.nativeGetPlaces())
        var o: JSONObject? = null
        for (i in 0 until arr.length()) { val x = arr.optJSONObject(i) ?: continue; if (x.optString("name") == sel) { o = x; break } }
        if (o == null) { box.addView(TextView(this).apply { text = "(データなし)" }); return }
        // 項目2: 撮影場所にも「変更の取り消し」ボタンを新設(dirty 連動。取消=保存内容から作り直し)。
        val placeCancel = addCancelButton(box, atTop = true) { buildPlaceDetail() }
        placeLat = o.optDouble("latitude", 0.0); placeLng = o.optDouble("longitude", 0.0)
        // 緯度・経度(DMS表示) + 取得手段(地図/貼り付け/現在地)
        box.addView(TextView(this).apply { text = "緯度・経度"; textSize = 13f; setTextColor(Color.GRAY); setPadding(0, dp(4), 0, dp(2)) })
        val btnRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        btnRow.addView(linkText("📍 地図から取得") {
            // 名前にカーソルが残ったままでも、ここで改名を確定させてから開く。
            //  確定しないまま座標を書くと、後から来る改名との順序で宛先が食い違う。
            commitListNameEdit(R.id.places_container)
            val la0 = if (placeLat != 0.0 || placeLng != 0.0) placeLat else 35.681
            val lo0 = if (placeLat != 0.0 || placeLng != 0.0) placeLng else 139.767
            openMapPicker(la0, lo0) { la, lo -> onPlaceCoord(la, lo) }
        })
        btnRow.addView(linkText("✎ 貼り付け") { commitListNameEdit(R.id.places_container); showPlacePasteDialog("%.6f, %.6f".format(placeLat, placeLng)) { la, lo -> onPlaceCoord(la, lo) } })
        btnRow.addView(linkText("＋ 現在地") { commitListNameEdit(R.id.places_container); fetchCurrentLocation { la, lo, alt -> if (alt != 0.0) placeAltEt?.setText(alt.toInt().toString()); onPlaceCoord(la, lo) } })
        box.addView(btnRow)
        val coordTv = TextView(this).apply { textSize = 18f; setTextColor(Color.BLACK); setPadding(0, dp(2), 0, dp(8)) }
        placeCoordTv = coordTv; box.addView(coordTv); refreshPlaceCoordText()
        // 標高(緯度経度から自動取得。手動再取得も可)
        val altHdr = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        altHdr.addView(TextView(this).apply { text = "標高 [m]"; textSize = 13f; setTextColor(Color.GRAY) })
        altHdr.addView(linkText("　🗻 緯度経度から取得") {
            if (placeLat == 0.0 && placeLng == 0.0) Toast.makeText(this, "先に緯度・経度を設定してください", Toast.LENGTH_SHORT).show()
            else fetchElevationInto(placeLat, placeLng)
        })
        box.addView(altHdr)
        val altEt = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
            setText(o.optDouble("altitude", 0.0).toInt().toString())
        }
        placeAltEt = altEt; box.addView(altEt)
        // タイムゾーン(2026-09-03)。
        //  【なぜ場所が持つか】計画の時刻(開始/終了・撮影制御方法の切替)は「その場所の現地時刻」で、
        //   スマホやエッジがどこにあるかとは関係ない。端末のTZで解釈していたため、日本で作った
        //   計画を現地へ持って行くと切替時刻が時差ぶんずれた。場所が持てば、どの端末で走らせても
        //   同じ瞬間になる。**既定は端末の値**なので、国内で使う限り気にしなくてよい。
        placeTzOffMin = o.optInt("tzOffMin", nowOffMin())
        box.addView(TextView(this).apply { text = "タイムゾーン"; textSize = 13f; setTextColor(Color.GRAY); setPadding(0, dp(8), 0, dp(2)) })
        val tzRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        val tzTv = TextView(this).apply { textSize = 18f; setTextColor(Color.BLACK); text = tzLabel(placeTzOffMin) }
        tzRow.addView(tzTv)
        tzRow.addView(linkText("　✎ 変更") { showPlaceTzDialog { off -> placeTzOffMin = off; tzTv.text = tzLabel(off); persistPlaceDetail(false, rebuildList = true) } })
        tzRow.addView(linkText("　＋ このスマホに合わせる") {
            placeTzOffMin = nowOffMin(); tzTv.text = tzLabel(placeTzOffMin); persistPlaceDetail(false, rebuildList = true)
        })
        box.addView(tzRow)
        // メモ(説明)
        box.addView(TextView(this).apply { text = "メモ"; textSize = 13f; setTextColor(Color.GRAY); setPadding(0, dp(8), 0, dp(2)) })
        val memoEt = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
            minLines = 2; gravity = Gravity.TOP or Gravity.START; setText(o.optString("memo"))
        }
        placeMemoEt = memoEt; box.addView(memoEt)
        // 自動挿入(項目10: 全体で1つだけ。ONにすると他の場所の指定は自動的に外れる)
        val cb = CheckBox(this).apply { text = "撮影計画に自動的に挿入する（1つの場所だけ）"; isChecked = o.optBoolean("autoInsert", false) }
        placeAutoCb = cb; box.addView(cb)
        cb.setOnCheckedChangeListener { _, _ ->
            // 即保存してリストを作り直す(他の場所のチェックが外れたことを表示へ反映するため)。
            persistPlaceDetail(false, rebuildList = true)
        }
        // 項目2: 標高/メモの未保存編集を dirty 判定(座標/自動挿入は即保存で作り直されるため基準が更新される)。
        startDirtyWatch(placeCancel) { placeDetailSig() }
    }

    private fun placeDetailSig(): String =
        "$placeLat,$placeLng,alt=${placeAltEt?.text},memo=${placeMemoEt?.text},auto=${placeAutoCb?.isChecked == true}"

    private fun onPlaceCoord(lat: Double, lng: Double) {
        placeLat = lat; placeLng = lng; refreshPlaceCoordText()
        persistPlaceDetail(false, rebuildList = true)   // 座標変更を即保存し一覧の座標表示も更新
        fetchElevationInto(lat, lng)                    // 標高を緯度経度から自動取得(§7.9。Open-Meteo 全世界・無料)
    }

    // Open-Meteo Elevation API(APIキー不要・全世界・無料。Copernicus DEM GLO-90)で標高を取得し
    // 標高欄へ反映して保存する。ネット不通などは黙って無視(手入力で対応可能)。
    private fun fetchElevationInto(lat: Double, lng: Double) {
        val target = selPlace
        Thread {
            try {
                val u = String.format(Locale.US, "https://api.open-meteo.com/v1/elevation?latitude=%.6f&longitude=%.6f", lat, lng)
                val conn = (java.net.URL(u).openConnection() as java.net.HttpURLConnection).apply {
                    connectTimeout = 6000; readTimeout = 6000; requestMethod = "GET"
                }
                val body = conn.inputStream.bufferedReader().use { it.readText() }
                conn.disconnect()
                val arr = JSONObject(body).optJSONArray("elevation")
                val elev = arr?.optDouble(0, Double.NaN) ?: Double.NaN
                if (!elev.isNaN()) runOnUiThread {
                    if (selPlace == target) {   // 取得中に別の場所へ切替えていたら反映しない
                        placeAltEt?.setText(elev.toInt().toString())
                        persistPlaceDetail(false, rebuildList = true)
                        Toast.makeText(this, "標高を取得: ${elev.toInt()}m", Toast.LENGTH_SHORT).show()
                    }
                }
            } catch (e: Exception) {
                // 失敗を黙って捨てると「地図から取得しても標高が入らない」ように見えるため知らせる(項目10)。
                // 代表例: エッジのAPへバインド中はインターネットへ出られない(§1.2.1)。手入力でも設定できる。
                runOnUiThread {
                    if (selPlace == target) {
                        Toast.makeText(this, "標高を取得できませんでした（ネット未接続？手入力できます）", Toast.LENGTH_LONG).show()
                    }
                }
            }
        }.start()
    }
    private fun refreshPlaceCoordText() {
        placeCoordTv?.text = if (placeLat == 0.0 && placeLng == 0.0) "未設定（ボタンで取得）"
            else "${toDms(placeLat, 'N', 'S')}  ${toDms(placeLng, 'E', 'W')}\n%.5f, %.5f".format(placeLat, placeLng)
    }
    // タイムゾーンの表示。"+09:00" の形にする(分まで持つ地域があるため時だけにはしない)。
    private fun tzLabel(offMin: Int): String {
        val sign = if (offMin < 0) "-" else "+"
        val a = Math.abs(offMin)
        return "%s%02d:%02d".format(sign, a / 60, a % 60)
    }

    // タイムゾーンを手で入れる。よく使う候補を出しつつ、任意の値も入れられるようにする。
    //  緯度経度からタイムゾーンを自動で決めることはできない(オフラインで引く手段が無く、
    //  経度からの近似は国境付近で外れる)。手で選ぶのが確実。
    private fun showPlaceTzDialog(onPick: (Int) -> Unit) {
        val cands = listOf(
            "このスマホ (" + tzLabel(nowOffMin()) + ")" to nowOffMin(),
            "日本 +09:00" to 540, "モンゴル +08:00" to 480, "中国 +08:00" to 480,
            "台湾 +08:00" to 480, "韓国 +09:00" to 540, "UTC +00:00" to 0)
        val labels = cands.map { it.first } + listOf("その他(手入力)")
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("タイムゾーン")
            .setItems(labels.toTypedArray()) { _, which ->
                if (which < cands.size) { onPick(cands[which].second); return@setItems }
                val et = EditText(this)
                et.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED or InputType.TYPE_NUMBER_FLAG_DECIMAL
                et.hint = "UTCからの時差(時)。例 8 / 9.5 / -5"
                androidx.appcompat.app.AlertDialog.Builder(this)
                    .setTitle("UTCからの時差(時)")
                    .setView(et)
                    .setPositiveButton("OK") { _, _ ->
                        val h = et.text.toString().trim().toDoubleOrNull()
                        if (h == null || h < -12.0 || h > 14.0) {
                            Toast.makeText(this, "-12〜+14 の範囲で入れてください", Toast.LENGTH_SHORT).show()
                        } else onPick(Math.round(h * 60.0).toInt())
                    }
                    .setNegativeButton("キャンセル", null).show()
            }.show()
    }

    private fun toDms(v: Double, pos: Char, neg: Char): String {
        val hemi = if (v >= 0) pos else neg
        val a = Math.abs(v); val d = a.toInt(); val mf = (a - d) * 60.0; val m = mf.toInt(); val s = (mf - m) * 60.0
        return "%d°%02d'%04.1f\"".format(d, m, s) + hemi
    }

    // 詳細の 標高/メモ/自動挿入/座標 を JSON にして Entity へ保存する。origName=改名前キー。
    private fun persistPlaceDetail(rebuild: Boolean, origName: String? = null, newName: String? = null, rebuildList: Boolean = false) {
        val key = origName ?: selPlace ?: return
        val name = newName ?: selPlace ?: key
        val alt = placeAltEt?.text?.toString()?.trim()?.toDoubleOrNull() ?: 0.0
        val memo = placeMemoEt?.text?.toString() ?: ""
        val auto = placeAutoCb?.isChecked ?: false
        val json = JSONObject().apply {
            put("name", name); put("memo", memo)
            put("latitude", placeLat); put("longitude", placeLng)
            put("altitude", alt); put("autoInsert", auto); put("tzOffMin", placeTzOffMin)
        }.toString()
        dataExec.execute {
            HgeNative.nativeSetPlaceDetail(key, json)
            if (rebuild) runOnUiThread { buildPlacesList(); buildPlaceDetail() }
            else if (rebuildList) runOnUiThread { buildPlacesList() }   // 座標だけ更新(詳細の入力欄は保持)
        }
    }

    // 現在地(GPS/ネットワーク)を取得して onGot(lat,lng,alt) を呼ぶ(§7.9)。権限が無ければ要求。
    // 【初回起動の1件を現在地にする(2026-09-04 UI依頼)】Entity は撮影場所ファイルが無いとき
    //  出荷時の場所(Tokyo)を1件だけ作る。撮影地はたいてい手元なので、取れるなら現在地に差し替える。
    //   ・タイムゾーンは端末から取る。**権限が要らず必ず取れる**(位置情報とは違う)
    //   ・座標が取れないとき(権限なし/測位失敗)は Tokyo のまま。無理に空にはしない
    //   ・標高は標高API→取れなければ測位値。座標さえ取れれば現在地を使う
    //  一度試したら二度としない(prefs)。ユーザーが消した場所を毎回作り直さないため。
    //  出荷時のまま(1件・名前が Tokyo)のときだけ触る。使い始めた後のデータは書き換えない。
    private fun seedFirstPlaceFromLocation() {
        val pf = hgcPrefs()
        if (pf.getBoolean("placeSeedTried", false)) { return }
        val arr = placeArray(HgeNative.nativeGetPlaces())
        if (arr.length() != 1 || arr.optJSONObject(0)?.optString("name") != "Tokyo") {
            pf.edit().putBoolean("placeSeedTried", true).apply()   // 既に使われている → 触らない
            return
        }
        pf.edit().putBoolean("placeSeedTried", true).apply()
        //  この1件を「撮影計画に自動的に挿入する」にする(2026-09-05 UI依頼)。
        //  初めて使う人にとって場所はこの1件だけなので、新しい計画もここから始まるのが自然。
        //  位置が取れず Tokyo のまま残ったときも同じで、**先に指定しておく**。
        //  改名は setPlaceDetailJson が指定ごと付け替えるので、current location へもついていく。
        dataExec.execute { HgeNative.nativeSetPlaceAutoInsert("Tokyo", 1) }
        //  出荷時の固定計画も同じ場所にする(2026-09-05 UI依頼)。そうしないと計画は Tokyo、
        //  撮影場所リストは current location という食い違った状態で使い始めることになる。
        fillPlaceFromCurrentLocation("Tokyo", kCurrentPlaceName, alsoSetPlan = true)
    }

    // 撮影場所1件を現在地の座標・標高で埋める。**作った直後の場所にだけ使う**。
    //  ・座標が取れない(権限なし/測位失敗)ときは何もしない。呼び出し元が入れた既定値、
    //    つまり出荷時の場所(Tokyo)がそのまま残る
    //  ・標高は**標高APIを優先**する。測位が返す高さは楕円体高で、海面からの標高とは 40m ほど
    //    違う(実測: API 58m に対し測位 108m)。APIが駄目なら測位値を使う
    //  ・名前を変えたいときだけ newName を渡す(初回起動の1件を current location にする)
    //  ・alsoSetPlan=true で、いま編集中の撮影計画の撮影場所にもこの場所を入れる。
    //    **初回起動の種のときだけ**使う(出荷時の固定計画と場所リストを食い違わせないため)。
    //    新しい場所を足したときに勝手に計画の場所が変わってはいけないので、既定は false。
    //  setPlaceDetailJson は受け取った autoInsert を真値として扱う。ここではいまの値を
    //  読んでそのまま戻すので、指定は外れない(種の場所は true で来て current location へ付け替わる)。
    private fun fillPlaceFromCurrentLocation(name: String, newName: String? = null,
                                             alsoSetPlan: Boolean = false) {
        fetchCurrentLocation { la, lo, alt ->
            Thread {
                val elev = fetchElevationOrNull(la, lo) ?: alt   // 通信は単一スレッドを塞がないよう別で
                dataExec.execute {
                // 待っている間にユーザーが名前を変えた/消したときは**何もしない**。無い名前へ書くと
                //  Entity は新しい場所を作ってしまい、一覧に幻の行が増える。
                val cur = findPlaceJson(name) ?: return@execute
                val o = JSONObject(cur.toString()).apply {
                    put("name", newName ?: name)
                    put("latitude", la); put("longitude", lo); put("altitude", elev)
                }.toString()
                HgeNative.nativeSetPlaceDetail(name, o)
                runOnUiThread {
                    if (selPlace == null) { selPlace = newName ?: name }                    // 起動直後(まだ何も選んでいない)
                    else if (selPlace == name && newName != null) { selPlace = newName }    // 改名を選択にも反映
                    // 【選択も入力中の欄も動かさない(2026-09-04 UI依頼)】開いている詳細は作り直さず
                    //  値だけ差し替える。作り直すと入力欄が壊れ、選択が動いたように見える。
                    if (selPlace == (newName ?: name)) {
                        placeLat = la; placeLng = lo; refreshPlaceCoordText()
                        placeAltEt?.setText(elev.toInt().toString())
                    }
                    // 一覧の作り直しは入力中の欄を壊すので、どこにも入力中でないときだけ行う
                    //  (作り直さなくても、次に場所を選び直した時点で最新になる)。
                    if (currentFocus == null) { buildPlacesList() }
                    // 計画への反映は**計画の単一スレッド**で行う(計画の操作は planExec に集約する決まり)。
                    //  hge_setPlanPlace がスケジュールを作り直して保存し、EV_SCHEDULE で詳細が更新される。
                    if (alsoSetPlan) {
                        planExec.execute {
                            HgeNative.nativeSetPlanPlace(newName ?: name)
                            runOnUiThread { refreshPlanList() }
                        }
                    }
                }
                }
            }.start()
        }
    }

    private fun findPlaceJson(name: String): JSONObject? {
        val arr = placeArray(HgeNative.nativeGetPlaces())
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optString("name") == name) { return o }
        }
        return null
    }

    // 標高APIを呼んで値を返す(取れなければ null)。**呼び出し側のスレッドで待つ**。
    //  入力欄へ書く fetchElevationInto と違い、画面を開いていなくても使える。
    private fun fetchElevationOrNull(lat: Double, lng: Double): Double? {
        try {
            val u = String.format(Locale.US, "https://api.open-meteo.com/v1/elevation?latitude=%.6f&longitude=%.6f", lat, lng)
            val conn = (java.net.URL(u).openConnection() as java.net.HttpURLConnection).apply {
                connectTimeout = 6000; readTimeout = 6000; requestMethod = "GET"
            }
            val body = conn.inputStream.bufferedReader().use { it.readText() }
            conn.disconnect()
            val elev = JSONObject(body).optJSONArray("elevation")?.optDouble(0, Double.NaN) ?: Double.NaN
            return if (elev.isNaN()) null else elev
        } catch (_: Exception) { return null }   // 圏外/エッジのAPへバインド中など。画面から手入力できる
    }

    private fun fetchCurrentLocation(onGot: (Double, Double, Double) -> Unit) {
        val granted = ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED ||
                      ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
        if (!granted) {
            pendingLocAction = { fetchCurrentLocation(onGot) }
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION), LOC_PERM_REQ)
            return
        }
        try {
            val lm = getSystemService(LOCATION_SERVICE) as android.location.LocationManager
            val loc = lm.getLastKnownLocation(android.location.LocationManager.GPS_PROVIDER)
                ?: lm.getLastKnownLocation(android.location.LocationManager.NETWORK_PROVIDER)
                ?: lm.getLastKnownLocation(android.location.LocationManager.PASSIVE_PROVIDER)
            if (loc == null) Toast.makeText(this, "現在地を取得できませんでした（位置情報をONにして屋外でお試しください）", Toast.LENGTH_LONG).show()
            else {
                onGot(loc.latitude, loc.longitude, if (loc.hasAltitude()) loc.altitude else 0.0)
                Toast.makeText(this, "現在地を取得しました", Toast.LENGTH_SHORT).show()
            }
        } catch (e: SecurityException) {
            Toast.makeText(this, "位置情報の権限がありません", Toast.LENGTH_LONG).show()
        }
    }

    // 型→色テーブルキー。
    private fun keyType(key: String): Int = when (key) {
        "night" -> 1; "sunrise" -> 2; "sunset" -> 3; "day" -> 4; "preNight" -> 5; "postNight" -> 6; else -> 0
    }
    private val colorTypeNames = mapOf(1 to "night", 2 to "sunrise", 3 to "sunset", 4 to "day", 5 to "preNight", 6 to "postNight")

    // システム共通の色を Entity から読み込む(0xRRGGBB)。
    private fun loadColors() {
        ccmBgMap.clear(); ccmTextMap.clear()
        try {
            val o = JSONObject(HgeNative.nativeGetColors())
            for ((t, nm) in colorTypeNames) {
                o.optJSONObject(nm)?.let { ccmTextMap[t] = it.optInt("text", 0x222222); ccmBgMap[t] = it.optInt("bg", 0xEEEEEE) }
            }
        } catch (_: Exception) {}
    }

    private fun ccmColor(type: Int): Int = 0xFF000000.toInt() or (ccmBgMap[type] ?: 0xEEEEEE)
    private fun ccmTextColor(type: Int): Int = 0xFF000000.toInt() or (ccmTextMap[type] ?: 0x222222)

    // 画面タイトルバーにシステム共通色(背景/文字)を適用する。
    private fun applyHeaderColor(headerId: Int, titleId: Int, type: Int) {
        findViewById<View>(headerId).setBackgroundColor(ccmColor(type))
        findViewById<TextView>(titleId).setTextColor(ccmTextColor(type))
    }

    private fun eventName(ev: Int): String = when (ev) {
        1 -> "Start"; 2 -> "日の入り"; 3 -> "市民薄明(夕)"; 4 -> "航海薄明(夕)"; 5 -> "天文薄明(夕)"
        6 -> "天文薄明(朝)"; 7 -> "航海薄明(朝)"; 8 -> "市民薄明(朝)"; 9 -> "日の出"
        10 -> "月の出"; 11 -> "月の入り"; 12 -> "End"; else -> "?"
    }

    // 時系列の行(イベント=灰)を並べて描画する。withBand=true のとき、各行の右側に
    // その時間帯の撮影制御方法を色分け表示する。連続する同じ種別は1つのバンドにまとめ、
    // 名称を中央に1回だけ表示する(高さは左の行群に合わせて整列。表示専用)。
    private fun renderSchedule(container: LinearLayout, o: JSONObject, highlightNow: Boolean,
                              withBand: Boolean = false) {
        container.removeAllViews()
        data class Row(val t: Long, val time: String, val label: String)
        val rows = mutableListOf<Row>()
        fun parse(s: String): Long = try { fmtIso.parse(s)?.time ?: 0L } catch (_: Exception) { 0L }
        fun hm(s: String): String = if (s.length >= 16) s.substring(5, 16).replace("T", " ") else s

        // イベント(日の出/薄明等)のみを時系列表示する。撮影制御方法は右側の色別バンドに出す。
        o.optJSONArray("events")?.let { arr ->
            for (i in 0 until arr.length()) {
                val e = arr.getJSONObject(i)
                val w = e.optString("when")
                rows.add(Row(parse(w), hm(w), eventName(e.optInt("event"))))
            }
        }
        rows.sortBy { it.t }

        // 各時間帯 windows=[start,end) を保持し、行の時刻に有効な撮影制御方法の種別を引く。
        data class Win(val type: Int, val s: Long, val e: Long)
        val wins = mutableListOf<Win>()
        o.optJSONArray("windows")?.let { arr ->
            for (i in 0 until arr.length()) {
                val w = arr.getJSONObject(i)
                wins.add(Win(w.optInt("type"), parse(w.optString("start")), parse(w.optString("end"))))
            }
        }

        // 1イベント行の TextView を作る(現在行ハイライト対応)。
        fun makeRowView(r: Row, active: Boolean, topMargin: Int): TextView {
            val tv = TextView(this)
            tv.text = if (active) "▶ ${r.time}   ${r.label}  (現在)" else "${r.time}   ${r.label}"
            if (active) tv.setTypeface(null, Typeface.BOLD)
            tv.setBackgroundColor(ccmColor(0))
            tv.setPadding(16, 8, 16, 8)
            tv.textSize = 13f
            tv.maxLines = 1   // 1行固定=等高(右バンドと正確に整列させるため)
            tv.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                    setMargins(0, topMargin, 0, 0)
                }
            return tv
        }

        if (!withBand) {
            // 従来表示: 全幅のイベント行を縦に並べる(撮影画面用。現在行をハイライト)。
            val now = System.currentTimeMillis()
            var activeIdx = -1
            if (highlightNow) { for (i in rows.indices) { if (rows[i].t <= now) activeIdx = i } }
            for ((i, r) in rows.withIndex()) container.addView(makeRowView(r, highlightNow && i == activeIdx, 1))
            return
        }

        // バンド付き表示: 左にイベント列、右に撮影制御方法バンド(BandView)を横並びにする。
        // バンドの境目は各撮影制御方法の実開始時刻から決める(イベント単位ではなく時刻基準):
        //  - 最寄りイベントの ±10分以内 → そのイベント行の「中心」から
        //  - それより前/後 → そのイベント行の手前/後ろ(行の境目)
        val n = rows.size
        val horiz = LinearLayout(this)
        horiz.orientation = LinearLayout.HORIZONTAL
        horiz.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)

        val left = LinearLayout(this)
        left.orientation = LinearLayout.VERTICAL
        left.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2f)
        for (r in rows) left.addView(makeRowView(r, false, 0))   // 余白なし=等高(バンドと整列)

        // 時刻 → 行位置(行単位。整数=行の境目, k+0.5=行kの中心)。
        fun rowPos(t: Long): Float {
            if (n == 0) return 0f
            var k = 0; var best = Math.abs(t - rows[0].t)
            for (idx in 1 until n) { val d = Math.abs(t - rows[idx].t); if (d < best) { best = d; k = idx } }
            return when {
                best <= 600000L -> k + 0.5f          // ±10分 → イベント行の中心
                t < rows[k].t   -> k.toFloat()       // 手前(行の上の境目)
                else            -> (k + 1).toFloat() // 後ろ(行の下の境目)
            }
        }

        // 撮影制御方法の区間(連続同種は統合)。開始時刻順。
        data class Run(val type: Int, val start: Long)
        val sorted = wins.sortedBy { it.s }
        val runs = mutableListOf<Run>()
        for (w in sorted) { if (runs.isEmpty() || runs.last().type != w.type) runs.add(Run(w.type, w.s)) }

        val segs = mutableListOf<BandView.Seg>()
        if (n > 0 && runs.isNotEmpty()) {
            val pos = FloatArray(runs.size + 1)
            pos[0] = 0f
            pos[runs.size] = n.toFloat()
            for (k in 1 until runs.size) pos[k] = rowPos(runs[k].start)
            for (k in 1..runs.size) pos[k] = pos[k].coerceIn(pos[k - 1], n.toFloat())  // 単調・範囲内に補正
            for (k in runs.indices) {
                val ty = runs[k].type
                val col = if (ty in 1..7) ccmColor(ty) else 0
                val lbl = if (ty in 1..7) ccmTypeName[ty] else null
                val txtCol = if (ty in 1..7) ccmTextColor(ty) else 0xFF212121.toInt()
                segs.add(BandView.Seg(pos[k], pos[k + 1], col, lbl, ty, txtCol))
            }
        }
        val band = BandView(this)
        band.segs = segs
        band.rows = n
        // 区間タップで撮影制御方法の編集画面へ(編集可能な 1..5 のみ。移行は無反応)。
        band.onTapType = { t -> ccmTypeToKey[t]?.let { openPlanCcmEdit(it) } }
        band.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
            .apply { setMargins(dp(6), 0, 0, 0) }

        horiz.addView(left)
        horiz.addView(band)
        container.addView(horiz)
    }

    // §7.3.2 スケジュール表示/編集ビュー(計画画面)。太陽高度軸(+6..-24)で夕方/朝方を分けて表示。
    private var curSunriseMode = 0
    private var curSunsetMode = 0
    // 薄明ページへの移動ボックス(時刻なし・時刻/イベント行くらいの幅=wrap)。
    //  朝の薄明=濃紺(上)→オレンジ(下)、夕方の薄明=オレンジ(上)→濃紺(下)。タップでそのページへ横スライド。
    private fun makeTwilightBox(page: Int, morning: Boolean): View {
        val orange = 0xFFF57C00.toInt(); val navy = 0xFF14274E.toInt()
        val colors = if (morning) intArrayOf(navy, orange) else intArrayOf(orange, navy)
        val g = android.graphics.drawable.GradientDrawable(
            android.graphics.drawable.GradientDrawable.Orientation.TOP_BOTTOM, colors)
        g.cornerRadius = dp(8).toFloat()
        val tv = TextView(this)
        tv.text = if (morning) "朝の薄明" else "夕方の薄明"
        tv.setTextColor(0xFFFFFFFF.toInt()); tv.textSize = 13f; tv.setTypeface(null, Typeface.BOLD)
        tv.gravity = Gravity.CENTER
        tv.maxLines = 2   // 言語によって長い場合は2行に折り返す(幅キャップと併用)
        tv.background = g
        tv.setPadding(dp(14), dp(8), dp(14), dp(8))
        tv.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { setMargins(0, dp(6), 0, dp(6)) }
        tv.setOnClickListener { planPager.setCurrent(page, true) }
        twilightBoxViews.add(tv)
        return tv
    }

    // 薄明移動ボックスの幅・高さを全ボックスで揃える(言語非依存)。
    //  幅=各ラベルの1行幅の最大値。ただし列幅(2列時は約半分)を上限とし、超える言語は2行に折り返す。
    //  高さ=その幅での各ボックスの折り返し後の高さの最大値(1行/2行が混在しても箱サイズを統一)。
    private fun equalizeTwilightBoxes(twoCol: Boolean) {
        val boxes = twilightBoxViews
        if (boxes.isEmpty()) return
        // 実際に置かれている欄の幅で決める。横向きで左右2分割にすると画面幅の半分になるため、
        //  画面幅で計算すると箱が欄からはみ出す。まだ配置前(幅0)のときだけ画面幅で見積もる。
        val hostW = if (planOverview.width > 0) planOverview.width else resources.displayMetrics.widthPixels
        val avail = hostW - dp(24) - dp(OVERVIEW_INDENT_DP)   // フォーム左右パディング + 概要の字下げ
        val cap = if (twoCol) (avail - dp(8)) / 2 else avail          // 2列なら列幅、1列なら全幅
        var maxLine = 0
        for (b in boxes) {
            val w = Math.ceil(b.paint.measureText(b.text.toString()).toDouble()).toInt() + b.paddingLeft + b.paddingRight
            if (w > maxLine) maxLine = w
        }
        val targetW = minOf(maxLine, cap).coerceAtLeast(dp(48))
        var maxH = 0
        for (b in boxes) {
            val innerW = (targetW - b.paddingLeft - b.paddingRight).coerceAtLeast(1)
            val sl = android.text.StaticLayout.Builder.obtain(b.text, 0, b.text.length, b.paint, innerW).build()
            val h = sl.height + b.paddingTop + b.paddingBottom
            if (h > maxH) maxH = h
        }
        for (b in boxes) {
            val lp = b.layoutParams
            lp.width = targetW; lp.height = maxH
            b.layoutParams = lp
        }
    }

    private fun makeDateBadge(md: String): View {
        val dh = TextView(this)
        dh.text = md; dh.setTypeface(null, Typeface.BOLD); dh.textSize = 12f
        dh.setTextColor(0xFFFFFFFF.toInt())
        dh.setPadding(dp(12), dp(3), dp(12), dp(3))
        dh.background = android.graphics.drawable.GradientDrawable().apply {
            cornerRadius = dp(9).toFloat(); setColor(0xFF66BB6A.toInt())
        }
        dh.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { setMargins(0, dp(8), 0, dp(2)) }
        return dh
    }

    private fun makeEventRow(time: String, label: String): View {
        val tv = TextView(this)
        tv.text = "$time   $label"; tv.textSize = 13f
        // 左パディングは持たせない。時刻の左端を撮影場所/カメラ…の内容と厳密に揃えるため、
        //  字下げは planOverview 側(OVERVIEW_INDENT_DP)だけで付ける(2026-08-30 UI依頼)。
        tv.setPadding(0, dp(5), dp(8), dp(5))
        return tv
    }

    // 先頭ページ: 概要スケジュール(表示専用)。時刻とイベント(Start/End/日の出/日の入/月の出/月の入)を
    // 日付ごとに時系列表示し、各薄明ページへ移動できるボックスを差し込む。
    // 列を分けるか = 薄明が2つ以上あるか。分ける位置 = 日付境界(0:00)、同じ日なら 12:00。
    // 日付バッジは列の分け方とは独立で、日付が変わるところへ必ず出す。
    // 内容の左端は他の項目(見出し96dp + 内容)と揃える。
    private fun renderOverview(o: JSONObject) {
        // 内容の左端を他の項目(見出し96dp + 内容)と揃える(2026-08-30 UI依頼)。
        //  見出し「概要スケジュール」は他の見出しと同じ左端のまま、中身だけ字下げする。
        planOverview.setPaddingRelative(dp(OVERVIEW_INDENT_DP), 0, 0, 0)
        planOverview.removeAllViews()
        twilightBoxViews.clear()
        val want = mapOf(1 to "Start", 12 to "End", 9 to "日の出", 2 to "日の入", 10 to "月の出", 11 to "月の入")
        fun parse(s: String): Long = try { fmtIso.parse(s)?.time ?: 0L } catch (_: Exception) { 0L }
        fun mdOf(d: String): String = if (d.length >= 10)
            "${d.substring(5, 7).toInt()}/${d.substring(8, 10).toInt()}" else d
        data class Ev(val t: Long, val code: Int, val md: String, val time: String, val label: String)
        val evs = ArrayList<Ev>()
        o.optJSONArray("events")?.let { arr ->
            for (i in 0 until arr.length()) {
                val e = arr.getJSONObject(i)
                val code = e.optInt("event")
                val label = want[code] ?: continue
                val w = e.optString("when")
                val date = if (w.length >= 10) w.substring(0, 10) else w
                val time = if (w.length >= 16) w.substring(11, 16) else w
                evs.add(Ev(parse(w), code, mdOf(date), time, label))
            }
        }
        evs.sortBy { it.t }
        if (evs.isEmpty()) {
            val tv = TextView(this); tv.text = "—"; tv.textSize = 13f; tv.setTextColor(0xFF888888.toInt())
            planOverview.addView(tv); return
        }

        // 薄明ブロック(=薄明ページ)。ページ0=フォームなので block i → ページ i+1。
        data class Nav(val page: Int, val morning: Boolean, val tStart: Long, val tEnd: Long)
        val navs = ArrayList<Nav>()
        o.optJSONArray("blocks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val b = arr.getJSONObject(i)
                navs.add(Nav(i + 1, b.optString("axis") != "down",
                             b.optLong("tStart", 0L) * 1000L, b.optLong("tEnd", 0L) * 1000L))
            }
        }
        // 移動ボックスの差し込み位置は**日の出/日の入のとなり**(2026-09-01 UI依頼)。
        //  夕方の薄明 … そのほとんどが日の入の後なので **日の入の直後**
        //  朝の薄明   … そのほとんどが日の出の前なので **日の出の直前**
        // 対応する日の出/日の入がその薄明の時間帯に無いとき(撮影窓の外など)は時刻で決める。
        //  朝のブロック   … ブロックが終わる時刻以降で最初のイベントの前
        //  夕方のブロック … ブロックが始まる時刻以前で最後のイベントの後
        // 【なぜ変えたか】以前は「i番目の朝 → i番目の日の出」と順番で対応付けていた。
        //  日の出/日の入が撮影窓の外にあると対応先が無くなり、End の前へ落ちる。
        //  5:00〜21:00 の計画(日の出4:56が窓の外)で「朝の薄明」が右列の End 直前に
        //  出てしまう不具合が実際に起きた。時刻で決めれば窓の内外に関係なく正しい位置になる。
        val before = HashMap<Int, MutableList<Nav>>()
        val after = HashMap<Int, MutableList<Nav>>()
        for (nv in navs) {
            if (nv.morning) {
                // 朝はその薄明の中にある日の出(9)の直前。無ければ終了時刻以降の最初のイベントの前。
                var at = evs.indexOfFirst { it.code == 9 && it.t >= nv.tStart && it.t <= nv.tEnd }
                if (at < 0) at = evs.indexOfFirst { it.t >= nv.tEnd }
                if (at < 0) at = evs.size - 1                      // 後ろに何も無ければ最後の前
                before.getOrPut(at) { mutableListOf() }.add(nv)
            } else {
                // 夕方はその薄明の中にある日の入(2)の直後。無ければ開始時刻以前の最後のイベントの後。
                var at = evs.indexOfFirst { it.code == 2 && it.t >= nv.tStart && it.t <= nv.tEnd }
                if (at < 0) at = evs.indexOfLast { it.t <= nv.tStart }
                if (at < 0) at = 0                                  // 前に何も無ければ先頭の後
                after.getOrPut(at) { mutableListOf() }.add(nv)
            }
        }

        // 表示物を1本の並びにする。列の切れ目は薄明ボックスの前後にも置きたいので、
        //  イベント番号ではなくこの並びの番号で分割位置を決める。
        // md は「その表示物が属する日」。薄明ボックスは差し込み先のイベントと同じ日にする
        //  (朝のボックスは次のイベントの前に入るので、日付バッジをボックスより先に出すため)。
        class Node(val ev: Ev?, val nav: Nav?, val md: String)
        val nodes = ArrayList<Node>()
        for ((idx, ev) in evs.withIndex()) {
            before[idx]?.forEach { nodes.add(Node(null, it, ev.md)) }
            nodes.add(Node(ev, null, ev.md))
            after[idx]?.forEach { nodes.add(Node(null, it, ev.md)) }
        }

        // 縦2列にする分割位置 k(nodes[k] 以降が右列)。2026-08-30 に決めたルール:
        //  薄明1つ              … 1列のまま
        //  薄明2つ・間に日付境界あり … その 0:00 で分ける
        //  薄明2つ・間に日付境界なし … 同じ日の 12:00 で分ける
        //  薄明3つ以上           … 日付が変わるところで分ける
        // 切れ目は必ず「時刻」(0:00 か 12:00)なので、画面を見なくても境目が言える。
        val cal = java.util.Calendar.getInstance()
        fun clockOf(base: Long, hour: Int): Long {      // base と同じ日の hour:00
            cal.timeInMillis = base
            cal.set(java.util.Calendar.HOUR_OF_DAY, hour); cal.set(java.util.Calendar.MINUTE, 0)
            cal.set(java.util.Calendar.SECOND, 0); cal.set(java.util.Calendar.MILLISECOND, 0)
            return cal.timeInMillis
        }
        // 時刻 t 以降の最初のノード。見つからなければ -1 を返し、下の twoCol 判定で1列に落ちる。
        // 切れ目の直前にある薄明ボックスは「その後ろのイベントに掛かる見出し」なので、
        //  薄明そのものが t より後ろなら一緒に右列へ送る(朝は明ける時刻、夕方は始まる時刻で見る)。
        //  これをしないと、夕方の薄明だけ左列の末尾に残って夕方のイベントと離れてしまう。
        fun splitAt(t: Long): Int {
            var s = nodes.indexOfFirst { it.ev != null && it.ev.t >= t }
            while (s > 0) {
                val nv = nodes[s - 1].nav ?: break
                if ((if (nv.morning) nv.tEnd else nv.tStart) < t) { break }
                s--
            }
            return s
        }
        var k = 0
        if (navs.size >= 3) {
            val newDay = evs.firstOrNull { it.md != evs.first().md }
            // 薄明3つ以上は必ず2日にまたがるので newDay は在るが、念のため 12:00 を保険にする。
            k = if (newDay != null) splitAt(clockOf(newDay.t, 0)) else splitAt(clockOf(navs.first().tStart, 12))
        } else if (navs.size == 2) {
            val t1 = minOf(navs[0].tStart, navs[1].tStart)
            val t2 = maxOf(navs[0].tStart, navs[1].tStart)
            val zero = clockOf(t2, 0)                   // 後ろの薄明の日の 0:00
            k = if (t1 < zero) splitAt(zero)            // 2つの薄明の間に日付境界がある
                else splitAt(clockOf(t1, 12))           // 同じ日 → 昼の12:00
        }
        val twoCol = k in 1 until nodes.size            // 右列が空になるなら1列に落とす

        val col1: LinearLayout
        val col2: LinearLayout
        if (twoCol) {
            val rowL = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            col1 = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            col2 = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
            rowL.addView(col1, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            rowL.addView(col2, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(dp(8), 0, 0, 0) })
            planOverview.addView(rowL, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        } else { col1 = planOverview; col2 = planOverview }

        // 日付バッジは「読む順(左列を上から、続けて右列を上から)で日付が変わったところ」に必ず1つ置く。
        //  列の分け方とは独立。1列でも日付をまたげば途中に出るし(例 23:00開始→翌08:00)、
        //  右列の先頭が左列の続きと同じ日付なら右列の頭には出さず、変わる位置=イベントの途中に入る。
        //  ノードの md を見るので、日付の変わり目に薄明ボックスが重なってもバッジが先に出る。
        var curDate = ""
        for ((idx, n) in nodes.withIndex()) {
            val col = if (twoCol && idx >= k) col2 else col1
            if (n.md != curDate) { curDate = n.md; col.addView(makeDateBadge(n.md)) }
            val ev = n.ev
            if (ev != null) { col.addView(makeEventRow(ev.time, ev.label)) }
            else { val nv = n.nav!!; col.addView(makeTwilightBox(nv.page, nv.morning)) }
        }
        equalizeTwilightBoxes(twoCol)   // 全ボックスの幅・高さを揃える(言語非依存)
    }

    // §7.3.2 薄明ページ(横スライド)を再構築する。blocks[] の各要素=1ページ(ScheduleView)。
    // ページ0(フォーム)は残し、以前の薄明ページを差し替える。ページ番号(タイトル)も更新。
    private fun rebuildTwilightPages(o: JSONObject) {
        // 朝日/夕日を「使う」かは計画が持つ(2026-08-11 改定)。1=使う / 2=使わない で保持する
        // (setBand が送る値と揃える。C++ 側は 2 以外を「使う」とみなす)。
        curSunriseMode = if (o.optBoolean("useSunrise", true)) 1 else 2
        curSunsetMode  = if (o.optBoolean("useSunset",  true)) 1 else 2
        for (p in twilightPages) planPager.removeView(p)
        twilightPages.clear()
        schedulePages.clear()
        val ed = !planReadOnly
        val planName = o.optString("planName")
        o.optJSONArray("blocks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val b = arr.getJSONObject(i)
                val segs = ArrayList<ScheduleView.Seg>()
                b.optJSONArray("segments")?.let { sa ->
                    for (k in 0 until sa.length()) {
                        val s = sa.getJSONObject(k); val ty = s.optInt("type")
                        val col = if (ty in 1..7) ccmColor(ty) else 0
                        val tc = if (ty in 1..7) ccmTextColor(ty) else 0xFF212121.toInt()
                        val nm = ccmTypeName[ty] ?: s.optString("name")
                        segs.add(ScheduleView.Seg(ty, nm, s.optDouble("altTop"),
                            s.optDouble("altBottom"), s.optBoolean("used"), col, tc))
                    }
                }
                val marks = ArrayList<ScheduleView.Mark>()
                b.optJSONArray("marks")?.let { ma ->
                    for (k in 0 until ma.length()) {
                        val m = ma.getJSONObject(k)
                        marks.add(ScheduleView.Mark(m.optString("label"), m.optString("time"), m.optDouble("alt")))
                    }
                }
                val axisDown = b.optString("axis") == "down"
                val block = ScheduleView.Block(if (axisDown) "夕方の薄明" else "朝の薄明", axisDown,
                    b.optString("date"), segs, marks)
                val sv = ScheduleView(this)
                sv.onTapType = { t -> ccmTypeToKey[t]?.let { k -> openPlanCcmEdit(k) } }
                sv.onMoveBoundary = { before, after, occ, altDeg, rising ->
                    Thread { HgeNative.nativeSetBoundaryByAlt(before, after, occ, altDeg, rising) }.start()
                }
                sv.onSetBand = { rising, insert -> setBand(rising, insert) }
                sv.isEnabled = ed
                sv.setData(listOf(block))
                // ページ=[計画名ヘッダ(タイトル行の下)] + [ScheduleView(残り全面)]。縦スクロールなし。
                val page = LinearLayout(this)
                page.orientation = LinearLayout.VERTICAL
                val nameTv = TextView(this)
                nameTv.text = planName.ifEmpty { "撮影計画" }
                nameTv.setTypeface(null, Typeface.BOLD); nameTv.textSize = 15f
                nameTv.maxLines = 1; nameTv.ellipsize = android.text.TextUtils.TruncateAt.END
                nameTv.setPadding(dp(12), dp(6), dp(12), dp(6))
                nameTv.setBackgroundColor(0xFFE3F2FD.toInt())
                page.addView(nameTv, LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
                page.addView(sv, LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
                planPager.addView(page, FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
                twilightPages.add(page)
                schedulePages.add(sv)
            }
        }
        // 最終ページ = 撮影シミュレーション(§7.3 画面360)。永続1インスタンスを毎回末尾へ付け直す。
        ensureSimReady()
        simPage?.let { sp ->
            planPager.removeView(sp)
            planPager.addView(sp, FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            sp.setPlanName(planName.ifEmpty { "撮影計画" })   // タイトル1行目(2026-08-08 UI依頼)
            sp.setGearText(simGearText)                      // センサー/焦点距離/画角(同上)
            // 方位磁石の日の出/日の入・月マーカー(表示JSONに含まれる)を反映。
            sp.setMarkers(
                o.optDouble("sunriseAz", Double.NaN).toFloat(),
                o.optDouble("sunsetAz", Double.NaN).toFloat(),
                o.optDouble("moonriseAz", Double.NaN).toFloat(),
                o.optDouble("moonsetAz", Double.NaN).toFloat())
            // 自己完結の撮影計画JSON(place/camera/lens/start/end 等)を読んで初期化・描画。
            // マスターレンズ(起動時にアセットから再コピー)も渡し、fisheye をファイル由来で即反映させる。
            simExec.execute {
                val raw = try { HgeNative.nativeGetPlanJson() } catch (e: Exception) { "" }
                val ml = try { HgeNative.nativeGetMasterLenses() } catch (e: Exception) { "[]" }
                runOnUiThread { sp.bind(raw, ml) }
            }
        }
        planPager.refreshPages()
        updatePagerTitle()
    }

    // シミュレーションページの下準備: 恒星(fixed_star.json)を一度読み込み、ページを1度だけ生成する。
    private fun ensureSimReady() {
        if (!starsLoadStarted) {
            starsLoadStarted = true
            simExec.execute {
                try {
                    val json = assets.open("fixed_star.json").bufferedReader().use { it.readText() }
                    HgeNative.nativeSimLoadStars(json)
                } catch (_: Exception) {}
            }
        }
        if (simPage == null) {
            simPage = SimPage(this, simExec,
                onLandscape = { checked ->
                    // 横向きチェックは撮影計画へ反映(先頭ページと同じ)。再生成→EV_SCHEDULEで再bind。
                    planExec.execute { HgeNative.nativeSetPlanLandscape(if (checked) 1 else 0) }
                },
                onDirection = { az, el ->
                    // 撮影方向/仰角の確定を撮影計画へ保存(cs の azimuth/elevation を永続化)。
                    // これで再表示や時刻変更で戻っても向きが保持される。再生成→EV_SCHEDULEで再bind。
                    pushDirectionToEntity(az, el)
                })
        }
    }

    // タイトル行は全ページ共通で「撮影計画 現在/総数」。計画名は薄明ページ内(タイトル行の下)に表示する。
    private fun updatePagerTitle() {
        val n = planPager.pageCount.coerceAtLeast(1)
        val cur = planPager.current
        val head = if (tplMode) "撮影計画ひな形" else "撮影計画"
        findViewById<TextView>(R.id.plan_title).text = "$head  ${cur + 1}/$n"
    }

    // 撮影要求済(撮影中/待機/未検出)の計画を表示しているときは一切編集できない(item7)。各操作部の有効/無効を切替。
    private fun updateReadOnly() {
        // 項目6: 撮影中/待機/未検出に加え、エッジ端末に送信済み(=どこかのエッジがロスターに保有)の計画も
        //  編集不可。エッジで停止して保持中(再開可能)の間もエッジが持ち主なので変更させない。スマホで停止すれば
        //  エッジから消え、ロックが解けて再び編集できる。※エッジ選択(スピナー)しただけの未開始計画はロックしない。
        planReadOnly = isPlanOnEdge(currentPlanId)
        val ed = !planReadOnly
        // plan_resetButton(=変更の取り消し)は planDirtyWatch が有効/無効を管理(撮影中は自動で無効)。
        // plan_endDate は自動決定で常時グレー・タップ不可のため対象外(2026-08-08 UI依頼)。
        intArrayOf(R.id.plan_startDate, R.id.plan_startTime, R.id.plan_endTime)
            .forEach { findViewById<View>(it).isEnabled = ed }
        intervalText.isEnabled = ed; landscapeCheck.isEnabled = ed
        syncShotCheck.isEnabled = ed; subCamText.isEnabled = ed	// 同期撮影も撮影中は編集不可
        cameraText.isEnabled = ed; lensText.isEnabled = ed; edgeSpinner.isEnabled = ed
        // 薄明ページと撮影制御方法ボタンは**ロック中も触れる**(2026-09-01 UI依頼)。
        //  帯をタップして中身を見たいため。編集(境目の移動・帯の出し入れ)だけを止める。
        schedulePages.forEach { it.isEnabled = true; it.readOnly = planReadOnly }
        findViewById<LinearLayout>(R.id.plan_ccmButtons).let { for (i in 0 until it.childCount) it.getChildAt(i).isEnabled = true }
    }

    // 夕日/朝日を使う(insert=true)/使わない(insert=false)。他方の指定は保持する。
    // 「使わない」にしても計画が持つ実体は消えないので、また使うことにすれば以前の編集内容が戻る。
    private fun setBand(rising: Boolean, insert: Boolean) {
        var sr = curSunriseMode
        var ss = curSunsetMode
        val m = if (insert) 1 else 2
        if (rising) sr = m else ss = m
        Thread { HgeNative.nativeSetBandMode(sr, ss) }.start()
    }

    // 撮影周期の表示(小数第1位まで。整数なら小数点を出さない)。15.0 -> "15" / 15.5 -> "15.5"
    // 撮影周期は常に小数第1位まで表示する(整数でも「15.0」)。
    private fun fmtInterval(v: Double): String = String.format("%.1f", v)

    // 撮影周期をキーボード入力(秒・小数第1位まで)。最小周期(最長ss+2)未満は警告して反映しない。
    private fun editInterval() {
        val cur = try { JSONObject(latestSchedule).optDouble("interval", 15.0) } catch (_: Exception) { 15.0 }
        val et = EditText(this)
        // 小数を入力できるようにする(長秒ss時に ss+2.0/+2.5/+3.0 のような細かい設定を行うため)。
        et.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
        et.setText(fmtInterval(cur))
        et.setSelection(et.text.length)
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("撮影周期(秒)")
            .setView(et)
            .setPositiveButton("OK") { _, _ ->
                val sec = et.text.toString().trim().toDoubleOrNull()
                if (sec == null) {
                    Toast.makeText(this, "数値で入力してください", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                planExec.execute {
                    val r = HgeNative.nativeSetPlanInterval(sec)
                    runOnUiThread {
                        // 拒否されたときに何が起きたか分かるよう、入力値と最小周期を出す(黙って元へ戻ると原因が分からない)。
                        if (r != 0) Toast.makeText(this,
                            "撮影周期 ${fmtInterval(sec)}秒 は設定できません(最小周期未満)。先にシャッター速度を短くしてください",
                            Toast.LENGTH_LONG).show()
                    }
                }
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // センサー/レンズ定数を変更する(機材リストに無い値の参考用。NPF/画角が再計算される)。

    // --- エッジ端末 ---
    private fun selectedEdge(): Edge? {
        val i = edgeSpinner.selectedItemPosition
        return if (i in 1..edgeSpinnerEdges.size) edgeSpinnerEdges[i - 1] else null
    }

    // スピナーに並べるエッジ端末。**名前順**にする(2026-09-01 UI依頼)。登録順のままだと増えるほど探しにくい。
    //  位置→端末の対応はこの一覧が権威(選択の保存もここから引く)。
    //  登録一覧 edges そのものは並べ替えない(エッジ端末設定の並びと保存順を変えないため)。
    private var edgeSpinnerEdges: List<Edge> = emptyList()
    private fun sortedEdges(): List<Edge> =
        edges.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })

    // --- エッジ端末の登録(prefsに永続化。設定で追加・検索で自動登録。オフラインでも選択可) ---
    private fun hgcPrefs() = getSharedPreferences("hgc", MODE_PRIVATE)

    // ── エッジのネットワーク設定を覚えておく(2026-08-29 UI依頼) ──────────────
    //
    // 【なぜ要るか】以前は画面を離れると入力が消え、エッジを選び直すたびに SSID と
    //  パスワードを打ち直していた。しかも AP と STA でこの2欄の意味が違う
    //  (AP=エッジが立てる側の資格 / STA=参加先の資格)ので、**モードを切り替えると
    //  前のモードで入れた値が上書きされて失われる**。STA→AP→STA と戻ったときに
    //  元の設定へ戻れるよう、**モードごとに別々に**覚える。
    //
    // 置き場: prefs の "ec_<端末名>" に {"ap":真偽,"sta":{ssid,pass},"apCfg":{ssid,pass}}。
    //  端末名がキーなので、改名したら付け替えが要る(renameRegisteredEdge から呼ぶ)。
    private class EdgeCfg(var ap: Boolean = false,
                          var staSsid: String = "", var staPass: String = "",
                          var apSsid: String = "",  var apPass: String = "")

    private fun edgeCfgKey(name: String) = "ec_" + name

    private fun loadEdgeCfg(name: String): EdgeCfg {
        val c = EdgeCfg()
        if (name.isEmpty()) return c
        try {
            val o = JSONObject(hgcPrefs().getString(edgeCfgKey(name), "") ?: "")
            c.ap = o.optBoolean("ap", false)
            o.optJSONObject("sta")?.let { c.staSsid = it.optString("ssid"); c.staPass = it.optString("pass") }
            o.optJSONObject("apCfg")?.let { c.apSsid = it.optString("ssid"); c.apPass = it.optString("pass") }
        } catch (_: Exception) {}
        return c
    }

    private fun saveEdgeCfg(name: String, c: EdgeCfg) {
        if (name.isEmpty()) return
        try {
            val o = JSONObject()
            o.put("ap", c.ap)
            o.put("sta", JSONObject().put("ssid", c.staSsid).put("pass", c.staPass))
            o.put("apCfg", JSONObject().put("ssid", c.apSsid).put("pass", c.apPass))
            hgcPrefs().edit().putString(edgeCfgKey(name), o.toString()).apply()
        } catch (_: Exception) {}
    }

    // ── 一度つないだ SSID のパスワード(端末ごとではなく全体で1つ) ────────────
    //
    // 【なぜ端末ごとにしないか】同じ家/現場の Wi-Fi へ何台も参加させるのが普通で、
    //  同じパスワードを台数ぶん打ち直すのは無駄。SSID が同じならパスワードも同じ、
    //  という前提はこの用途では妥当。**SSID を選んだ/入れたときに自動で埋める**。
    //  違っていればユーザーが上書きすればよく、そのとき覚え直す。
    private fun knownWifiPass(ssid: String): String {
        if (ssid.isEmpty()) return ""
        return try { JSONObject(hgcPrefs().getString("wifiPass", "{}") ?: "{}").optString(ssid, "") }
               catch (_: Exception) { "" }
    }

    private fun rememberWifiPass(ssid: String, pass: String) {
        if (ssid.isEmpty() || pass.isEmpty()) return
        try {
            val o = JSONObject(hgcPrefs().getString("wifiPass", "{}") ?: "{}")
            if (o.optString(ssid, "") == pass) return       // 変化なしは書かない
            o.put(ssid, pass)
            hgcPrefs().edit().putString("wifiPass", o.toString()).apply()
        } catch (_: Exception) {}
    }

    // 【壊れた行は読み込み時に落とす(2026-09-02)】台帳は一度おかしな行が入ると保存され続け、
    //  自分では直らなかった。エッジのLCDは半角英数字しか出せないので、非ASCIIの名前は
    //  端末名ではない=別種のJSON(撮影計画の進捗)が紛れ込んだ跡。落として書き戻す。
    private fun loadRegisteredEdges() {
        edges.clear()
        var dropped = false
        try {
            val a = JSONArray(hgcPrefs().getString("regEdges", "[]") ?: "[]")
            for (i in 0 until a.length()) {
                val o = a.getJSONObject(i)
                val nm = o.optString("name")
                if (nm.isEmpty() || !isAsciiEdgeName(nm)) { dropped = true; continue }
                edges.add(Edge(nm, o.optString("ip"), o.optInt("port", 50506)))
            }
        } catch (_: Exception) { return }   // 読めないときは触らない(消してしまわない)
        if (dropped) saveRegisteredEdges()
    }
    // エッジの端末名を変更したとき、スマホ側の登録も追従させる(2026-08-08)。
    //  ・登録済みリスト(regEdges)の名前を差し替える(新名が既にあれば古い方を消すだけ)
    //  ・計画ごとのエッジ割り当て(pe_<planId>)も付け替える。ここを直さないと担当が外れる
    // oldName が空(新規登録)や、新旧が同じときは何もしない。
    private fun renameRegisteredEdge(oldName: String, newName: String) {
        if (oldName.isEmpty() || newName.isEmpty() || oldName == newName) return
        val idx = edges.indexOfFirst { it.name == oldName }
        if (idx < 0) return
        if (edges.any { it.name == newName }) edges.removeAt(idx)          // 新名が既にある → 重複を残さない
        else edges[idx] = Edge(newName, edges[idx].ip, edges[idx].port)
        saveRegisteredEdges()
        // ネットワーク設定(ec_<名前>)も付け替える。置いていくと、次に同じ名前を作ったときに
        //  前の持ち主の設定が出てくるうえ、本人の設定は迷子になる。新名が既に自分の設定を
        //  持っているなら、そちらを正として古い方を捨てるだけにする。
        if (hgcPrefs().getString(edgeCfgKey(newName), "").isNullOrEmpty())
        { saveEdgeCfg(newName, loadEdgeCfg(oldName)) }
        hgcPrefs().edit().remove(edgeCfgKey(oldName)).apply()
        // 計画の割り当てを付け替える(prefs の pe_<planId> に端末名で入っている)。
        val pf = hgcPrefs()
        val ed = pf.edit()
        pf.all.keys.filter { it.startsWith("pe_") }.forEach { k ->
            if (pf.getString(k, "") == oldName) ed.putString(k, newName)
        }
        ed.apply()
        refreshEdgeSpinner()
        android.util.Log.i("EdgeProv", "renamed registered edge: $oldName -> $newName")
    }


    // ---------- 8.2 エッジ端末設定(画面。2026-08-08 UI依頼で2ダイアログを1画面へ統合) ----------
    //
    // 【なぜ画面にしたか】従来は「エッジ端末の登録」と「エッジ端末設定」の2ダイアログに分かれ、
    //  登録は片方、設定はもう片方という往復が要った。他の設定画面と同じ「上=一覧/分割バー/
    //  下=入力」に揃えて1画面にし、一覧で選ぶ→下で設定、新規は一覧の先頭行から、で完結させる。
    //
    // 一覧で選んだ端末が設定対象(selectedEdgeName)になり、BLEのQR表示要求もその名前の機体
    // だけに向ける。新規(未選択)のときだけ、名前を広告しない出荷時のエッジにも反応する。
    private var selectedEdgeName = ""          // 一覧で選択中の端末名(空=新規追加)
    private var edgeApMode = false             // AP/STA の切替状態(ラベルの出し分けにも使う)
    private var edgeNameEt: EditText? = null
    private var edgeSsidEt: EditText? = null
    private var edgePassEt: EditText? = null
    private var edgeSsidLabel: TextView? = null
    private var edgePassLabel: TextView? = null
    private var edgePopView: TextView? = null
    // QR側からモードを反映するためスイッチ本体も保持する(2026-08-08)。
    private var edgeApSwitch: CompoundButton? = null

    private fun openEdgeSettings() {
        // 【開いたときは先頭の端末を選んでおく(2026-08-29 UI依頼)】以前は必ず新規追加の
        //  状態で開いていた。ふだんは既にある端末を見に来るので、毎回1タップ余分だった。
        //  並びは一覧と同じアルファベット順。**1件も無いときだけ**新規で開く。
        selectedEdgeName = edges.sortedBy { it.name.lowercase() }.firstOrNull()?.name ?: ""
        edgeApMode = loadEdgeCfg(selectedEdgeName).ap   // その端末を最後に設定したモードで開く
        scannedPop = ""; scannedName = ""
        buildEdgeList(); buildEdgeForm()
        setInitialSplit(R.id.edge_container)
        flipper.displayedChild = 15
    }

    // 上段: 登録済み一覧。**他の一覧画面(所持カメラ/レンズ)と同じ作りに揃える**(2026-08-29 UI依頼)。
    //  ・行は共通の listRow。右端は「削除」ボタンではなく **⋮ のコンテキストメニュー**
    //  ・「削除」「すべて削除」はそのメニューの項目
    //  ・「＋ 新規エッジ端末」は**一覧の一番下**(所持カメラの「＋ 新規カメラ追加」と同じ位置)
    //  以前はここだけ独自の行を組み立てていて、右端に素の Button が出ていた。
    private fun buildEdgeList() {
        renderList(ListPane(
            containerId = R.id.edge_container,
            // 並びはアルファベット順(大文字小文字を区別しない)。登録した順だと、増えたときに
            //  どこにあるか分からなくなる。**表示の並びだけ**で、保存の順は変えない。
            rows = {
                edges.sortedBy { it.name.lowercase() }.map { e ->
                    // 副行は**いま届いているか**。IP は普段読んでも何もできないのでやめた
                    //  (2026-08-29 UI依頼)。押す前に「送っても無駄」と分かるのが要点。
                    val sub = when (edgeOnline[e.name]) {
                        true  -> "オンライン"
                        false -> "オフライン"
                        else  -> "確認中"        // 起動直後、まだ一度もスイープしていない
                    }
                    ListItem(e.name, e.name, sub, listOf(
                        "削除" to { confirmRemoveEdge(e) },
                        "すべて削除" to { confirmRemoveAllEdges() }))
                }
            },
            selected = { selectedEdgeName.ifEmpty { null } },
            setSelected = { selectedEdgeName = it ?: "" },
            onSelect = { nm ->
                stashEdgeForm()                          // 今見ている端末の入力を控えてから切り替える
                selectedEdgeName = nm
                scannedPop = ""; scannedName = ""
                edgeApMode = loadEdgeCfg(nm).ap          // その端末を最後に設定したモードで開く
                buildEdgeList(); buildEdgeForm()
            },
            // 名前は**この行で直接**直す(所持カメラ/レンズと同じ)。以前は下の設定欄に
            //  「端末識別名」があり、一覧と入力欄の2か所に名前があって分かりにくかった。
            onRename = { orig, nm -> commitEdgeRename(orig, nm) },
            addLabel = "＋ 新規端末", onAdd = { addEdge() },
            emptyText = "(登録なし。下の「＋ 新規端末」から追加)"))
    }

    // 【「＋ 新規エッジ端末」は押した時点で登録する(2026-09-04 UI依頼)】以前は一覧の最下行が
    //  名前の入力欄で、打ってから「登録だけする」か「設定を送信」で台帳へ入れていた。
    //  他の一覧(撮影場所/所持カメラ/所持レンズ)は押した時点で作って行で名前を直す形なので
    //  そちらへ揃えた。できる状態は旧「登録だけする」と同じ(IPは空・ポートは既定値)。
    //  名前は仮なので、行をタップして実機に合わせて直す。半角英数字だけ(エッジのLCDに出る)。
    private fun addEdge() {
        var n = 0
        var nm = "Edge%02d".format(n)
        while (edges.any { it.name == nm }) { n++; nm = "Edge%02d".format(n) }
        edges.add(Edge(nm, "", 50506))
        selectedEdgeName = nm
        scannedPop = ""; scannedName = ""
        edgeApMode = false
        saveRegisteredEdges(); refreshEdgeSpinner()
        buildEdgeList(); buildEdgeForm()
    }

    // 一覧での名称インライン編集の確定。
    //  エッジの名前は**エッジのLCDにも出る**ので半角英数字だけ(日本語は表示できない)。
    //  ここで弾かないと、送信の段になって初めて叱られることになる。
    private fun commitEdgeRename(orig: String, newName: String) {
        val nm = newName.trim()
        if (nm.isEmpty() || nm == orig) { buildEdgeList(); return }
        if (!isAsciiEdgeName(nm)) {
            Toast.makeText(this, "端末識別名は半角英数字で入力してください(外部端末で日本語は表示できません)", Toast.LENGTH_LONG).show()
            buildEdgeList(); return
        }
        if (edges.any { it.name == nm }) { showNameInUse(nm); buildEdgeList(); return }
        stashEdgeForm()                    // 旧名のまま入力を残してから付け替える
        renameRegisteredEdge(orig, nm)     // 登録・計画の割り当て・ネットワーク設定を移す
        if (selectedEdgeName == orig) selectedEdgeName = nm
        buildEdgeList(); buildEdgeForm(); refreshEdgeSpinner()
    }

    // 登録から1台外す。**エッジ本体の設定は変えない**(こちらの台帳から消すだけ)。
    private fun confirmRemoveEdge(e: Edge) {
        AlertDialog.Builder(this)
            .setTitle("外部端末の削除")
            .setMessage("「" + e.name + "」を登録から削除しますか？(端末本体の設定は変わりません)")
            .setPositiveButton("削除する") { _, _ ->
                edges.remove(e); saveRegisteredEdges(); refreshEdgeSpinner()
                // 【逃げ道】この端末が持っていた計画の縛りも一緒に解く。壊れた/失くした
                //  端末の分がいつまでも残ると、そのカメラを永久に変更も削除もできなくなる。
                edgeHeldByEdge.remove(e.name); saveEdgeHeld()
                hgcPrefs().edit().remove(edgeCfgKey(e.name)).apply()   // その端末のネットワーク設定も捨てる
                if (selectedEdgeName == e.name) selectedEdgeName = ""
                buildEdgeList(); buildEdgeForm(); refreshPlanList(); updateReadOnly()
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    // 登録を全部外す。台数を出してから訊く(誤爆の重さが分かるように)。
    private fun confirmRemoveAllEdges() {
        if (edges.isEmpty()) return
        AlertDialog.Builder(this)
            .setTitle("すべて削除")
            .setMessage("登録している" + edges.size + "台をすべて削除しますか？(端末本体の設定は変わりません)")
            .setPositiveButton("すべて削除") { _, _ ->
                val names = edges.map { it.name }
                edges.clear(); saveRegisteredEdges(); refreshEdgeSpinner()
                val ed = hgcPrefs().edit()
                for (n in names) { edgeHeldByEdge.remove(n); ed.remove(edgeCfgKey(n)) }
                ed.apply()
                saveEdgeHeld()
                selectedEdgeName = ""
                buildEdgeList(); buildEdgeForm(); refreshPlanList(); updateReadOnly()
            }
            .setNegativeButton("やめる", null)
            .show()
    }

    // いま画面に出ている入力を、その端末の設定として残す。
    //  端末を切り替える・画面を離れる・送信する、のいずれでも呼ぶ。呼び忘れると
    //  「打ったのに次に開いたら消えている」になる。新規(名前が未定)のときは残さない。
    private fun stashEdgeForm() {
        val name = selectedEdgeName
        if (name.isEmpty()) return
        val sid = edgeSsidEt?.text?.toString() ?: return
        val pw  = edgePassEt?.text?.toString() ?: ""
        val c = loadEdgeCfg(name)
        c.ap = edgeApMode
        if (edgeApMode) { c.apSsid = sid; c.apPass = pw } else { c.staSsid = sid; c.staPass = pw }
        saveEdgeCfg(name, c)
        // STA のときだけ、SSID とパスワードの対応を全体の控えにも残す。
        //  AP のときの資格は「エッジが立てる側」のものなので、参加先の控えに混ぜてはいけない。
        if (!edgeApMode) { rememberWifiPass(sid, pw) }
    }

    private fun buildEdgeForm() {
        val ctx = this
        val d = resources.displayMetrics.density
        val box = findViewById<LinearLayout>(R.id.edge_form)
        box.removeAllViews()
        // 端末が1台も無いとき(=まだ何も選べない)は設定欄を出さない。出しても送る先が無く、
        //  送信を押して初めて叱られることになる。先に一覧の「＋ 新規エッジ端末」を押してもらう。
        if (selectedEdgeName.isEmpty()) {
            box.addView(TextView(ctx).apply {
                text = "上の「＋ 新規端末」で端末を追加してください"
                setTextColor(Color.GRAY); setPadding(0, dp(16), 0, dp(16))
            })
            return
        }

        fun label(t: String): TextView {
            val v = TextView(ctx).apply { text = t; setPadding(0, (10 * d).toInt(), 0, (2 * d).toInt()) }
            box.addView(v); return v
        }

        box.addView(TextView(ctx).apply {
            text = if (selectedEdgeName.isEmpty()) "端末の設定" else "「" + selectedEdgeName + "」の設定"
            setTypeface(null, Typeface.BOLD)
            textSize = 16f
        })

        // 【名前は一覧で直す(2026-08-29 UI依頼)】ここには入力欄を置かない。一覧の行を
        //  直接編集する(所持カメラ/レンズと同じ)。同じ名前が2か所にあると、どちらが
        //  本物か分からなくなるため。送信処理はこの隠しの欄から名前を読む。
        if (selectedEdgeName.isNotEmpty()) {
            val nameE = EditText(ctx); applyEdgeNameInput(nameE)
            nameE.setText(selectedEdgeName)   // 画面には出さない。送信処理がここから名前を読む
            edgeNameEt = nameE
        }

        // ネットワークモード。ONでエッジ自身がAP(屋外・ルーター無し)、OFFで既存ネットへ参加。
        val apSwitch = android.widget.Switch(ctx).apply {
            text = "端末をAPにする (OFF=既存ネットに接続)"
            isChecked = edgeApMode
            setPadding(0, (12 * d).toInt(), 0, 0)
        }
        box.addView(apSwitch)

        // SSID/password。APモードでは「エッジが立てるAPの資格」、STAでは「参加先の資格」。
        // 2026-08-08 UI依頼: どちらを編集しているのかが分かるようラベルを出し分ける。
        val ssidLabel = label("接続先 SSID")
        // SSID 欄と選択ボタンは**同じ行**に置く(2026-08-29 UI依頼)。縦に積むと、
        //  この画面だけで1画面ぶんの高さを使ってしまい、下のログ設定まで届かない。
        val ssidE = EditText(ctx)
        val ssidPickBtn = blueButton("SSID選択") { }   // 押した時の処理は下で入れる
        ssidPickBtn.layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            setMargins(dp(6), 0, 0, 0)
        }
        box.addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(ssidE, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(ssidPickBtn)
        })
        val passLabel = label("接続先 password")
        // 既定で見えるようにする(2026-08-27 UI依頼)。理由はカメラのパスワード欄と同じ。
        val passE = EditText(ctx).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        }
        // 【伏せ字を外せるようにする(2026-08-27 UI依頼)】繋がらないときに打ち間違いを
        //  確かめられないと原因が絞れない。所持カメラのパスワード欄と同じ作りにする。
        box.addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(passE, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            addView(CheckBox(ctx).apply {
                text = "表示"; textSize = 12f; isChecked = true
                setOnCheckedChangeListener { _, on ->
                    val p = passE.selectionStart
                    passE.inputType = if (on) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                                      else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
                    passE.setSelection(p.coerceIn(0, passE.text.length))
                }
            })
        })
        edgeSsidEt = ssidE; edgePassEt = passE; edgeSsidLabel = ssidLabel; edgePassLabel = passLabel

        // 【モードごとに別々に覚える】この2欄は AP と STA で意味が違う
        //  (AP=エッジが立てる側の資格 / STA=参加先の資格)。1組しか持たないと、
        //  モードを切り替えた瞬間に前のモードの値が消える。STA→AP→STA と戻ったとき
        //  元の設定が出るように、両方を保持して切り替えのたびに入れ替える。
        val cfg = loadEdgeCfg(selectedEdgeName)
        fun fillFor(ap: Boolean) {
            ssidE.setText(if (ap) cfg.apSsid else cfg.staSsid)
            passE.setText(if (ap) cfg.apPass else cfg.staPass)
        }
        // いま見えている入力を、いまのモードの側へ控える(切り替えの直前に呼ぶ)。
        fun keepCurrent(ap: Boolean) {
            val sid = ssidE.text.toString(); val pw = passE.text.toString()
            if (ap) { cfg.apSsid = sid; cfg.apPass = pw } else { cfg.staSsid = sid; cfg.staPass = pw }
        }
        fillFor(edgeApMode)

        fun applyModeLabels(ap: Boolean) {
            ssidLabel.text = if (ap) "AP SSID (空なら端末の既定値)" else "接続先 SSID"
            passLabel.text = if (ap) "AP password (空なら端末の既定値)" else "接続先 password"
            // APモードでは周辺Wi-Fiから選ぶ意味が無い(自分で立てる側なので)。
            // 無効の見た目は背景と文字色(btn_blue_round / colors.xml)が受け持つ。
            //  ここで alpha を掛けると二重に薄くなり、書き込み画面のボタンと色が揃わない。
            ssidPickBtn.isEnabled = !ap
        }
        applyModeLabels(edgeApMode)
        apSwitch.setOnCheckedChangeListener { _, ap ->
            keepCurrent(!ap)          // 切り替える前のモードの側へ控える
            edgeApMode = ap
            applyModeLabels(ap)
            fillFor(ap)               // 新しいモードで覚えている値を出す
        }
        edgeApSwitch = apSwitch
        // 接続先が変わったら、パスワードはその SSID のものに入れ替える。
        //
        // 【必ず入れ替えること(2026-08-29 実機報告)】以前は「空のときだけ入れる」に
        //  していたため、AP→STA と戻して別の SSID を選ぶと**前の接続先のパスワードが
        //  そのまま残り**、気づかないまま送ってしまう。覚えていれば入れる、覚えて
        //  いなければ**空にする**。中途半端に残さないのが安全。
        fun applySsid(sid: String) {
            ssidE.setText(sid)
            val known = knownWifiPass(sid)
            passE.setText(known)          // 知らない先なら空になる
            if (known.isNotEmpty()) {
                Toast.makeText(ctx, "以前に使ったパスワードを入れました", Toast.LENGTH_SHORT).show()
            }
        }
        ssidPickBtn.setOnClickListener { pickWifiSsid { sid -> applySsid(sid) } }
        // 手で打った場合も同じ。ただし**打ち替えたときだけ**にする。触っていないのに
        //  入力を離れただけでパスワードが消えると、打ち直しを強いることになる。
        var ssidOnFocus = ""
        ssidE.setOnFocusChangeListener { _, has ->
            if (has) { ssidOnFocus = ssidE.text.toString(); return@setOnFocusChangeListener }
            if (edgeApMode) { return@setOnFocusChangeListener }      // AP の SSID は自分で決める側
            val now = ssidE.text.toString()
            if (now != ssidOnFocus) { passE.setText(knownWifiPass(now)) }
        }

        val popView = TextView(ctx).apply {
            setPadding(0, (12 * d).toInt(), 0, 0)
            text = if (scannedPop.isEmpty()) "PoP: 未取得 — QRをスキャンしてください" else "PoP: " + scannedPop
        }
        box.addView(popView); edgePopView = popView

        // QR と送信は左右に並べる(操作の順が左→右で読める)。
        val qrBtn   = blueButton("端末のQRをスキャン") { requestEdgeQr() }
        val sendBtn = blueButton("設定を送信") { }
        for (b in listOf(qrBtn, sendBtn)) {
            b.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply {
                setMargins(dp(2), dp(6), dp(2), 0)
            }
        }
        box.addView(LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            addView(qrBtn); addView(sendBtn)
        })
        sendBtn.apply {
            setOnClickListener {
                // 撮影中は AP/STA を切り替えさせない。切り替えるとエッジとカメラの回線が切れて
                // 撮影が壊れる(2026-08-14 指示)。エッジ側でも同じ判定で断るが、ここで止めれば
                // 理由をユーザーに見せられる。
                // 見るのは**送信先のその1台**だけ(2026-08-17 指示)。以前は全計画をまとめて見て
                // いたため、別のエッジが撮影しているだけで手を出せなくなっていた。
                // 送信先は編集中の登録名。新規(未選択)なら入力欄の名前=まだ撮影していない。
                val target = selectedEdgeName.ifEmpty { (edgeNameEt?.text?.toString() ?: "").trim() }
                if (isEdgeCaptureBusy(target)) {
                    Toast.makeText(ctx, "「" + target + "」は撮影中です。ネットワーク設定を変えるとカメラとの回線が切れます。撮影を止めてから行ってください",
                                   Toast.LENGTH_LONG).show()
                } else {
                    stashEdgeForm()   // 送った内容をその端末の設定として残す
                    sendEdgeProvision()
                }
            }
        }

        // 「登録だけする」は廃止(2026-09-04 UI依頼)。「＋ 新規エッジ端末」を押した時点で
        //  台帳に入るので、この操作でできる状態と同じものが既にできている。

        // ── ログ取得(この端末のぶん) ───────────────────────────────
        //
        // 【なぜ端末ごとか(2026-08-29 UI依頼)】困るのはたいてい1台だけで、全部の端末で
        //  記録を増やす理由がない。しかも StickS3 の保存領域は 1.5MB しかなく、
        //  撮影ログを入れると一晩(16時間・15秒周期)で約1.5MB、STACK/HEAP だけでも約320KB になる。
        //  必要な端末にだけ入れられるようにする。
        if (selectedEdgeName.isNotEmpty()) {
            box.addView(TextView(ctx).apply {
                text = "ログ取得"
                setTypeface(null, Typeface.BOLD); textSize = 15f
                setPadding(0, dp(16), 0, dp(2))
            })
            box.addView(TextView(ctx).apply {
                text = "次の撮影から効きます。量が多いので、普段は切っておくことをおすすめします。"
                textSize = 12f; setTextColor(Color.GRAY); setPadding(0, 0, 0, dp(2))
            })
            val lo = loadEdgeLogOpt(selectedEdgeName)
            fun logCb(label: String, on: Boolean, set: (Boolean) -> Unit): CheckBox {
                val cb = CheckBox(ctx)
                cb.text = label
                cb.isChecked = on
                // 【ここで無効にしないこと(2026-08-29 実機報告)】以前は isCaptureBusy()
                //  (=どこかの端末が撮影中)を見ていたため、**1台が撮影しているだけで
                //  全端末のログ設定が触れなくなっていた**。禁止の判定は画面全体で1か所
                //  (下の isEdgeCaptureBusy)に任せ、こちらは何もしない。
                cb.setOnCheckedChangeListener { _, v -> set(v); saveEdgeLogOpt(selectedEdgeName, lo) }
                box.addView(cb)
                return cb
            }
            logCb("撮影ログ (1コマごとの露出・測光)", lo.shot) { lo.shot = it }
            logCb("バッテリログ (電池残量の定期記録)", lo.batt) { lo.batt = it }
            logCb("STACK・HEAP (毎分の診断)",        lo.sys)  { lo.sys  = it }
        }

        // 【撮影中はこの画面を触らせない(2026-08-29 UI依頼)】ネットワーク設定を変えると
        //  カメラとの回線が切れて撮影が壊れる。ログの設定も次の撮影からしか効かない。
        //  以前は送信ボタンを押した後で断っていたが、押す前に分かる方がよい。
        //  対象は**この端末が撮影中のとき**だけ(他の端末の撮影は関係ない)。
        if (isEdgeCaptureBusy(selectedEdgeName)) {
            setViewTreeEnabled(box, false)
            box.addView(TextView(ctx).apply {
                text = "「" + selectedEdgeName + "」は撮影中です。撮影が終わってから設定してください。"
                textSize = 12f; setTextColor(Color.parseColor("#C62828")); setPadding(0, dp(10), 0, 0)
            })
        }
    }

    // 画面の中身をまとめて有効/無効にする(入れ子も辿る)。撮影中の入力禁止に使う。
    private fun setViewTreeEnabled(v: View, on: Boolean) {
        v.isEnabled = on
        if (v is ViewGroup) { for (i in 0 until v.childCount) { setViewTreeEnabled(v.getChildAt(i), on) } }
    }

    // BLEでエッジに start を送ってQRを表示させ、続けてカメラでスキャンする。
    private fun requestEdgeQr() {
        val ctx = this
        ensureBlePermissions {
            edgePopView?.text = "端末にQR表示を要求中(BLE)..."
            EdgeBle(ctx,
                log = { m -> runOnUiThread { edgePopView?.text = m } },
                result = { ok, m -> runOnUiThread {
                    if (ok) { edgePopView?.text = "QR表示OK。カメラでスキャンしてください"; scanEdgeQr() }
                    else { edgePopView?.text = m; Toast.makeText(ctx, "QR表示要求に失敗: " + m, Toast.LENGTH_LONG).show() }
                } }
            ).also { it.setTargetName(selectedEdgeName) }.startQr()
        }
    }

    // QRの読み取り。エッジが出すのはプロビジョニングQRだけである。
    //  {"n":端末名,"pop":合言葉,"m":sta|ap,"s":AP SSID,"p":AP password}
    //  PoP は設定送信の暗号鍵(SHA-256)になり、m/s/p は現在値として入力欄へ入る。
    //  ※ AP参加QR(標準 WIFI: 形式)は廃止した(2026-08-08)。スマホとエッジの接続は将来
    //    BLEへ移すためQRで参加させる必要が無く、カメラは元々QRを読めない。APのSSID/
    //    パスワードはエッジ本体の画面に文字で表示する。
    private fun scanEdgeQr() {
        val ctx = this
        val options = GmsBarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        GmsBarcodeScanning.getClient(ctx, options).startScan()
            .addOnSuccessListener { barcode ->
                val contents = barcode.rawValue ?: ""
                try {
                    val o = JSONObject(contents)
                    scannedPop = o.optString("pop", "")
                    scannedName = o.optString("n", "")
                    // QR に入っているエッジ側の名前は**採用しない**(2026-09-04)。名前は一覧の行が
                    //  持ち主で、送信すればこちらの名前がエッジへ入る。読んだ名前で上書きすると
                    //  一覧の表示と食い違う。違う名前にしたければ行で直す。
                    // エッジの現在値を入力欄へ入れる(2026-08-08 UI依頼)。読んだ値をそのまま送れば
                    //  変わらず、書き換えて送れば変えた値が設定される。旧版のQRには m/s/p が
                    //  無いので、その場合は今の入力を触らない(空文字で上書きしない)。
                    val curMode = o.optString("m", "")
                    if (curMode.isNotEmpty()) {
                        val ap = (curMode == "ap")
                        edgeApMode = ap
                        edgeApSwitch?.isChecked = ap   // リスナ経由でラベルも切り替わる
                    }
                    val apSsid = o.optString("s", "")
                    val apPass = o.optString("p", "")
                    if (curMode == "ap" && apSsid.isNotEmpty()) {
                        edgeSsidEt?.setText(apSsid); edgePassEt?.setText(apPass)
                    }
                    edgePopView?.text = if (curMode == "ap")
                        "PoP取得済。AP SSID: " + apSsid + "   password: " + apPass + " — 変更したい場合だけ書き換えて「設定を送信」"
                    else
                        "PoP: " + scannedPop + "(取得済)。内容を入力し「設定を送信」"
                    Toast.makeText(ctx, "QR読取 OK: " + scannedName + " / PoP=" + scannedPop, Toast.LENGTH_LONG).show()
                } catch (_: Exception) {
                    Toast.makeText(ctx, "QR内容が不正: " + contents, Toast.LENGTH_LONG).show()
                }
            }
            .addOnCanceledListener { Toast.makeText(ctx, "QRスキャンを中止しました", Toast.LENGTH_SHORT).show() }
            .addOnFailureListener { e -> Toast.makeText(ctx, "スキャン失敗: " + e.message, Toast.LENGTH_LONG).show() }
    }

    // 入力内容をエッジへ送る(PoP由来鍵で暗号化してBLE書き込み)。
    private fun sendEdgeProvision() {
        val ctx = this
        if (scannedPop.isEmpty()) { Toast.makeText(ctx, "先にQRスキャンでPoPを取得してください", Toast.LENGTH_SHORT).show(); return }
        val name = (edgeNameEt?.text?.toString() ?: "").trim()
        if (name.isEmpty() || !isAsciiEdgeName(name)) {
            Toast.makeText(ctx, "端末識別名は半角英数字で入力してください(外部端末で日本語は表示できません)", Toast.LENGTH_LONG).show(); return
        }
        val ssid = edgeSsidEt?.text?.toString() ?: ""
        val pass = edgePassEt?.text?.toString() ?: ""
        val mode = if (edgeApMode) "ap" else "sta"
        // STAのSSIDは「その端末へまだ接続先を送っていないとき」だけ必須。一度送ってあれば
        //  空=端末名だけ変更(エッジは接続先を保つ)。以前は「新規登録のとき」で見ていたが、
        //  登録が先に済むようになったので、保存済みの接続先の有無で見る(2026-09-04)。
        // APは空ならエッジ側が既定値(HGC-Edge-<MAC下2桁> / 8桁乱数)を用意する。
        if (mode == "sta" && ssid.isEmpty() && loadEdgeCfg(selectedEdgeName).staSsid.isEmpty()) {
            Toast.makeText(ctx, "SSIDを入力してください(新規登録は接続先が必要です)", Toast.LENGTH_LONG).show(); return
        }
        // 【APは長さを守らせる(2026-08-17)】Wi-FiのAPはパスワード8〜63文字・SSID1〜32文字でないと
        //  そもそも立ち上がらない。短いまま送るとエッジは再起動後にAPを出せず、画面も出ず
        //  カメラも繋がらない(BLE以外で到達できなくなる)。送る前にここで止めて理由を伝える。
        if (mode == "ap") {
            if (ssid.length > 32) {
                Toast.makeText(ctx, "APのSSIDは32文字以内にしてください", Toast.LENGTH_LONG).show(); return
            }
            if (pass.isNotEmpty() && pass.length < 8) {
                Toast.makeText(ctx, "APのパスワードは8文字以上にしてください(Wi-Fiの決まりです。空にすると端末が自動で作ります)", Toast.LENGTH_LONG).show(); return
            }
            if (pass.length > 63) {
                Toast.makeText(ctx, "APのパスワードは63文字以内にしてください", Toast.LENGTH_LONG).show(); return
            }
        }
        val json = JSONObject().put("name", name).put("ssid", ssid).put("pass", pass).put("mode", mode).toString()
        ensureBlePermissions {
            edgePopView?.text = "BLE送信中..."
            EdgeBle(ctx,
                log = { m -> runOnUiThread { edgePopView?.text = m } },
                result = { ok, m -> runOnUiThread {
                    Toast.makeText(ctx, m, Toast.LENGTH_LONG).show()
                    edgePopView?.text = m
                    if (ok) {
                        // 送信できた端末を登録へ反映する。既存なら改名に追従、新規なら追加。
                        if (selectedEdgeName.isEmpty()) {
                            if (edges.none { it.name == name }) edges.add(Edge(name, "", 50506))
                            saveRegisteredEdges(); refreshEdgeSpinner()
                        } else {
                            renameRegisteredEdge(selectedEdgeName, name)
                        }
                        selectedEdgeName = name
                        scannedPop = ""
                        buildEdgeList(); buildEdgeForm()
                    }
                } }
            ).provision(scannedPop, json)
        }
    }

    private fun saveRegisteredEdges() {
        val a = JSONArray()
        edges.forEach { a.put(JSONObject().apply { put("name", it.name); put("ip", it.ip); put("port", it.port) }) }
        hgcPrefs().edit().putString("regEdges", a.toString()).apply()
    }
    // 「端末」欄でエッジを使わない(スマホ直結)ときの表示(2026-09-02 UI依頼)。
    //  選択肢と一覧の副行で同じ文字を出すため、1か所に置いて両方から使う。
    private val kPhoneEdgeLabel = "スマホ"

    // 初回起動で作る撮影場所の名前(2026-09-04 UI依頼)。
    private val kCurrentPlaceName = "current location"

    // 計画ごとのエッジ選択(prefsに端末"名称"を保存。空=スマホ直結)。
    // IPはDHCPで変わり事前に不定のため、識別は端末名称で行い、IPは開始時に検索で解決する。
    private fun planEdgeName(planId: String) = if (planId.isEmpty()) "" else (hgcPrefs().getString("pe_$planId", "") ?: "")
    private fun setPlanEdgeName(planId: String, name: String) {
        if (planId.isEmpty()) return
        if (planEdgeName(planId) == name) return   // 変化なしは書かない(誤書き込みの影響を最小化・prefs churn抑制)
        hgcPrefs().edit().putString("pe_$planId", name).apply()
        // 一覧の副行に端末名を出すようになったので(2026-09-02)、変わったら作り直す。
        //  スピナーからの選択と、エッジ端末の改名に伴う付け替えの両方がここを通る。
        if (::planListContainer.isInitialized) refreshPlanList()
    }
    private fun planEdge(planId: String): Edge? {
        val name = planEdgeName(planId)
        return if (name.isEmpty()) null else (edges.firstOrNull { it.name == name } ?: Edge(name, "", 50506))
    }
    // 端末名称に一致するエッジをネットワーク検索して現在のIPを解決する(見つからなければ null)。
    // 項目9: IPは記憶せず、必ずブロードキャストで問い合わせて「端末名が合致した個体」だけを採用する。
    //  ・キャッシュ(last-seen)IPは絶対に流用しない。繋ぎ直しで同じIPを別端末が持つと誤認するため。
    //  ・混雑WiFiでの取りこぼし対策として、名前一致が出るまで数回リトライする(それでも駄目なら null)。
    private fun discoverEdgeByName(name: String): Edge? {
        repeat(3) {
            val js = HgeNative.nativeEdgeSearch(2500)
            try {
                val arr = JSONArray(js)
                for (i in 0 until arr.length()) {
                    val o = arr.getJSONObject(i)
                    if (o.optString("edgeName") == name) return Edge(name, o.optString("ip"), o.optInt("port", 50506))
                }
            } catch (_: Exception) {}
        }
        return null   // ブロードキャストで同名端末が見つからない=本当に居ない(キャッシュIPは流用しない)
    }
    // 登録一覧の last-seen IP を更新(stop/poll 用に開始時の解決結果を保持)。
    // 常時スイープからも30秒ごとに呼ばれるため、変化が無ければ prefs へ書かない。
    // 【登録済みのIP更新だけ】知らない名前が来ても**台帳に足さない**(2026-09-02)。
    //  以前はここで黙って新規登録していたため、検索応答に紛れ込んだ別種のJSON
    //  (撮影計画の進捗)の名前が、そのままエッジ端末として永久に残った。
    //  台帳へ足せるのは、検証済みの探索結果(registerDiscoveredEdge)と画面からの登録だけ。
    private fun updateEdgeIp(name: String, ip: String, port: Int) {
        val i = edges.indexOfFirst { it.name == name }
        if (i < 0) return                                        // 未登録は足さない
        if (ip.isEmpty()) return                                 // 接続先の分からない値で潰さない
        if (edges[i].ip == ip && edges[i].port == port) return   // 変化なし
        edges[i] = Edge(name, ip, port)
        saveRegisteredEdges()
    }

    // 探索で見つけたエッジを台帳へ入れる(未登録なら追加、既登録ならIP更新)。
    //  ここが**ネットワーク由来の唯一の登録口**なので、名前と接続先を確かめてから入れる。
    private fun registerDiscoveredEdge(name: String, ip: String, port: Int) {
        if (name.isEmpty() || !isAsciiEdgeName(name)) return   // エッジのLCDに出せない名前は端末名ではない
        if (ip.isEmpty()) return                               // 接続先が無いものは端末として登録しない
        if (edges.none { it.name == name }) { edges.add(Edge(name, ip, port)); saveRegisteredEdges() }
        else updateEdgeIp(name, ip, port)
    }

    // 実在しない計画に紐づくエッジ割当(pe_<計画id>)を落とす。計画を消しても残り続けるため。
    private fun pruneOrphanPlanEdges(liveIds: Set<String>) {
        if (liveIds.isEmpty()) return   // 計画一覧が取れていないときは触らない
        val p = hgcPrefs()
        val gone = p.all.keys.filter { it.startsWith("pe_") && !liveIds.contains(it.removePrefix("pe_")) }
        if (gone.isEmpty()) return
        p.edit().apply { gone.forEach { remove(it) } }.apply()
    }

    // 実際に保存されているエッジ割当を、スピナーの表示へ反映する。
    //  表示が実態からズレたまま残らないようにするためのもの。ズレているときだけ選択し直す
    //  (毎回 setSelection すると、その通知でまたここへ戻ってくる)。
    //  chosen を渡すと、ユーザーが選んだ値が保存されなかったときだけ知らせる。
    //  黙って選択が戻ると画面の不具合に見えるので、選び直せるように一言出す。
    private fun showStoredEdgeOnSpinner(planId: String, chosen: String? = null) {
        if (planId.isEmpty()) return
        val stored = planEdgeName(planId)
        val idx = if (stored.isEmpty()) 0
                  else edgeSpinnerEdges.indexOfFirst { it.name == stored }.let { if (it >= 0) it + 1 else 0 }
        if (edgeSpinner.selectedItemPosition != idx) { try { edgeSpinner.setSelection(idx) } catch (_: Exception) {} }
        if (chosen != null && chosen != stored) {
            Toast.makeText(this, "外部端末を変更できませんでした。もう一度選んでください", Toast.LENGTH_SHORT).show()
        }
    }

    private fun refreshEdgeSpinner() {
        val labels = mutableListOf(kPhoneEdgeLabel)
        // 【内蔵カメラのときは外部端末を出さない(2026-09-05)】内蔵カメラはその端末の中にしか
        //  無いので、外部端末を選べても必ず失敗する。選択肢から外して起き得ない組み合わせを消す。
        val builtinCam = try { planUsesBuiltinCamera() } catch (_: Exception) { false }
        // IPは動的なので名称のみ表示。常時スイープの生存状態を ●=オンライン/○=オフライン で付す(不明=無印)。
        edgeSpinnerEdges = if (builtinCam) emptyList() else sortedEdges()
        edgeSpinnerEdges.forEach {
            val mark = when (edgeOnline[it.name]) { true -> "● "; false -> "○ "; null -> "" }
            labels.add(mark + it.name)
        }
        val name = planEdgeName(currentPlanId)
        val idx = if (name.isEmpty()) 0 else edgeSpinnerEdges.indexOfFirst { it.name == name }.let { if (it >= 0) it + 1 else 0 }
        // このスピナーは currentPlanId の選択を表示している、と先に宣言する(保存先の固定)。
        //  ここで触っていない限り以降の選択通知は保存されないので、順序による漏れは起きない。
        edgeSpinnerPlanId = currentPlanId
        edgeSpinnerUserTouched = false   // 表示同期の前に、取りこぼしたユーザー操作フラグを落とす
        if (labels != edgeSpinnerLabels) {   // 表示が変わる時だけ作り直す(30秒スイープの度に通知を出さない)
            edgeSpinnerLabels = labels.toList()
            val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, labels)
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            edgeSpinner.adapter = adapter
        }
        if (edgeSpinner.selectedItemPosition != idx) { try { edgeSpinner.setSelection(idx) } catch (_: Exception) {} }
    }

    // 検索で見つかったエッジを登録一覧へ統合する(名称で重複判定、IPは最新へ更新)。手動登録分は残す。
    // エッジ端末名はエッジのLCD(英字フォントのみ表示)で出すため、印字可能なASCIIのみ許可する。
    // 日本語などの非ASCIIはエッジで表示できないため入力段階で弾く(貼り付けも除去)。
    private fun asciiEdgeNameFilter() = android.text.InputFilter { source, start, end, _, _, _ ->
        var changed = false
        val sb = StringBuilder()
        for (i in start until end) { val c = source[i]; if (c.code in 0x20..0x7E) sb.append(c) else changed = true }
        if (changed) sb.toString() else null   // null=変更なし(元の入力をそのまま許可)
    }
    private fun applyEdgeNameInput(et: EditText) {
        et.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        et.filters = arrayOf(asciiEdgeNameFilter(), android.text.InputFilter.LengthFilter(31))
    }
    private fun isAsciiEdgeName(s: String): Boolean = s.all { it.code in 0x20..0x7E }

    // 項目4: 周辺のWi-Fi(SSID)一覧から選ばせる。エッジをSTAにする際の接続先入力を楽にする。
    //  ・スキャン結果は位置情報権限が要る(ACCESS_FINE_LOCATION は取得済み: §7.9の現在地取得と共用)。
    //  ・取得できない/空のときは手入力を促す(SSIDは非表示APや権限拒否では列挙できないため)。
    private fun pickWifiSsid(onPick: (String) -> Unit) {
        val ctx = this
        val need = Manifest.permission.ACCESS_FINE_LOCATION
        if (ContextCompat.checkSelfPermission(this, need) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(need), 4321)
            Toast.makeText(ctx, "SSID一覧には位置情報の許可が必要です。許可後にもう一度お試しください", Toast.LENGTH_LONG).show()
            return
        }
        val wm = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
        val ssids = try {
            @Suppress("DEPRECATION")
            wm.scanResults.mapNotNull { r ->
                @Suppress("DEPRECATION") val s = r.SSID?.trim('"') ?: ""
                if (s.isEmpty()) null else s
            }.distinct().sorted()
        } catch (_: Exception) { emptyList() }
        // 現在接続中のSSIDは先頭に出す(たいていこれを選ぶため)。
        val cur = currentWifiSsid()
        val list = (listOfNotNull(cur) + ssids.filter { it != cur }).distinct()
        if (list.isEmpty()) { Toast.makeText(ctx, "SSIDを取得できませんでした。手入力してください", Toast.LENGTH_LONG).show(); return }
        AlertDialog.Builder(ctx)
            .setTitle("接続先SSIDを選択")
            .setItems(list.toTypedArray()) { _, i -> onPick(list[i]) }
            .setNegativeButton("やめる", null)
            .show()
    }

    // エッジ端末の登録/管理(設定)。名称で手動登録(IPは撮影開始時に自動検索)。オフラインでも登録でき計画で選べる。

    // エッジ端末の状態を約10秒ごとに問い合わせ、スマホ表示へ反映する(仕様8.2)。
    // エッジ側で停止された場合もここで検知してスマホUIを更新する(既知ギャップの解消)。
    // 撮影中/待機/未検出の「エッジ計画」を列挙する。計画ごとに別エッジを指定でき並行撮影も可能なので、
    // 表示中の1台ではなく“アクティブな全エッジ計画”を対象にする。phone直は planEdge==null で除外される。
    private fun activeEdgePlans(): List<String> =
        (capturingPlans + waitingPlans + disconnectedPlans + stoppingPlans).filter { planEdge(it) != null }
        // stoppingPlans も対象: 中止操作で集合からは即除去(即時アイコン反映)するが、停止(IDLE)の確定は
        // ポーリングで検知する必要があるため、確定までポールを続ける(reconcileEdgePlan がガード処理)。

    // 1つのエッジ計画の進捗JSONを状態集合へ反映する。表示中の計画(currentPlanId)ならステータス行も更新。
    // ST_IDLE/ST_ERROR ならその計画を各集合から除く(=撮影終了)。
    private fun reconcileEdgePlan(pid: String, pj: String) {
        if (pj.isEmpty()) return
        try {
            val o = JSONObject(pj)
            // ① エッジ書き戻し: エッジが接続確定したカメラの serial/assignedName を所持カメラへ反映する
            //  (エッジ撮影ではスマホがカメラに接続しないため、この経路が唯一の識別情報伝播)。serial単位で1回だけ適用。
            // 認証で弾かれているなら、その理由を覚えておく(「見つかりません」を言い換えるため)。
            //  【理由は遅れて届く】×とダイアログは取得に失敗した瞬間に出るが、理由が分かるのは
            //   その次の周回になる。届いた時点で内容が変わるなら、出 している案内を出し直す。
            //   そうしないと、多重表示の抑止が働いて古い文言("見つかりません")のまま残る。
            val nt = o.optInt("notice", 0)
            val prev = planAuthNotice[pid] ?: 0
            if (nt != 0) planAuthNotice[pid] = nt else planAuthNotice.remove(pid)
            if (nt != prev && nocamDialogShown.contains(pid) && disconnectedPlans.contains(pid))
            {
                clearNoCam(pid)            // いま出ている案内を閉じ、抑止も解く
                showNoCameraDialog(pid)    // 新しい理由で出し直す
            }
            val cSerial = o.optString("serial")
            if (cSerial.isNotEmpty() && edgeAppliedSerials.add(cSerial)) {
                val cModel = o.optString("model"); val cAssignedName = o.optString("assignedName")
                Thread { try { HgeNative.nativeRecordCameraIdentity(cModel, cSerial, cAssignedName, true) } catch (_: Exception) {} }.start()
            }
            val st = o.optInt("state")
            histOnState(pid, st)   // 項目9: エッジ側で起きたこと(開始/終了/カメラ断/復帰)を履歴に残す
            var changed = false
            // 開始操作中(開始要求が未達の可能性)は IDLE 報告を無視する。開始要求の結果確定(startOnEdge)後に
            // 通常のポーリング反映へ戻る。これが無いと2台順次開始(直列化)の2台目が「まだIDLE」を拾われて
            // 集合から外れ、ポーリング対象からも消えて“エッジ撮影中なのに開始前アイコン”のまま固まる。
            if (startingPlans.contains(pid)) return
            // 「中止」操作済みの計画は停止(IDLE/ERROR)確定まで NOCAMERA を無視(ダイアログ無限再表示を防ぐ)。
            if (stoppingPlans.contains(pid)) {
                if (st == HgeNative.ST_IDLE || st == HgeNative.ST_ERROR) {
                    stoppingPlans.remove(pid)
                    capturingPlans.remove(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
                    if (capturingPlans.isEmpty()) stopBlink()
                    if (currentPlanId == pid) captureStatus.text = ""
                    refreshPlanList(); updateReadOnly()
                    if (activeEdgePlans().isEmpty()) handler.removeCallbacks(edgePoll)
                }
                return
            }
            when (st) {
                HgeNative.ST_NOCAMERA, HgeNative.ST_DISCONNECTED -> {
                    if (disconnectedPlans.add(pid)) changed = true
                    if (capturingPlans.remove(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    refreshCaptureStatusForCurrent()
                    showNoCameraDialog(pid)   // Phase3c: 継続/中止ポップアップ(多重抑止あり)
                }
                HgeNative.ST_WAITING, HgeNative.ST_SEARCHING -> {
                    if (waitingPlans.add(pid)) changed = true
                    if (capturingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)   // 未検出→待機へ復帰: 表示中のNOCAMERAダイアログを閉じる
                    refreshCaptureStatusForCurrent()
                }
                HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                    if (capturingPlans.add(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)   // 未検出→撮影へ復帰: 表示中のNOCAMERAダイアログを閉じる
                    rememberProgress(pid, o)
                    refreshCaptureStatusForCurrent()
                }
                HgeNative.ST_IDLE, HgeNative.ST_ERROR -> {   // この計画は終了 → 各集合から除去
                    if (capturingPlans.remove(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)
                    planProgress.remove(pid)   // 終わった計画の枚数は捨てる(次に選んだとき古い値を出さない)
                    refreshCaptureStatusForCurrent()
                }
                // その他(READY 等)は無視
            }
            if (capturingPlans.isEmpty()) stopBlink() else startBlink()
            if (changed) { refreshPlanList(); updateReadOnly() }
        } catch (_: Exception) {}
    }

    // エッジ側で停止された場合もここで検知してスマホUIを更新する(既知ギャップの解消)。
    // 表示中の計画に依らず、アクティブな各エッジ計画をそれぞれ自分のエッジでポールする(並行対応)。
    private val edgePoll = object : Runnable {
        override fun run() {
            val active = activeEdgePlans()
            if (active.isEmpty()) { return }   // アクティブなエッジ計画が無ければ停止(次の start/restore で再開)
            for (pid in active) {
                val e = planEdge(pid) ?: continue
                if (!e.reachable()) continue   // 話しかけられない(Wi-Fi:IP未解決 / BLE:名前なし)はスキップ
                Thread {
                    val pj = HgeNative.nativeEdgeProgress(e.addr(), e.port, pid)   // 計画別: この計画idの状態だけを取る(1エッジ複数カメラの誤検出防止)
                    runOnUiThread { reconcileEdgePlan(pid, pj) }
                }.start()
            }
            handler.postDelayed(this, 10000)   // 約10秒間隔(アクティブが続く限り再スケジュール)
        }
    }

    // edgePoll を単一インスタンスで(再)起動する。多重 postDelayed による二重ループを防ぐ。
    private fun ensureEdgePoll() { handler.removeCallbacks(edgePoll); handler.postDelayed(edgePoll, 2000) }

    // 遅延アームのポンプ(§7.4)。スマホ直接撮影の予約(将来窓)計画は開始スレッドを期日(窓90秒前)まで
    // 作らない(エンティティ側 hge_pump)。エッジはエッジ自身の loop が毎秒ポンプするため対象外。
    private val hgePump = object : Runnable {
        override fun run() {
            Thread { try { HgeNative.nativePump() } catch (_: Exception) {} }.start()
            handler.postDelayed(this, 5000)
        }
    }

    // ── APモードのエッジAPへのネットワークバインド(§1.2.1) ──
    // Android は「インターネットゲートウェイの無いWi-Fi(=エッジのSoftAP)」を数十秒で自動的に
    // 見限り、母艦LAN(モバイル/別Wi-Fi)へ切り替える。そのままではスマホがエッジと別網になり、
    // ETP(TCP/UDP)が届かず制御・監視ができない。そこで、現在スマホが接続中のWi-Fiが "HGC-Edge*"
    // (エッジのSoftAP)のときは、そのNetworkを requestNetwork で確保し bindProcessToNetwork で
    // プロセス全体の通信(ネイティブのソケットも含む)をそのNICへ固定する。これで自動離脱を防ぎ、
    // インターネット判定に依らずエッジと通信できる。別SSID(母艦LAN)に居る間はバインドしない=従来動作。
    private val cm by lazy { getSystemService(CONNECTIVITY_SERVICE) as ConnectivityManager }
    private var edgeApNetwork: Network? = null
    private var edgeApCallback: ConnectivityManager.NetworkCallback? = null

    // 現在接続中のWi-Fi SSID(引用符除去)。取得不能は null。
    private fun currentWifiSsid(): String? {
        return try {
            val wm = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION") val raw = wm.connectionInfo?.ssid ?: return null
            raw.trim('"').let { if (it.isEmpty() || it == "<unknown ssid>") null else it }
        } catch (_: Exception) { null }
    }

    // SSID が "HGC-Edge" 始まり(=エッジSoftAP)ならバインドを起動、そうでなければ解除する。
    // edgeSweep(30秒毎)から呼ぶ。冪等(多重 requestNetwork を防ぐ)。
    private fun updateEdgeApBinding() {
        val ssid = currentWifiSsid()
        val onEdgeAp = ssid != null && ssid.startsWith("HGC-Edge")
        if (onEdgeAp) {
            if (edgeApCallback != null) return   // 既にバインド機構が稼働中
            val req = NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)   // インターネット無しWi-Fiも対象に
                .build()
            val cb = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    // このNetworkが本当にエッジSoftAPか(SSID一致)を確認してからバインドする(誤バインド防止)。
                    val nc = cm.getNetworkCapabilities(network)
                    val wi = nc?.transportInfo as? WifiInfo
                    val ns = wi?.ssid?.trim('"')
                    if (ns == null || ns.startsWith("HGC-Edge")) {
                        edgeApNetwork = network
                        cm.bindProcessToNetwork(network)
                        android.util.Log.i("EdgeApBind", "bound to $ns")
                    }
                }
                override fun onLost(network: Network) {
                    if (network == edgeApNetwork) { cm.bindProcessToNetwork(null); edgeApNetwork = null }
                }
            }
            edgeApCallback = cb
            try { cm.requestNetwork(req, cb) } catch (_: Exception) { edgeApCallback = null }
        } else {
            stopEdgeApBinding()
        }
    }

    private fun stopEdgeApBinding() {
        edgeApCallback?.let { try { cm.unregisterNetworkCallback(it) } catch (_: Exception) {} }
        edgeApCallback = null
        if (edgeApNetwork != null) { try { cm.bindProcessToNetwork(null) } catch (_: Exception) {}; edgeApNetwork = null }
    }

    // ── エッジ常時スイープ(§6.2.1 sessions) ──
    // 撮影の有無に関わらず約30秒ごとにUDP検索(C_SEARCH)をブロードキャストし、全エッジの
    //  ・生存(オンライン/オフライン)とIP変動(DHCP)の追従
    //  ・実行中セッション一覧(応答の sessions[])によるエッジ側ローカル開始/自動再開/停止の検出
    // を行う。後から電源を入れたエッジも次のスイープで自動的に現れる。撮影中の進捗詳細は従来どおり
    // edgePoll(10秒・計画別C_PROGRESS)が担い、スイープは「集合の同期と発見」を担当する役割分担。
    // UDPは混雑WiFiで取りこぼすため、連続2回無応答の登録エッジはlast-seen IPへTCP生存確認してから
    // オフライン判定する(1回の取りこぼしで表示を揺らさない)。
    // エッジごとに最後に時刻を送った時点。スイープのたびに送ると TCP の張り直しが増えるので、
    //  「見つかった瞬間」と「しばらく経ったとき」だけにする(エッジ側も差が小さければ何もしない)。
    private val edgeTimeSentAt = HashMap<String, Long>()
    private val kEdgeTimeRefreshMs = 30L * 60L * 1000L   // 30分

    // 見つけたエッジへ現在時刻とタイムゾーンを送る。
    //
    // 【なぜスイープで送るか(2026-08-28)】従来は撮影計画を送るときと、選択中のエッジへだけ
    //  送っていた。そのため電源を入れただけのエッジは時計を持たないままになる。時計が無いと
    //   ・撮影窓の判定ができない
    //   ・カメラ認証の nc の種が 0 になり、他の端末が一度触ると追いつけなくなる(実測)
    //  スイープは登録済みエッジを常時見つけているので、そこに乗せるのが素直。
    // 【送る値はその場で作る(2026-09-03)】瞬間は UTC のエポック秒、表示用オフセットは
    //  そのとき の TimeZone から取り直す。書式器に持たせると生成時のTZを抱えたままになり、
    //  現地でTZを変えた直後に食い違ってエッジの時計がずれる。
    private fun nowUtcSec(): Long = System.currentTimeMillis() / 1000L
    private fun nowOffMin(): Int = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000

    // 【エッジの時計のずれを知らせる(2026-09-03)】エッジは撮影待機中・撮影中は時計もタイムゾーンも
    //  受け付けない(走っている最中に時刻が飛ぶとコマも窓の判定も壊れるため)。そのため一度ずれると
    //  計画を止めるまで直らず、**気づかないまま一晩ずれたまま撮る**ことが起きた(2026-09-02 実害)。
    //  検索応答に載るエッジの時計と突き合わせ、ずれていたら知らせる。**開始は止めない**(2026-09-03 指示)。
    //  同じ端末で何度も出さない。直れば忘れて、また出せるようにする。
    private val edgeClockWarned = HashSet<String>()
    private val kClockSkewSec = 60L        // これを超えたら「時計がずれている」とみなす

    private fun checkEdgeClock(name: String, edgeUtc: Long, edgeTzOff: Int) {
        if (edgeUtc <= 0L) { return }      // 時計未設定または旧FW → 判定しない
        val skew = Math.abs(nowUtcSec() - edgeUtc)
        val tzNg = (edgeTzOff != nowOffMin())
        if (skew <= kClockSkewSec && !tzNg) { edgeClockWarned.remove(name); return }   // 直った
        if (!edgeClockWarned.add(name)) { return }                                     // 既に知らせた
        val sb = StringBuilder("外部端末「").append(name).append("」の時計がずれています")
        if (skew > kClockSkewSec) { sb.append("(").append(skew / 60).append("分").append(skew % 60).append("秒)") }
        if (tzNg) { sb.append("(タイムゾーンが違います)") }
        sb.append("。撮影中・待機中は直せません。計画を止めると次の同期で直ります")
        Toast.makeText(this, sb.toString(), Toast.LENGTH_LONG).show()
    }

    private fun sendEdgeTime(ed: Edge) {
        if (ed.ip.isEmpty()) return
        edgeTimeSentAt[ed.name] = System.currentTimeMillis()
        val u = nowUtcSec(); val off = nowOffMin()
        Thread { try { HgeNative.nativeEdgeSyncTime(ed.addr(), ed.port, u, off) } catch (_: Exception) {} }.start()
    }

    // エッジへ渡した台帳の指紋(エッジ名 → 中身のハッシュ)。同じ物を送り直さないため。
    private val edgeBookSent = HashMap<String, Int>()
    // 最後に渡した時刻(エッジ名 → ms)。同じ内容の送り直しに間隔をあけるため。
    private val edgeBookAt = HashMap<String, Long>()
    // 同じ内容を送り直す最短の間隔。エッジを入れ替えた/初期化した場合の取り返しは要るが、
    //  毎スイープ送る必要はない。時刻の送り直し(30分)と同じ考え方。
    private val kEdgeBookRefreshMs = 30L * 60L * 1000L

    // 所持カメラ台帳をエッジへ渡す。
    //  【なぜ計画と別に要るか】エッジは撮影計画を受け取るまでカメラの資格情報を持てないが、
    //   カメラへの挨拶(初回Wi-Fi参加を成立させる200)は計画を作る前に要る。
    //  【全量で置き換わる】送った内容がその時点の全量なので、カメラを消した・作り直した・
    //   パスワードを変えた、がこの1本で伝わる。消すための操作は別に要らない。
    private fun sendEdgeCameraBook(ed: Edge) {
        if (ed.ip.isEmpty()) return
        val book = try { HgeNative.nativeCameraBookJson() } catch (_: Exception) { return }
        if (book.isEmpty()) return
        Thread {
            try {
                if (HgeNative.nativeEdgeSendCameraBook(ed.addr(), ed.port, book) == 0) {
                    // 覚えるのは**中身**の指紋。台帳 JSON は暗号文の nonce で毎回変わるので、
                    //  それを覚えると「毎回変わった」ことになり 30 秒ごとに送り直してしまう。
                    edgeBookSent[ed.name] = (try { HgeNative.nativeCameraBookSig() } catch (_: Exception) { "" }).hashCode()
                    edgeBookAt[ed.name]   = System.currentTimeMillis()
                }
            } catch (_: Exception) {}
        }.start()
    }

    // 所持カメラをいじったら、知っているエッジ全部へ渡し直す(追加・変更・削除のいずれでも)。
    //  次のスイープでも指紋が変わっていれば送るので、ここで届かなくても取り返せる。
    private fun pushCameraBookToEdges() {
        for (ed in edges) { if (ed.ip.isNotEmpty()) sendEdgeCameraBook(ed) }
    }

    private val edgeSweep = object : Runnable {
        override fun run() {
            updateEdgeApBinding()   // エッジSoftAP接続中はそのNICへバインド維持(Androidの自動離脱を防ぐ)
            Thread {
                val js = try { HgeNative.nativeEdgeSearch(2000) } catch (_: Exception) { "[]" }
                // name → (edge, sessionsフィールド有無, sessions{planId→state}, heldPlansフィールド有無, 保有ロスター)
                //  utc/tzOff はエッジ自身の時計(新FWのみ)。0=未設定または旧FW→ずれの判定はしない。
                data class Found(val edge: Edge, val hasSessions: Boolean, val sessions: Map<String, Int>,
                                 val hasHeld: Boolean, val heldPlans: Set<String>, val reports: Int,
                                 val utc: Long, val tzOff: Int)
                val found = HashMap<String, Found>()
                try {
                    val arr = JSONArray(js)
                    for (i in 0 until arr.length()) {
                        val o = arr.optJSONObject(i) ?: continue
                        val nm = o.optString("edgeName"); if (nm.isEmpty()) continue
                        val sess = HashMap<String, Int>()
                        val has = o.has("sessions")   // 旧FWのエッジは sessions を返さない→セッション同期はしない
                        if (has) {
                            val sa = o.optJSONArray("sessions") ?: JSONArray()
                            for (k in 0 until sa.length()) {
                                val so = sa.optJSONObject(k) ?: continue
                                val id = so.optString("id"); if (id.isNotEmpty()) sess[id] = so.optInt("state")
                            }
                        }
                        // 項目6: 保有計画ロスター(新FW)。エッジが持つ全計画id。無い=旧FW→ロック解除同期はしない。
                        val held = HashSet<String>()
                        val hasHeld = o.has("heldPlans")
                        if (hasHeld) {
                            val ha = o.optJSONArray("heldPlans") ?: JSONArray()
                            for (k in 0 until ha.length()) { ha.optString(k)?.takeIf { it.isNotEmpty() }?.let { held.add(it) } }
                        }
                        // 溜まっている撮影レポートの件数(新FWのみ)。>0 のときだけ引き取りに行く。
                        found[nm] = Found(Edge(nm, o.optString("ip"), o.optInt("port", 50506)), has, sess, hasHeld, held,
                                          o.optInt("reports", 0), o.optLong("utc", 0L), o.optInt("tzOff", 0))
                    }
                } catch (_: Exception) {}
                // UDP無応答の登録エッジ: 連続2回でTCP生存確認(取りこぼし救済)→それも不応答ならオフライン。
                val tcpOnline = ArrayList<String>(); val offline = ArrayList<String>()
                for (ed in edges.toList()) {
                    if (found.containsKey(ed.name)) continue
                    val miss = (edgeMiss[ed.name] ?: 0) + 1
                    edgeMiss[ed.name] = miss
                    if (miss < 2) continue
                    val alive = ed.reachable() &&
                        (try { HgeNative.nativeEdgeProgress(ed.addr(), ed.port, "") } catch (_: Exception) { "" }).isNotEmpty()
                    if (alive) tcpOnline.add(ed.name) else offline.add(ed.name)
                }
                runOnUiThread {
                    var uiDirty = false
                    for ((nm, f) in found) {
                        edgeMiss[nm] = 0
                        // 居なかったものが見えた瞬間に時刻を送る(電源を入れた直後がこれ)。
                        //  以後は間を空けて送り直すだけ。ずれていなければエッジ側が何もしない。
                        val appeared = edgeOnline[nm] != true
                        if (edgeOnline[nm] != true) { edgeOnline[nm] = true; uiDirty = true }
                        val lastSent = edgeTimeSentAt[nm] ?: 0L
                        if (appeared || System.currentTimeMillis() - lastSent > kEdgeTimeRefreshMs) {
                            sendEdgeTime(f.edge)
                        }
                        // 台帳は中身が変わったときだけ送る(4台で571バイト・1往復0.1秒)。
                        //  【送りすぎないこと(2026-08-28 実機で StickS3 がリセット)】以前は
                        //   「見えた瞬間」でも無条件に送っていた。ところがAPモードのエッジでは
                        //   オンライン判定が毎回立ち直るため、実質**スイープのたび(30秒おき)**に
                        //   送っていた。受け取る側は計画を読み直すので内部ヒープを食い潰す。
                        //   見えた瞬間の再送は残すが、同じ内容なら間隔をあける。
                        val bookNow = try { HgeNative.nativeCameraBookSig().hashCode() } catch (_: Exception) { 0 }
                        val bookAge = System.currentTimeMillis() - (edgeBookAt[nm] ?: 0L)
                        if (edgeBookSent[nm] != bookNow || (appeared && bookAge > kEdgeBookRefreshMs)) {
                            sendEdgeCameraBook(f.edge)
                        }
                        // デバッグログの取捨。エッジは不揮発へ残さない(電源を入れ直すと
                        //  既定=採らない に戻る)ので、毎回送って揃え直す。数十バイト。
                        //  設定は**その端末のもの**を送る(端末ごとに違ってよい)。
                        sendEdgeLogOpt(nm)
                        checkEdgeClock(nm, f.utc, f.tzOff)   // 時計のずれを知らせる(止めはしない)
                        registerDiscoveredEdge(nm, f.edge.ip, f.edge.port)   // 未登録なら登録・既登録はIP追従
                        if (f.hasSessions) reconcileEdgeSessions(f.edge, f.sessions)
                        if (f.hasHeld) reconcileEdgeRoster(f.edge, f.heldPlans)   // 項目6: エッジ側削除の検知→ロック解除
                    }
                    for (nm in tcpOnline) { edgeMiss[nm] = 0; if (edgeOnline[nm] != true) { edgeOnline[nm] = true; uiDirty = true } }
                    for (nm in offline)   { if (edgeOnline[nm] != false) { edgeOnline[nm] = false; uiDirty = true } }
                    if (uiDirty) refreshEdgeSpinner()
                }
                // エッジに溜まった撮影レポートを引き取る。件数が入っているときだけ通信するので、
                // 定常(レポート0件)ではこのスイープの通信量は従来と変わらない。
                for (f in found.values) { if (f.reports > 0 && f.edge.ip.isNotEmpty()) collectEdgeReports(f.edge) }
            }.start()
            handler.postDelayed(this, 30000)   // 30秒ごと(常時)
        }
    }

    // 項目6: エッジが応答したロスター(保有計画id)を、そのエッジの保有集合として記録する。UIスレッドから呼ぶ。
    //  ・ロック判定(isPlanOnEdge)はこの保有集合を見る。エッジがある計画を保有=編集/削除不可。
    //  ・エッジ側で計画を削除すると次スイープのロスターから消える→保有集合から外れ→自動でロック解除。
    //  ・応答したエッジの分だけ更新(オフライン/未応答のエッジ分は前回値を保持=取りこぼしでロックを揺らさない)。
    //  ・スマホのローカルに無い計画id(=孤児)は保有集合に入れない(表示できないため)。
    // 保有台帳を prefs へ残す/読み戻す。アプリを終了してもロックを維持するため。
    private fun saveEdgeHeld() {
        try {
            val o = JSONObject()
            for ((k, v) in edgeHeldByEdge) { o.put(k, JSONArray(v.toList())) }
            getSharedPreferences("hgc", MODE_PRIVATE).edit().putString("edgeHeld", o.toString()).apply()
        } catch (_: Exception) {}
    }

    private fun loadEdgeHeld() {
        try {
            val t = getSharedPreferences("hgc", MODE_PRIVATE).getString("edgeHeld", "") ?: ""
            if (t.isEmpty()) return
            val o = JSONObject(t)
            for (k in o.keys()) {
                val a = o.optJSONArray(k) ?: continue
                val set = HashSet<String>()
                for (i in 0 until a.length()) a.optString(i).takeIf { it.isNotEmpty() }?.let { set.add(it) }
                if (set.isNotEmpty()) edgeHeldByEdge[k] = set
            }
        } catch (_: Exception) {}
    }

    private fun reconcileEdgeRoster(ed: Edge, heldPlans: Set<String>) {
        val localIds = try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            (0 until pa.length()).mapNotNull { pa.optJSONObject(it)?.optString("id")?.ifEmpty { null } }.toHashSet()
        } catch (_: Exception) { HashSet<String>() }
        val newHeld = heldPlans.filterTo(HashSet()) { localIds.contains(it) }
        if (edgeHeldByEdge[ed.name] != newHeld) {
            edgeHeldByEdge[ed.name] = newHeld
            saveEdgeHeld()                        // アプリを終了してもロックを保つ
            refreshPlanList(); updateReadOnly()   // 保有の増減(=ロック状態)を表示へ反映
        }
    }

    // スイープ結果(エッジの実行中セッション一覧)をスマホの表示集合へ同期する。UIスレッドから呼ぶこと。
    //  ・採用: エッジで走行中なのにスマホが追跡していない計画(エッジ側ローカル開始/自動再開)を集合へ入れ、
    //          エッジ担当(planEdgeName)も設定 → 計画リストにアイコンが点く。進捗詳細は以降 edgePoll が反映。
    //  ・除去: このエッジ担当なのにセッションに無い(=エッジ側で停止/終了済み)計画を集合から外す。
    //  開始/停止操作の過渡(startingPlans/stoppingPlans)中の計画には触れない(操作経路との競合防止)。
    private fun reconcileEdgeSessions(ed: Edge, sessions: Map<String, Int>) {
        val localIds = HashSet<String>()
        try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            for (k in 0 until pa.length()) { val id = pa.optJSONObject(k)?.optString("id") ?: ""; if (id.isNotEmpty()) localIds.add(id) }
        } catch (_: Exception) {}
        var changed = false
        for ((pid, st) in sessions) {
            if (!localIds.contains(pid)) continue   // スマホに無い計画(削除済み等)は表示できない
            if (startingPlans.contains(pid) || stoppingPlans.contains(pid)) continue
            val wasTracked = capturingPlans.contains(pid) || waitingPlans.contains(pid) || disconnectedPlans.contains(pid)
            when (st) {
                HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                    if (capturingPlans.add(pid)) changed = true
                    waitingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
                }
                HgeNative.ST_WAITING, HgeNative.ST_SEARCHING -> {
                    if (waitingPlans.add(pid)) changed = true
                    capturingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
                }
                HgeNative.ST_NOCAMERA, HgeNative.ST_DISCONNECTED -> {
                    if (disconnectedPlans.add(pid)) changed = true
                    capturingPlans.remove(pid); waitingPlans.remove(pid)
                }
                else -> {}   // IDLE/ERROR等は下の除去側で扱う
            }
            if (!wasTracked && (capturingPlans.contains(pid) || waitingPlans.contains(pid) || disconnectedPlans.contains(pid))) {
                setPlanEdgeName(pid, ed.name)   // 採用: 以降の停止/ポーリングにこのエッジを使う
            }
        }
        // 除去: このエッジ担当で追跡中なのに、エッジのセッション一覧に無い(=停止/終了済み)計画。
        // ただし一覧が上限(エッジ側 kMaxSessions=16)まで埋まっている場合は、載り切らなかっただけの
        // 実行中セッションを停止済みと誤認し得るため除去を保留する(採用側のみ反映)。
        if (sessions.size >= 16) { if (changed) { if (capturingPlans.isEmpty()) stopBlink() else startBlink(); refreshPlanList(); updateReadOnly(); if (activeEdgePlans().isNotEmpty()) ensureEdgePoll() }; return }
        for (pid in (capturingPlans + waitingPlans + disconnectedPlans).toList()) {
            if (sessions.containsKey(pid)) continue
            if (planEdgeName(pid) != ed.name) continue
            if (startingPlans.contains(pid) || stoppingPlans.contains(pid)) continue
            capturingPlans.remove(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid); clearNoCam(pid)
            changed = true
        }
        if (changed) {
            if (capturingPlans.isEmpty()) stopBlink() else startBlink()
            refreshPlanList(); updateReadOnly()
            if (activeEdgePlans().isNotEmpty()) ensureEdgePoll()   // 採用した計画の進捗詳細を10秒ポールで追従
        }
    }

    // 能動的な時刻同期。エッジは電波の悪い所ではNTP不可、かつStickS3はRTC無しで再起動時に時計を失う。
    // スマホが「選択中のエッジ」へ定期的に現在時刻(C_TIME)を送って時計を保つ(撮影開始と無関係に常時)。
    // 同期後はエッジ内部タイマが時計を進めるので、KEY1ローカル開始や無人撮影でも撮影窓を正しく判定できる。
    // 選択エッジがオフライン/IP未解決なら送信は失敗し無視する。
    private val edgeTimeSync = object : Runnable {
        override fun run() {
            val e = selectedEdge()
            if (e != null && e.ip.isNotEmpty()) {
                val u = nowUtcSec(); val off = nowOffMin()
                Thread { try { HgeNative.nativeEdgeSyncTime(e.addr(), e.port, u, off) } catch (_: Exception) {} }.start()
            }
            handler.postDelayed(this, 30000)   // 30秒ごと
        }
    }

    // 計画名をモノクロ2値ビットマップ(width u16LE, height u16LE, 1bpp MSB先頭, 1=白)に変換する。
    // エッジ端末はフォントに依存せず名称を表示できる(§8.2.1 多言語対応)。
    private fun makeNameBitmapBytes(name: String): ByteArray {
        // エッジの計画リスト行に収まるサイズ(高さ約26px)。名前はスマホのフォントで描いて2値化し送る(多言語対応)。
        val paint = android.graphics.Paint().apply {
            textSize = 20f; color = Color.WHITE; isAntiAlias = false; typeface = Typeface.DEFAULT_BOLD
        }
        val w = Math.ceil(paint.measureText(name).toDouble()).toInt().coerceIn(1, 260)
        val fm = paint.fontMetrics
        val h = Math.ceil((fm.bottom - fm.top).toDouble()).toInt().coerceIn(1, 100)
        val bmp = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
        val cv = android.graphics.Canvas(bmp); cv.drawColor(Color.BLACK)
        cv.drawText(name, 0f, -fm.top, paint)
        val bpr = (w + 7) / 8
        val out = java.io.ByteArrayOutputStream()
        out.write(w and 0xFF); out.write((w shr 8) and 0xFF)
        out.write(h and 0xFF); out.write((h shr 8) and 0xFF)
        val px = IntArray(w); val row = ByteArray(bpr)
        for (y in 0 until h) {
            java.util.Arrays.fill(row, 0)
            bmp.getPixels(px, 0, w, 0, y, w, 1)
            for (x in 0 until w) {
                val p = px[x]; val lum = ((p shr 16 and 0xFF) + (p shr 8 and 0xFF) + (p and 0xFF)) / 3
                if (lum > 128) row[x / 8] = (row[x / 8].toInt() or (0x80 shr (x and 7))).toByte()
            }
            out.write(row)
        }
        bmp.recycle()
        return out.toByteArray()
    }

    // 計画idから計画名を同期的に引く(計画一覧はid+名前を持つ)。エッジへ送る名前ビットマップ用。
    // latestSchedule(選択中計画のキャッシュ)は選択後に非同期イベントで遅れて更新されるため、
    // 2計画の順次開始で別計画の名前を作る競合があった(エッジの計画名取り違えの原因)。idから直接引いて根絶する。
    private fun planNameFor(id: String): String {
        try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            for (k in 0 until pa.length()) {
                val po = pa.optJSONObject(k) ?: continue
                if (po.optString("id") == id) return po.optString("planName")
            }
        } catch (_: Exception) {}
        return ""
    }

    // エッジへ計画を送って撮影開始する。ブロッキングI/Oを含むため edgeExec(専用バックグラウンド)から呼ぶこと。
    // 項目5: 送る計画JSON(planJson)は呼び出し側が planId から取得して渡す。選択中の計画(グローバル状態)に
    //  依存しないので planExec を占有せず、送信中も計画選択が詰まらない。
    private fun startOnEdge(e: Edge, planId: String, planJson: String) {
        // 時刻同期(C_TIME)はエッジ端末の時計を「現在時刻」に合わせるためのもの。現在時刻を送る。
        // 瞬間は UTC の整数、表示用オフセットはその場で取り直す(2026-09-03。nowUtcSec の説明を参照)。
        val utcSec = nowUtcSec()
        val off = nowOffMin()
        val name = planNameFor(planId)   // 対象planIdの名前を同期取得(非同期キャッシュ latestSchedule は使わない)
        val nameBmp = makeNameBitmapBytes(if (name.isEmpty()) "撮影計画" else name)
        run {
            // 【2026-08-06 廃止】開始前のカメラIP通知はやめた(理由は pushPresenceToActiveEdges の跡地を参照)。
            //  エッジは自分でカメラを見つけるので、スマホが見ているIPを渡す必要がない。
            //  探索に1秒余分にかかっても、間違ったネットワークのIPを渡すより良い。
            val r = HgeNative.nativeEdgeStart(e.addr(), e.port, utcSec, off, nameBmp, planId, planJson)
            // 開始が失敗コードを返しても、エッジ側では実際に走っている場合がある:
            //  ・既に同じ計画がエッジで走行中 → C_ACTION が INVALID_STATE で NAK(-4)
            //  ・2台順次開始のETP競合や ACK 取りこぼしで NAK/タイムアウト
            // このとき従来は「開始失敗」として待機集合から外し“開始前アイコン”に戻していた(=エッジ撮影中
            // なのにスマホは開始前)。失敗時はこの計画の実状態を問い合わせ、走っていれば開始成功として扱う。
            // ネットワークI/Oはこのスレッド上で行う(UIを固めない)。
            val running = if (r == 0) true else {
                val pj = try { HgeNative.nativeEdgeProgress(e.addr(), e.port, planId) } catch (_: Exception) { "" }
                if (pj.isEmpty()) true   // エッジ無応答=判定不能 → 除去せず edgePoll に委ねる(誤って開始前へ戻さない)
                else {
                    val st = try { JSONObject(pj).optInt("state", HgeNative.ST_IDLE) } catch (_: Exception) { HgeNative.ST_IDLE }
                    st != HgeNative.ST_IDLE && st != HgeNative.ST_ERROR   // 明示的IDLE/ERROR以外は走っているとみなし維持
                }
            }
            if (running) addHistory("send plan", planId)   // 項目9: 撮影計画をエッジへ送った
            runOnUiThread {
                startingPlans.remove(planId)   // 開始要求の結果確定(以降はポーリングが実状態を反映)
                if (running) {
                    // 項目2(再修正3): 送った時点でこのエッジが保有していると台帳へ即記録する。
                    //  従来は30秒周期のスイープ(reconcileEdgeRoster)が反映するまで台帳が空で、
                    //  「エッジから削除→開始→停止」と素早く操作すると isPlanOnEdge が false のままになり、
                    //  ⋮メニューに「エッジ端末から削除」が出なかった(スイープが回ると出る=出たり出なかったり)。
                    //  送信が成功した=エッジが持っている、は我々が確実に知っている事実なので待つ必要がない。
                    edgeHeldByEdge.getOrPut(e.name) { mutableSetOf() }.add(planId)
                    // waitingPlans には startPlan で追加済み。以降は edgePoll(全エッジ計画対象)が各計画の状態を反映する。
                    if (currentPlanId == planId) { captureStatus.text = "● 外部端末へ転送・撮影開始" }
                    ensureEdgePoll()
                } else {
                    capturingPlans.remove(planId); waitingPlans.remove(planId); disconnectedPlans.remove(planId)
                    refreshPlanList(); updateReadOnly()
                    // エッジが理由を返していればそれを出す。「code=-3」だけでは何が悪いのか分からない
                    //  (実際にSDカードの挿し忘れで気づけなかった。2026-08-20)。
                    val nt = try { HgeNative.nativeLastEdgeNotice() } catch (_: Exception) { 0 }
                    val n1 = try { HgeNative.nativeLastEdgeNoticeN1() } catch (_: Exception) { 0 }
                    val why = if (nt != 0) noticeText(nt, n1.toLong()) else "外部端末 開始できませんでした (code=$r)"
                    Toast.makeText(this, why, Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    // エッジ計画の停止。停止確定(IDLE)まで抑止しつつ非同期停止する共通経路(beginStop)へ委譲。
    // これにより NOCAMERA ダイアログの無限再表示や、停止途中のポーリング競合を防ぐ。
    private fun stopOnEdge(e: Edge, planId: String) {
        beginStop(planId) { HgeNative.nativeEdgeStop(e.addr(), e.port, planId) }
    }
}
