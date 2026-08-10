package app.laxei.holygrail

import android.Manifest
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.ContentValues
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
    private lateinit var landscapeCheck: CheckBox
    private var suppressLandscape = false   // updatePlanDisplay でのチェック設定が native を呼ばないように
    // 項目11: 計画1ページ目の撮影方向/仰角ウィジェット(compass/elevationView/dirText)は廃止。
    // 方向・仰角は撮影シミュレーション画面で設定する(cs の azimuth/elevation は保持)。
    private lateinit var planOverview: LinearLayout      // 概要スケジュール(先頭ページ・表示専用)
    private lateinit var planPager: PlanPager            // 横スライドのページャ(先頭+薄明ページ)
    private lateinit var planFormScroll: ScrollView      // 先頭ページのフォーム縦スクロール
    private lateinit var planListScroll: ScrollView      // 先頭ページの計画リスト(分割バー上)
    private lateinit var planListContainer: LinearLayout
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
    // 撮影計画画面の「変更の取り消し」ボタンの dirty 連動(現在の計画 != ベースライン なら有効)。
    // 計画画面表示中のみ、planExec 上で現在値を取り比較する(編集と直列化して安全)。
    private val planDirtyWatch = object : Runnable {
        override fun run() {
            if (::flipper.isInitialized && flipper.displayedChild == 0 && ::resetButton.isInitialized) {
                planExec.execute {
                    val cur = try { HgeNative.nativeGetPlanJson() } catch (e: Exception) { "" }
                    runOnUiThread {
                        if (flipper.displayedChild == 0)
                            setCancelEnabled(resetButton, !planReadOnly && planBaseline.isNotEmpty() && cur != planBaseline)
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
    // この計画は「エッジに送信済み(=どこかのエッジが保有)」か。ロック(編集/削除不可)の判定に使う。
    //  開始操作の過渡(startingPlans)や実行中集合も、ロスター反映前の一瞬をロックするため含める。
    private fun isPlanOnEdge(id: String): Boolean =
        edgeHeldByEdge.values.any { it.contains(id) } || isPlanUsingCamera(id)
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
    private lateinit var edgeSearchButton: Button
    private lateinit var searchButton: Button
    private lateinit var ipInput: EditText
    private lateinit var connectButton: Button

    // 撮影制御方法初期値: メニュー + 方法別エディタ
    private lateinit var planMenu: ImageView
    private var ccmJson: JSONObject? = null     // 編集中のccm全体(初期値 or 計画固有)
    private var editingKey = "night"            // 編集中の方法
    private var editingPlanCcm = false          // true=計画固有ccmを編集 / false=初期値ccm
    private var editColor = 0                    // (旧)per-ccm色。現在は未使用(色はシステム共通へ移行)
    // システム共通の色(型ごとの文字色/背景色。0xRRGGBB)。nativeGetColors から読み込む。
    private val ccmBgMap = HashMap<Int, Int>()
    private val ccmTextMap = HashMap<Int, Int>()
    // 色の設定画面の状態
    private var colorType = "night"
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
        copyMasterAssets(baseDir)   // インストール同梱の機材マスタを /master へ展開(nativeInit より前)
        HgeNative.nativeSetLogDir(baseDir.absolutePath)
        HgeNative.nativeInit()
        HgeNative.nativeSetListener(this)
        // 起動時のログ整理(当日以外が5件以上なら古い順に削除、最新4件まで残す)。端末TZで「当日」を判定。
        val tzOffMin = java.util.TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60000
        Thread { HgeNative.nativePruneOldLogs(tzOffMin) }.start()
        handler.postDelayed(edgeTimeSync, 3000)   // 選択中エッジへ能動的な時刻同期を開始(RTC無し機/電波悪環境向け)
        handler.postDelayed(edgeSweep, 6000)      // エッジ常時スイープ(生存/IP追従+エッジ側開始・停止の検出。撮影の有無に関わらず30秒毎)
        handler.postDelayed(hgePump, 5000)        // 遅延アームのポンプ(スマホ直接撮影の予約計画の開始スレッドを期日に生成)
        loadColors()
        loadExpoValues()
        buildExposureEditors()
        loadRegisteredEdges()   // 設定で登録したエッジ端末(オフラインでも選択可)
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
        restoreEdgeState()  // 再起動時: エッジが撮影中なら状態を復元(item9)
        resumePhoneCapture()  // 再起動時: スマホ直結で撮影中だった計画を再開(item2)
        Thread { try { HgeNative.nativePresenceStart() } catch (_: Exception) {} }.start()  // P4: 常駐プレゼンスマップ開始
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
                    found.add(Edge(o.optString("name", "エッジ端末"), o.optString("ip"), o.optInt("port", 50506)))
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
            for (ed in found) {
                runOnUiThread { updateEdgeIp(ed.name, ed.ip, ed.port); refreshEdgeSpinner() }   // 発見したエッジは撮影有無に関わらず登録
                for (pid in localPlans) {
                    val pj = try { HgeNative.nativeEdgeProgress(ed.ip, ed.port, pid) } catch (_: Exception) { "" }
                    if (pj.isEmpty()) continue
                    val st = try { JSONObject(pj).optInt("state", HgeNative.ST_IDLE) } catch (_: Exception) { HgeNative.ST_IDLE }
                    val active = st == HgeNative.ST_CAPTURING || st == HgeNative.ST_SEARCHING || st == HgeNative.ST_WAITING ||
                                 st == HgeNative.ST_NOCAMERA || st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_STOPPING
                    if (!active) continue   // このエッジはこの計画を走らせていない(IDLE)
                    val disc = (st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_NOCAMERA)
                    val wait = (st == HgeNative.ST_WAITING || st == HgeNative.ST_SEARCHING)
                    runOnUiThread {
                        setPlanEdgeName(pid, ed.name)   // この計画のエッジ担当を復元(以降の停止/ポールに使う)
                        when { disc -> disconnectedPlans.add(pid); wait -> waitingPlans.add(pid); else -> { capturingPlans.add(pid); startBlink() } }
                        if (currentPlanId == pid) {
                            when {
                                disc -> { captureStatus.text = "● カメラが見つかりません(エッジ)"; captureStatus.setTextColor(0xFFD32F2F.toInt()) }
                                wait -> { captureStatus.text = "● 撮影開始待ち(エッジ)"; captureStatus.setTextColor(0xFF1565C0.toInt()) }
                                else -> { captureStatus.text = "● エッジ撮影中(復元)"; captureStatus.setTextColor(0xFF2E7D32.toInt()) }
                            }
                            captureStatus.visibility = View.VISIBLE
                        }
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
        cameraText = findViewById(R.id.plan_cameraText)
        lensText = findViewById(R.id.plan_lensText)
        intervalText = findViewById(R.id.plan_intervalText)
        npfText = findViewById(R.id.plan_npfText)
        landscapeCheck = findViewById(R.id.plan_landscape)
        planOverview = findViewById(R.id.plan_overviewContainer)
        planPager = findViewById(R.id.plan_pager)
        planFormScroll = findViewById(R.id.plan_formScroll)
        planListScroll = findViewById(R.id.plan_listScroll)
        planPager.onPageChanged = { updatePagerTitle() }
        planListContainer = findViewById(R.id.plan_listContainer)
        captureStatus = findViewById(R.id.plan_captureStatus)
        edgeSpinner = findViewById(R.id.plan_edgeSpinner)
        edgeSearchButton = findViewById(R.id.plan_edgeSearchButton)
        searchButton = findViewById(R.id.searchButton)
        ipInput = findViewById(R.id.ipInput)
        connectButton = findViewById(R.id.connectButton)
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

    // 項目I: 現在の画面に応じて「戻る」を実行する。各画面の戻るボタンと同じ動作にする。
    //  戻り先があれば true、先頭ページ(=これ以上戻れない)なら false を返す。
    //  ViewFlipper index: 0 撮影計画 / 1 撮影中 / 2 ccmメニュー(未使用) / 3 ccm編集 /
    //   4 メニュー / 5 カメラリスト / 6 カメラ追加 / 7 レンズリスト / 8 レンズ追加 / 9 色 /
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
            6 -> leaveCameraAdd(false)
            7 -> leaveLensList()
            8 -> leaveLensAdd(false)
            9 -> leaveColorScreen()
            10 -> leaveSmoothingScreen()
            11 -> leavePlacesList()
            12 -> { flipper.displayedChild = 4; buildGearMenu() }        // カメラ予約表 → メニュー
            13 -> { flipper.displayedChild = 4; buildGearMenu() }        // 操作履歴 → メニュー
            15 -> { flipper.displayedChild = 4; buildGearMenu() }        // エッジ端末設定 → メニュー
            else -> { flipper.displayedChild = 0 }
        }
        return true
    }

    private fun wireListeners() {
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
        edgeSearchButton.setOnClickListener {
            Thread {
                val js = HgeNative.nativeEdgeSearch(2000)
                runOnUiThread { onEdgesFound(js) }
            }.start()
        }
        // スピナーでエッジを選ぶと、表示中の計画にその選択を保存する(計画ごとにエッジを指定)。
        //  ユーザーがスピナーに触れた時だけ保存を許す。プログラムによる同期(refreshEdgeSpinner)で届く
        //  通知は常に無視する — これが「別計画へエッジ割当が漏れる」不具合の根治。
        edgeSpinner.setOnTouchListener { _, _ -> edgeSpinnerUserTouched = true; false }
        edgeSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, pos: Int, idl: Long) {
                if (!edgeSpinnerUserTouched) return           // プログラム由来の通知(=表示の同期)。保存しない
                edgeSpinnerUserTouched = false
                // 保存先はスピナーが表示している計画。念のため選択中の計画と一致する時だけ書く(不一致=表示が追従前)。
                val target = edgeSpinnerPlanId
                val nm = if (pos in 1..edges.size) edges[pos - 1].name else ""
                if (target.isNotEmpty() && target == currentPlanId) { setPlanEdgeName(target, nm) }
                // 【2026-08-06】保存の成否によらず、最後に「実際に保存されている値」を表示へ戻す。
                //  上のガードで弾かれると保存されないのに選んだ表示だけが残り、
                //  「Edje00 と出ているのにスマホ直結で撮影が始まる」という気づけない食い違いになる
                //  (2026-08-05 実機で発生)。表示が実態と一致していれば、その場で気づいて選び直せる。
                showStoredEdgeOnSpinner(target, nm)
            }
            override fun onNothingSelected(parent: android.widget.AdapterView<*>?) {}
        }
        searchButton.setOnClickListener { HgeNative.nativeSearchDevices() }
        connectButton.setOnClickListener {
            val host = ipInput.text.toString().trim()
            Thread { HgeNative.nativeConnectManual(host) }.start()
        }
        // 撮影計画リスト(分割バー上)の分割バー。
        setupDivider(R.id.plan_listDivider, R.id.plan_listScroll)
        // 撮影周期(タップでキーボード入力)。最小未満は警告。
        intervalText.setOnClickListener { editInterval() }
        // 横向き(ランドスケープ)。
        landscapeCheck.setOnCheckedChangeListener { _, checked ->
            if (suppressLandscape) return@setOnCheckedChangeListener
            planExec.execute { HgeNative.nativeSetPlanLandscape(if (checked) 1 else 0) }
        }
        // センサー/レンズ定数の変更(機材リストに無い値の参考用)。
        findViewById<Button>(R.id.plan_gearConst).setOnClickListener { editGearConst() }
        // 撮影制御方法の編集ボタン(全種・固定配置)をスケジュールの下に構築する。
        buildCcmEditButtons()
        // メニュー(plan_menu→600.メニュー)。帯付きの一覧から各画面へ分岐。
        planMenu.setOnClickListener { openGearMenu() }
        findViewById<ImageView>(R.id.gmenu_back).setOnClickListener { flipper.displayedChild = 0; capturePlanBaseline() }
        // 650 カメラ予約表(項目17)。戻る/メニューどちらもメニューへ戻す。
        findViewById<ImageView>(R.id.reserve_back).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        findViewById<ImageView>(R.id.reserve_menu).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        // 660 操作履歴(項目9)
        findViewById<ImageView>(R.id.history_back).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        findViewById<ImageView>(R.id.history_menu).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        // 670 撮影レポート。読むだけの画面なので離脱時に保存するものは無い。
        findViewById<ImageView>(R.id.report_back).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        findViewById<ImageView>(R.id.report_menu).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        setupDivider(R.id.report_divider, R.id.report_listScroll)
        // 8.2 エッジ端末設定(2026-08-08 UI依頼で画面化)
        findViewById<ImageView>(R.id.edge_back).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        findViewById<ImageView>(R.id.edge_menu).setOnClickListener { flipper.displayedChild = 4; buildGearMenu() }
        setupDivider(R.id.edge_divider, R.id.edge_listScroll)
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
        findViewById<ImageView>(R.id.cameralist_back).setOnClickListener { leaveCameraList() }
        findViewById<ImageView>(R.id.cameralist_menu).setOnClickListener { leaveCameraList() }
        setupDivider(R.id.cameralist_divider, R.id.cameralist_listScroll)
        // 622 カメラ追加(離脱時にチェックを追加 / 取消でチェック解除)
        findViewById<ImageView>(R.id.cameraadd_back).setOnClickListener { leaveCameraAdd(false) }
        findViewById<ImageView>(R.id.cameraadd_menu).setOnClickListener { leaveCameraAdd(true) }
        findViewById<Button>(R.id.cameraadd_cancel).setOnClickListener { checkedCamAdd.clear(); buildCameraAdd() }
        // 630 所持レンズ
        findViewById<ImageView>(R.id.lenslist_back).setOnClickListener { leaveLensList() }
        findViewById<ImageView>(R.id.lenslist_menu).setOnClickListener { leaveLensList() }
        setupDivider(R.id.lenslist_divider, R.id.lenslist_listScroll)
        // 632 レンズ追加
        findViewById<ImageView>(R.id.lensadd_back).setOnClickListener { leaveLensAdd(false) }
        findViewById<ImageView>(R.id.lensadd_menu).setOnClickListener { leaveLensAdd(true) }
        findViewById<Button>(R.id.lensadd_cancel).setOnClickListener { checkedLensAdd.clear(); buildLensAdd() }
        // 640 撮影場所リスト(§7.9)。戻る/メニューで離脱時に自動保存。
        findViewById<ImageView>(R.id.places_back).setOnClickListener { leavePlacesList() }
        findViewById<ImageView>(R.id.places_menu).setOnClickListener { leavePlacesList() }
        setupDivider(R.id.places_divider, R.id.places_listScroll)
        // 撮影計画(330)のカメラ/レンズをタップで所持から選択する。
        cameraText.setOnClickListener { choosePlanCamera() }
        lensText.setOnClickListener { choosePlanLens() }
        findViewById<Button>(R.id.cmenu_night).setOnClickListener { openCcmEdit("night") }
        findViewById<Button>(R.id.cmenu_sunrise).setOnClickListener { openCcmEdit("sunrise") }
        findViewById<Button>(R.id.cmenu_sunset).setOnClickListener { openCcmEdit("sunset") }
        findViewById<Button>(R.id.cmenu_day).setOnClickListener { openCcmEdit("day") }
        findViewById<ImageView>(R.id.edit_back).setOnClickListener { stopDirtyWatch(); persistCcmEdit(); flipper.displayedChild = if (editingPlanCcm) 0 else 4 }
        findViewById<Button>(R.id.edit_save).visibility = View.GONE   // 取消はエディタ先頭行へ移動
        // 色はメニュー「色の設定」(システム共通)で設定する(per-ccm色は廃止)。
        findViewById<ImageView>(R.id.color_back).setOnClickListener { leaveColorScreen() }
        findViewById<ImageView>(R.id.color_menu).setOnClickListener { leaveColorScreen() }
        // 露出平滑化(630)。戻る/メニューで保存して離脱。取り消しで保存値から再読込。
        findViewById<ImageView>(R.id.smooth_back).setOnClickListener { leaveSmoothingScreen() }
        findViewById<ImageView>(R.id.smooth_menu).setOnClickListener { leaveSmoothingScreen() }
        findViewById<Button>(R.id.smooth_cancel).setOnClickListener { loadSmoothingScreen(); resetDirtyBaseline() }
        setupValueSlider(R.id.smooth_hyst_seek, 20) {
            findViewById<TextView>(R.id.smooth_hyst_val).text = String.format("%.1fev", seekToHyst(it))
        }
        setupValueSlider(R.id.smooth_ma_seek, 10) {
            findViewById<TextView>(R.id.smooth_ma_val).text = "${it}frame"
        }
        // 初期値プリセット一覧の分割バー(620と同挙動)
        setupDivider(R.id.edit_presetDivider, R.id.edit_presetScroll)
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
        gearItem(box, "カメラ予約表") { openReserveTable() }   // 項目17
        gearItem(box, "操作履歴") { openHistory() }            // 項目9
        gearItem(box, "撮影レポート") { openReportList() }     // 670: 撮影1回ぶんの結果と所見
        gearBand(box, "撮影制御方法 初期値")
        // 項目3: 「月の影響への対処」は撮影制御方法初期値から削除。
        gearItem(box, "夜間撮影") { openPresetScreen("night") }
        gearItem(box, "朝日撮影") { openPresetScreen("sunrise") }
        gearItem(box, "夕日撮影") { openPresetScreen("sunset") }
        gearItem(box, "日中撮影") { openPresetScreen("day") }
        gearBand(box, "自動露出")
        gearItem(box, "露出平滑化") { openSmoothingScreen() }
        gearBand(box, "色の設定")
        // 項目3: 「月の影響への対処」は色の設定から削除。
        gearColorItem(box, "night", "夜間撮影")
        gearColorItem(box, "sunrise", "朝日撮影")
        gearColorItem(box, "sunset", "夕日撮影")
        gearColorItem(box, "day", "日中撮影")
        gearColorItem(box, "preNight", "夜間前移行")
        gearColorItem(box, "postNight", "夜間後移行")
        gearBand(box, "所持機材")
        gearItem(box, "カメラリスト") { openCameraList() }
        gearItem(box, "レンズリスト") { openLensList() }
        gearBand(box, "エッジ端末")
        // 2026-08-08 UI依頼: 「登録」と「設定」を1画面へ統合した(登録は画面内の
        // 「＋ 新規エッジ端末」から行う)。
        gearItem(box, "エッジ端末設定") { openEdgeSettings() }
        gearBand(box, "ログ")
        // 撮影中/開始要求中はグレー表示で不可(コピー処理が撮影と競合しないように)。
        gearItem(box, "ログ取得", enabled = !isCaptureBusy()) { retrieveLogs() }

        // 版数を一番下に出す(2026-08-08 UI依頼)。どのビルドを使っているかを画面だけで確認できる。
        // major.minor はエッジ端末と一致させる約束なので、食い違っていたらどちらかの書き込み漏れ。
        box.addView(TextView(this).apply {
            text = "バージョン " + appVersionName()
            textSize = 12f
            setTextColor(0xFF9E9E9E.toInt())
            setPadding(dp(16), dp(16), dp(16), dp(8))
        })
    }

    // このアプリの版数("0.0.x")。build.gradle.kts の versionName をそのまま読む。
    private fun appVersionName(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
    } catch (_: Exception) { "?" }

    // 撮影実行中/開始要求(待機・未検出含む)中か。ログ取得の可否判定に使う。
    private fun isCaptureBusy(): Boolean =
        capturingPlans.isNotEmpty() || waitingPlans.isNotEmpty() || disconnectedPlans.isNotEmpty()

    // ログ取得: スマホ自身のログ + オンラインの各エッジのログを、ユーザーが見える場所へコピーする。
    // 保存先: スマホ = Download/hgclog/、エッジ = Download/hgclog-<端末名>/(何のフォルダか分かるように)。
    // 同名ファイルは上書き(内容は同じはずなので日時フォルダで増やさない)。
    // コピー中はキャンセル不可のダイアログで他操作を抑止する(§ログ取得。仕様書外の追加機能)。
    private fun retrieveLogs() {
        if (isCaptureBusy()) { Toast.makeText(this, "撮影中/開始要求中はログ取得できません", Toast.LENGTH_SHORT).show(); return }
        // API28以下は公開Downloadsへ直接書くため書込み権限が要る(29+はMediaStoreで不要)。
        if (Build.VERSION.SDK_INT < 29 &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE), 4713)
            Toast.makeText(this, "ストレージ権限を許可してからもう一度お試しください", Toast.LENGTH_LONG).show()
            return
        }
        val dlg = AlertDialog.Builder(this).setTitle("ログ取得中").setMessage("準備中...").setCancelable(false).create()
        dlg.setCanceledOnTouchOutside(false); dlg.show()
        fun setMsg(m: String) = runOnUiThread { dlg.setMessage(m) }
        Thread {
            var copied = 0
            val errors = StringBuilder()
            try {
                // 1) スマホ自身のログ(getExternalFilesDir/log/hg_*.log)→ Download/hgclog/
                setMsg("スマホのログをコピー中...")
                val logDir = java.io.File(getExternalFilesDir(null), "log")
                val phoneLogs = logDir.listFiles { f -> f.isFile && f.name.endsWith(".log") } ?: emptyArray()
                for (f in phoneLogs) {
                    try { saveToDownloads("hgclog", f.name, f.readBytes()); copied++ }
                    catch (e: Exception) { errors.append("phone/${f.name} ") }
                }
                // 2) オンラインのエッジ端末のログ(エッジ名のサブフォルダへ)
                setMsg("エッジ端末を検索中...")
                val edgesJson = try { JSONArray(HgeNative.nativeEdgeSearch(2500)) } catch (e: Exception) { JSONArray() }
                for (i in 0 until edgesJson.length()) {
                    val o = edgesJson.optJSONObject(i) ?: continue
                    val ip = o.optString("ip"); val port = o.optInt("port", 50506)
                    if (ip.isEmpty()) continue
                    val ename = o.optString("name").ifEmpty { "edge_$ip" }
                    setMsg("エッジ「$ename」のログ一覧を取得中...")
                    val listJson = try { JSONArray(HgeNative.nativeEdgeLogList(ip, port)) } catch (e: Exception) { JSONArray() }
                    for (k in 0 until listJson.length()) {
                        val logName = listJson.optString(k); if (logName.isEmpty()) continue
                        setMsg("エッジ「$ename」: $logName")
                        val bytes = fetchEdgeLog(ip, port, logName)
                        if (bytes.isNotEmpty()) {
                            try { saveToDownloads("hgclog-" + sanitizeFolder(ename), logName, bytes); copied++ }
                            catch (e: Exception) { errors.append("$ename/$logName ") }
                        }
                    }
                }
            } catch (e: Exception) { errors.append("(${e.message}) ") }
            val n = copied
            runOnUiThread {
                dlg.dismiss()
                val msg = if (errors.isEmpty()) "ログ $n 件を\nDownload/hgclog(スマホ)・hgclog-<端末名>(エッジ)\nに保存しました" else "ログ $n 件を保存(一部失敗: $errors)"
                AlertDialog.Builder(this).setTitle("ログ取得").setMessage(msg).setPositiveButton("OK", null).show()
            }
        }.start()
    }

    // エッジのログ name を分割取得して全バイトを返す(4KBチャンクで offset を進める)。
    private fun fetchEdgeLog(ip: String, port: Int, name: String): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        var offset = 0; var guard = 0
        while (guard++ < 20000) {
            val chunk = try { HgeNative.nativeEdgeLogRead(ip, port, name, offset) } catch (e: Exception) { ByteArray(0) }
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
        val lab1 = TextView(this); lab1.text = "文字の色"; lab1.textSize = 14f; box.addView(lab1)
        val p1 = com.jaredrummler.android.colorpicker.ColorPickerView(this)
        p1.setAlphaSliderVisible(true); p1.setColor(ccmTextColor(t), true)
        // ピッカー変更で即タイトル文字色に反映(その場で見た目確認)。
        p1.setOnColorChangedListener { c -> findViewById<TextView>(R.id.color_title).setTextColor(0xFF000000.toInt() or (c and 0xFFFFFF)) }
        box.addView(p1); colorTextPicker = p1
        val lab2 = TextView(this); lab2.text = "背景の色"; lab2.textSize = 14f; lab2.setPadding(0, dp(16), 0, 0); box.addView(lab2)
        val p2 = com.jaredrummler.android.colorpicker.ColorPickerView(this)
        p2.setAlphaSliderVisible(true); p2.setColor(ccmColor(t), true)
        // ピッカー変更で即タイトル背景色に反映。
        p2.setOnColorChangedListener { c -> findViewById<View>(R.id.color_header).setBackgroundColor(0xFF000000.toInt() or (c and 0xFFFFFF)) }
        box.addView(p2); colorBgPicker = p2
        // 項目2: 色の設定にも「変更の取り消し」ボタンを新設(dirty 連動。取消=保存色から作り直し)。
        val colorCancel = addCancelButton(box, atTop = true) { buildColorScreen() }
        startDirtyWatch(colorCancel) { "${colorTextPicker?.color ?: 0},${colorBgPicker?.color ?: 0}" }
    }

    private fun leaveColorScreen() { stopDirtyWatch(); saveColorScreen(); flipper.displayedChild = 4; buildGearMenu() }

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
    private fun leaveSmoothingScreen() { stopDirtyWatch(); saveSmoothingScreen(); flipper.displayedChild = 4; buildGearMenu() }
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
        Thread { HgeNative.nativeSetColors(js); runOnUiThread { loadColors() } }.start()
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
        setInitialSplit(presetScrollId(), presetListId())
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

    // プリセット一覧。名称はその場で直接編集可。新規追加も文字を直接入力。⋮=緑ピル。
    private fun buildPresetList(containerId: Int) {
        val box = findViewById<LinearLayout>(containerId); box.removeAllViews()
        val prefName = HgeNative.nativeGetPreferredCcm(presetType)
        for (p in presetCcms) {
            val nm = p.optString("name")
            val row = LinearLayout(this); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
            val star = TextView(this); star.text = if (nm == prefName) "★" else "　"; star.textSize = 14f; star.setPadding(dp(2), 0, dp(4), 0)
            row.addView(star)
            if (nm == selPresetName) {
                // 選択中のみ名称インライン編集可(分割バー画面共通の動作)。
                val et = EditText(this); et.setText(nm); et.isSingleLine = true; et.textSize = 19f
                et.setBackgroundColor(0x00000000); et.setTypeface(null, Typeface.BOLD); et.setTextColor(Color.BLACK)
                et.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                et.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
                et.setOnFocusChangeListener { _, has -> if (!has) commitRename(nm, et.text.toString()) }
                et.setOnEditorActionListener { v, a, _ ->
                    if (a == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) {
                        commitRename(nm, v.text.toString()); v.clearFocus()
                        (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager).hideSoftInputFromWindow(v.windowToken, 0)
                        true
                    } else false
                }
                row.addView(et)
            } else {
                // 未選択はタップで選択のみ(キーボードを出さない)。
                val t = TextView(this); t.text = nm; t.textSize = 16f; t.setTextColor(Color.parseColor("#888888"))
                t.setPadding(0, dp(6), 0, dp(6))
                t.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                t.setOnClickListener { selectPreset(nm) }
                row.addView(t)
            }
            row.addView(ctxMenuButton(listOf("削除" to { removePreset(nm) })))
            box.addView(row); box.addView(thinDivider())
        }
        // 新規追加(文字を直接入力して確定で作成)
        val addEt = EditText(this); addEt.hint = "＋ 新規追加"; addEt.isSingleLine = true; addEt.textSize = 16f
        addEt.setBackgroundColor(0x00000000); addEt.setTextColor(Color.parseColor("#1565C0"))
        addEt.setPadding(dp(6), dp(8), dp(6), dp(8))
        addEt.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
        val commitAdd = { val t = addEt.text.toString().trim(); if (t.isNotEmpty()) { addEt.setText(""); addPresetNamed(t) } }
        addEt.setOnEditorActionListener { _, _, _ -> commitAdd(); true }
        addEt.setOnFocusChangeListener { _, has -> if (!has) commitAdd() }
        box.addView(addEt)
    }

    private fun rebuildPresetList() { buildPresetList(presetListId()) }
    private fun persistCurrentPreset() { persistCcmEdit() }

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

    // インストール同梱の assets/master/*.json を /master へコピーする。
    // osfile のベース = getExternalFilesDir(null) なので dataManager は /master で読める。
    // 同梱アセットを常に上書きコピーする: lenses_list.json 等の機材マスタ(fisheye 等)を
    // 編集→ビルドすれば、次回起動で即反映される(「ファイルを変更すれば即反映」)。
    // (将来サーバ更新を入れる場合は、内容差分やバージョンで上書き可否を判断する。)
    private fun copyMasterAssets(baseDir: java.io.File) {
        try {
            val dir = java.io.File(baseDir, "master")
            if (!dir.exists()) dir.mkdirs()
            for (name in listOf("cameras.json", "lenses.json")) {
                val out = java.io.File(dir, name)
                assets.open("master/$name").use { ins ->
                    out.outputStream().use { os -> ins.copyTo(os) }
                }
            }
        } catch (e: Exception) {
            // マスタ未配置でも entity 側の出荷時フォールバックで最低限動く。
        }
    }

    // --- 選択状態と詳細編集の参照 ---
    private var selCamera: String? = null               // 620 選択中の所持カメラ名
    private var selLens: String? = null                 // 630 選択中の所持レンズ名
    private val camFields = HashMap<String, EditText>()  // カメラ詳細の入力欄
    private var camAutoInsert: CheckBox? = null
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

    private fun openCameraList() { buildCameraList(); buildCameraDetail(); setInitialSplit(R.id.cameralist_listScroll, R.id.cameralist_container); flipper.displayedChild = 5 }
    private fun openCameraAdd()  { checkedCamAdd.clear(); buildCameraAdd(); flipper.displayedChild = 6 }
    private fun openLensList()   { buildLensList(); buildLensDetail(); setInitialSplit(R.id.lenslist_listScroll, R.id.lenslist_container); flipper.displayedChild = 7 }
    private fun openLensAdd()    { checkedLensAdd.clear(); expandedMakers.clear(); buildLensAdd(); flipper.displayedChild = 8 }

    // 分割バーの初期高さ(上=リスト)。リストが短ければ内容ぴったりまで上に詰め、
    // 多い場合(内容が画面の1/4超)は1/4で止める。ほとんどは1件なので上寄せになる。
    private fun setInitialSplit(listId: Int, containerId: Int) {
        val v = findViewById<View>(listId)
        val c = findViewById<View>(containerId)
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
    private fun listRow(title: String, sub: String, selected: Boolean, onSelect: () -> Unit,
                        menuItems: List<Pair<String, () -> Unit>>, onRename: ((String) -> Unit)? = null): View {
        val row = LinearLayout(this)
        row.orientation = LinearLayout.HORIZONTAL
        row.gravity = Gravity.CENTER_VERTICAL
        row.setPadding(dp(6), dp(8), dp(6), dp(8))
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
    private fun buildCameraList() {
        val box = findViewById<LinearLayout>(R.id.cameralist_container)
        box.removeAllViews()
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera")?.optString("name") }
        if (selCamera == null || selCamera !in names) selCamera = names.firstOrNull()
        for (i in 0 until arr.length()) {
            val cam = arr.optJSONObject(i)?.optJSONObject("camera") ?: continue
            val name = cam.optString("name")
            // 項目D: 愛称/シリアルは未取得なら「未定義」と出す(SSDPでオンライン取得後に実値が入る)。
            val fr = cam.optString("friendly").ifEmpty { "未定義" }
            val sn = cam.optString("serial").ifEmpty { "未定義" }
            val sub = "愛称:$fr  S/N:$sn"
            box.addView(listRow(name, sub, name == selCamera,
                onSelect = { selectCamera(name) },
                menuItems = listOf(
                    "削除" to {
                        Thread { HgeNative.nativeRemoveOwnedCamera(name)
                            runOnUiThread { if (selCamera == name) selCamera = null; buildCameraList(); buildCameraDetail() } }.start()
                    },
                    "接続カメラ検索" to { searchAndAddCameras() }
                ),
                onRename = { newName -> commitCameraRename(name, newName) }))
            box.addView(thinDivider())
        }
        box.addView(linkText("＋ 新規カメラ追加") { openCameraAdd() })
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
        box.removeAllViews(); camFields.clear(); camAutoInsert = null; camLensNames.clear()
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
        // 項目D: 愛称(FriendlyName)とシリアルNo.は、カメラがオンラインになりSSDPで取得できてから
        //  自動で入る。手入力はしない(勝手に入れない)。未取得のうちは「未定義」と表示する。
        box.addView(displayRow("愛称", cam.optString("friendly").ifEmpty { "未定義" }))
        box.addView(displayRow("シリアルNo.", cam.optString("serial").ifEmpty { "未定義" }))
        box.addView(editRow2("センサーサイズ", "sensorSize", cam.optDouble("sensorSize", 0.0).toString(),
            "sensorSizeV", cam.optDouble("sensorSizeV", 0.0).toString(), "×", "mm", true))
        box.addView(editRow("センサーpixel(横)", "sensorPixel", cam.optInt("sensorPixel", 0).toString(), true))
        val iso = arrMinMax(cam.optJSONArray("isoList"))
        box.addView(editRow2("ISO感度", "isoMin", iso.first, "isoMax", iso.second, "〜", "", false))
        val ss = arrMinMax(cam.optJSONArray("ssList"))
        box.addView(editRow2("シャッター速度", "ssMin", ss.first, "ssMax", ss.second, "〜", "", false))
        val cb = CheckBox(this); cb.text = "撮影計画の初期値にする"; cb.isChecked = ocObj?.optBoolean("autoInsert", false) ?: false
        camAutoInsert = cb; box.addView(cb)

        // 組み合わせるレンズ(先頭=初期値)。並べ替えはハンドルをドラッグ(ss/iso/fnと同じ)。
        box.addView(thinDivider())
        val hdr = TextView(this); hdr.text = "組み合わせるレンズ(先頭が初期値)"; hdr.textSize = 13f; hdr.setTextColor(Color.GRAY)
        hdr.setPadding(0, dp(8), 0, dp(4)); box.addView(hdr)
        ocObj?.optJSONArray("lensList")?.let { ll -> for (i in 0 until ll.length()) ll.optJSONObject(i)?.optString("name")?.let { camLensNames.add(it) } }
        val lensBox = LinearLayout(this); lensBox.orientation = LinearLayout.VERTICAL
        box.addView(lensBox); camLensContainer = lensBox
        renderCamLensReorder()
        // 項目2: 「変更の取り消し」を dirty 連動に(未変更=グレー無効、変更で有効、戻すと無効)。
        startDirtyWatch(camCancel) { camDetailSig() }
    }

    // 所持カメラ詳細の編集内容シグネチャ(dirty 比較用)。編集欄・初期値チェック・レンズ順序を連結。
    private fun camDetailSig(): String {
        val sb = StringBuilder()
        camFields.toSortedMap().forEach { (k, v) -> sb.append(k).append('=').append(v.text).append(';') }
        sb.append("auto=").append(camAutoInsert?.isChecked == true).append(";lens=").append(camLensNames.joinToString(","))
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

    private fun leaveCameraList() { stopDirtyWatch(); persistCameraDetail(false); flipper.displayedChild = 4 }

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
        val orig = origName ?: selCamera ?: return
        if (camFields.isEmpty()) return
        val o = JSONObject()
        for ((k, et) in camFields) {
            when (k) {
                "sensorSize", "sensorSizeV" -> o.put(k, et.text.toString().toDoubleOrNull() ?: 0.0)
                "sensorPixel" -> o.put(k, et.text.toString().toIntOrNull() ?: 0)
                else -> o.put(k, et.text.toString())
            }
        }
        o.put("name", selCamera ?: orig)      // 名称はリストのインライン編集分(selCamera)
        o.put("autoInsert", camAutoInsert?.isChecked ?: false)
        val ln = JSONArray(); camLensNames.forEach { ln.put(it) }; o.put("lensNames", ln)
        val js = o.toString()
        if (rebuild) {
            Thread {
                HgeNative.nativeSetOwnedCameraDetail(orig, js)
                runOnUiThread { buildCameraList(); buildCameraDetail() }
            }.start()
        } else {
            Thread { HgeNative.nativeSetOwnedCameraDetail(orig, js) }.start()
        }
    }

    // ---------- 622 カメラ追加(マスタ。チェックで複数追加) ----------
    private fun buildCameraAdd() {
        val box = findViewById<LinearLayout>(R.id.cameraadd_container)
        box.removeAllViews()
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
    private fun leaveCameraAdd(toMenu: Boolean) {
        val sel = ArrayList(checkedCamAdd); checkedCamAdd.clear()
        if (sel.isEmpty()) { if (toMenu) openGearMenu() else openCameraList(); return }
        Thread {
            // §4a: 同機種で未識別(愛称もシリアルも無い)のカメラが既にあると、もう一台は区別できないため追加不可。
            var ok = 0; val failed = ArrayList<String>()
            sel.forEach { if (HgeNative.nativeAddOwnedCamera(it) == 0) ok++ else failed.add(it) }
            runOnUiThread {
                val msg = when {
                    failed.isEmpty() -> "${ok}台を追加しました"
                    ok == 0 -> "追加できません: 同機種で未識別のカメラが既にあります。先に愛称かシリアルを設定してください"
                    else -> "${ok}台追加。${failed.size}台は追加不可(同機種の未識別カメラが既にあるため)"
                }
                Toast.makeText(this, msg, Toast.LENGTH_LONG).show(); if (toMenu) openGearMenu() else openCameraList()
            }
        }.start()
    }

    // ---------- 630 所持レンズ ----------
    private fun buildLensList() {
        val box = findViewById<LinearLayout>(R.id.lenslist_container)
        box.removeAllViews()
        val arr = camArray(HgeNative.nativeGetOwnedLenses())
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }
        if (selLens == null || selLens !in names) selLens = names.firstOrNull()
        for (i in 0 until arr.length()) {
            val l = arr.optJSONObject(i) ?: continue
            val name = l.optString("name")
            val sub = "${l.optString("maker")}  ${l.optDouble("focalLength", 0.0).toInt()}mm  F${l.optDouble("fn", 0.0)}"
            box.addView(listRow(name, sub, name == selLens,
                onSelect = { selectLens(name) },
                menuItems = listOf("削除" to {
                    Thread { HgeNative.nativeRemoveOwnedLens(name)
                        runOnUiThread { if (selLens == name) selLens = null; buildLensList(); buildLensDetail() } }.start()
                }),
                onRename = { newName -> commitLensRename(name, newName) }))
            box.addView(thinDivider())
        }
        box.addView(linkText("＋ 新規レンズ追加") { openLensAdd() })
    }

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

    private fun leaveLensList() { stopDirtyWatch(); persistLensDetail(false); flipper.displayedChild = 4 }

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
            Thread { HgeNative.nativeSetOwnedLensDetail(orig, js); runOnUiThread { buildLensList(); buildLensDetail() } }.start()
        } else {
            Thread { HgeNative.nativeSetOwnedLensDetail(orig, js) }.start()
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

    private fun leaveLensAdd(toMenu: Boolean) {
        val sel = ArrayList(checkedLensAdd); checkedLensAdd.clear()
        if (sel.isEmpty()) { if (toMenu) openGearMenu() else openLensList(); return }
        Thread {
            sel.forEach { HgeNative.nativeAddOwnedLens(it) }
            runOnUiThread { Toast.makeText(this, "${sel.size}本を追加しました", Toast.LENGTH_SHORT).show(); if (toMenu) openGearMenu() else openLensList() }
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
    private fun choosePlanCamera() {
        val arr = camArray(HgeNative.nativeGetOwnedCameras())
        if (arr.length() == 0) { openCameraList(); return }
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optJSONObject("camera")?.optString("name") }
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("カメラを選択")
            .setItems(names.toTypedArray()) { _, which ->
                val name = names[which]
                planExec.execute {
                    HgeNative.nativeSetPlanCamera(name)
                    val sched = HgeNative.nativeScheduleJson()
                    runOnUiThread { latestSchedule = sched; updatePlanDisplay(sched); reloadExpoEditors() }  // item3: iso/ss範囲をカメラに合わせ直す
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

    // 撮影制御方法の編集ボタン(全5種)を常に定位置(スケジュールの下)に並べる。タップで編集画面へ。
    // 3列グリッド(夜間/朝日/夕日 と 日中/月)。色分けして「ボタンらしい」見た目にする。
    private fun buildCcmEditButtons() {
        val parent = findViewById<LinearLayout>(R.id.plan_ccmButtons)
        parent.removeAllViews()
        // 1=夜間 2=朝日 3=夕日 4=日中(月は廃止)。
        val groups = listOf(listOf(1, 2, 3), listOf(4))
        for (group in groups) {
            val row = LinearLayout(this)
            row.orientation = LinearLayout.HORIZONTAL
            row.layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            for (t in group) {
                val key = ccmTypeToKey[t] ?: continue
                // 他のボタン(保存/撮影開始/接続/リセット)と同じ形にするため、テーマ既定の
                // materialButtonStyle で生成し、角丸はスタイル(=他ボタンと同一)を継承する。
                val btn = com.google.android.material.button.MaterialButton(
                    this, null, com.google.android.material.R.attr.materialButtonStyle)
                btn.text = ccmTypeName[t]
                btn.isAllCaps = false
                btn.textSize = 13f
                btn.setTextColor(ccmTextColor(t))
                btn.backgroundTintList = ColorStateList.valueOf(ccmColor(t))
                btn.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    .apply { setMargins(dp(2), dp(2), dp(2), dp(2)) }
                btn.setOnClickListener { openPlanCcmEdit(key) }
                row.addView(btn)
            }
            // 列数を3に揃えるための空きスペーサ(最終行の日中/月を左寄せ・同幅にする)
            repeat(3 - group.size) {
                val sp = android.view.View(this)
                sp.layoutParams = LinearLayout.LayoutParams(0, 1, 1f)
                row.addView(sp)
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
        val title = mapOf("night" to "夜間撮影", "sunrise" to "朝日撮影", "sunset" to "夕日撮影", "day" to "日中撮影")[key]
        findViewById<TextView>(R.id.edit_title).text = title + (if (editingPlanCcm) "（この計画）" else "（初期値）")
        applyHeaderColor(R.id.edit_header, R.id.edit_title, keyType(key))   // タイトルバーにシステム共通色
        val showPreset = !editingPlanCcm   // 初期値編集時のみプリセット一覧を出す
        findViewById<View>(R.id.edit_presetScroll).visibility = if (showPreset) View.VISIBLE else View.GONE
        findViewById<View>(R.id.edit_presetDivider).visibility = if (showPreset) View.VISIBLE else View.GONE

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
        val cancelBtn = addPresetTopRow(R.id.edit_content, { cancelCcmEdit() }, onPick)
        startDirtyWatch(cancelBtn) { buildCcmEditJson()?.toString() ?: "" }      // item8: 変更で赤・未変更でグレー
        flipper.displayedChild = 3
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
        o.put("color", ccmBgMap[keyType(editingKey)] ?: 0)   // 色はシステム共通(転送用に派生値を入れる)
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
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    v.parent?.requestDisallowInterceptTouchEvent(true)   // ScrollView のスクロールを抑止
                    dragFrom = index; highlightGap(ev.rawY); true
                }
                MotionEvent.ACTION_MOVE -> { highlightGap(ev.rawY); true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { dropAt(ev.rawY); true }
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
                dividers[k].setBackgroundColor(if (k == g) 0xFF1565C0.toInt() else 0x00000000)
            }
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
    private fun refreshPlanList() {
        // 一覧の読み出しも計画操作と同じ単一スレッドで実行し、改名・編集の直後に最新状態を読む。
        planExec.execute {
            val js = HgeNative.nativeListPlans()
            val cur = HgeNative.nativeCurrentPlanId()
            // 項目5: 計画の新規作成/変更/削除いずれの経路も refreshPlanList を通るので、ここで予約表を
            //  全計画から作り直す(終了が過去の計画は buildReservations 内で除外)。削除した計画が予約表に
            //  残る・変更が反映されない不具合の根治。buildReservations/saveReservations はUI非依存。
            saveReservations(buildReservations())
            runOnUiThread { currentPlanId = cur; buildPlanList(js); updateReadOnly(); refreshEdgeSpinner() }
        }
    }

    private fun buildPlanList(js: String) {
        planListContainer.removeAllViews()
        // 常に先頭に「新規撮影計画」行(タップで初期値の新規計画を作成)。
        val add = TextView(this)
        add.text = "＋ 新規撮影計画"
        add.textSize = 15f
        add.setPadding(dp(8), dp(10), dp(8), dp(10))
        add.setTextColor(0xFF1565C0.toInt())
        add.setOnClickListener { doNewPlan() }
        planListContainer.addView(add)
        try {
            val arr = JSONArray(js)
            for (i in 0 until arr.length()) { planListContainer.addView(buildPlanRow(arr.getJSONObject(i))) }
        } catch (_: Exception) {}
        // 再構築直後に選択中行のEditTextが自動フォーカスしてキーボードが出るのを防ぐ(フォーカスをスクロールへ)。
        planListScroll.isFocusableInTouchMode = true
        planListScroll.requestFocus()
        // リスト件数が少なければ内容ぴったりまで縮める(item6: リスト最下段で止める)。
        setInitialSplit(R.id.plan_listScroll, R.id.plan_listContainer)
    }

    private fun buildPlanRow(p: JSONObject): View {
        val id = p.optString("id")
        val name = p.optString("name")
        val capturable = p.optBoolean("capturable")
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
        tv.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
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
                        val r = HgeNative.nativeRenamePlan(id, nm)   // item5: 重複なら ERR_NAME_DUP
                        runOnUiThread { if (r == HgeNative.ERR_NAME_DUP) showNameInUse(nm); refreshPlanList() }
                    }
                }
            }
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
                override fun onViewDetachedFromWindow(v: View) { commit() }
            })
        } else {
            // 未選択(または撮影中)の行 → タップで選択のみ(キーボードは出さない)。
            tv.isFocusable = false; tv.isFocusableInTouchMode = false
            tv.setOnClickListener { selectPlanRow(id) }
        }
        row.addView(tv)
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
        val onEdge = isPlanOnEdge(id)
        // 「実撮影中」だけを撮影中とみなす。中止操作直後は stoppingPlans に数秒残るが、
        //  中止済みならもう撮影していないので「エッジ端末から削除」を出してよい。
        val capturingNow = capturingPlans.contains(id)
        val menu = mutableListOf<Pair<String, () -> Unit>>("コピーを追加" to { copyPlanRow(id) })
        if (!onEdge) menu.add("削除" to { confirmDeletePlan(id, name) })
        // 項目2: エッジ送信済み(ロック中)かつ未撮影なら「エッジ端末から削除」で外して編集可能にする。
        //  停止しただけの計画はエッジが保有し続けロックされたまま(項目4)なので、これで明示的に外す。
        if (onEdge && !capturingNow) menu.add("エッジ端末から削除" to { confirmRemoveFromEdge(id, name) })
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

    private fun doNewPlan() {
        planExec.execute {
            HgeNative.nativeNewPlan("")
            val cur = HgeNative.nativeCurrentPlanId()
            val list = buildReservations(); saveReservations(list)   // 項目17: 新規作成時に予約表を作り直す
            runOnUiThread { currentPlanId = cur; refreshPlanList() }
        }
    }

    // 項目5/6(再修正): 開始要求などの非同期処理が終わっても、ユーザーが見ている計画を勝手に切り替えない。
    //  native 側の「選択中の計画」は処理の都合で対象計画へ移動するが、UIの表示は動かさず、
    //  終わったら native の選択を表示中の計画へ戻して UI と native の食い違いを解消する。
    private fun restoreViewingSelection() {
        val viewing = currentPlanId
        if (viewing.isEmpty()) return
        planExec.execute { try { HgeNative.nativeSelectPlan(viewing) } catch (_: Exception) {} }
    }

    private fun startPlan(id: String) {
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
                HgeNative.nativeSelectPlan(id)
                val r = HgeNative.nativeCaptureStartPlan(id)
                runOnUiThread {
                    startingPlans.remove(id)   // 開始要求の結果確定
                    when (r) {
                        HgeNative.ERR_OVERLAP_LIMIT -> { waitingPlans.remove(id); Toast.makeText(this, "撮影期間が重なる計画は2件までです。時間をずらすか他の計画を停止してください", Toast.LENGTH_LONG).show() }
                        HgeNative.ERR_QUEUE_FULL   -> { waitingPlans.remove(id); Toast.makeText(this, "撮影開始要求が上限(100件)に達しました", Toast.LENGTH_LONG).show() }
                        else -> {}   // 待機はタップ時に反映済み。実状態はEV_STATEで即補正される
                    }
                    refreshPlanList(); updateReadOnly()
                    restoreViewingSelection()   // 項目5: 表示中の計画を勝手に変えない
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
                        .setTitle("エッジ端末が見つかりません")
                        .setMessage("エッジ端末「${name}」が見つからないため撮影を開始できません。\n端末の電源・ネットワーク接続を確認してください。")
                        .setPositiveButton("OK", null)
                        .show()
                    return@runOnUiThread
                }
                updateEdgeIp(name, e.ip, e.port)   // 解決したIPを保持(停止/状態確認に使う)
                // 項目5: planExec では「選択→計画JSON取得」だけを素早く原子的に行い(選択中の計画に依存する
                //  グローバル取得のため直列化が必要)、遅いネットワーク送信は edgeExec へ逃がす。これで送信中も
                //  計画選択(planExec)が詰まらず UI が固まらない。送る JSON は取得済みの値を startOnEdge へ渡す。
                planExec.execute {
                    HgeNative.nativeSelectPlan(id)
                    val planJson = try { HgeNative.nativeGetPlanJson() } catch (_: Exception) { "" }
                    // 待機はタップ時に反映済み。項目5: 表示中の計画は動かさず、選択だけ元へ戻す。
                    runOnUiThread { refreshPlanList(); updateReadOnly(); restoreViewingSelection() }
                    edgeExec.execute { startOnEdge(e, id, planJson) }   // 遅いI/Oは専用スレッドで(planExecを塞がない)
                }
            }
        }.start()
    }

    // Phase3c: カメラ未検出(NOCAMERA)時の「継続/中止」ポップアップ。○○=計画カメラの愛称/名称。
    // 継続=即再探索(スマホ=pokeAcquire / エッジ=C_RESEARCH)、見つからなければ猶予後に再表示。
    // 中止=撮影停止。nocamDialogShown で多重表示を抑止する。UIスレッドから呼ぶこと。
    private fun planCameraLabel(id: String): String {
        try {
            if (id == currentPlanId) {
                val cam = JSONObject(HgeNative.nativeGetPlanJson()).optJSONObject("camera")
                val label = listOf(cam?.optString("friendly") ?: "", cam?.optString("name") ?: "", cam?.optString("model") ?: "")
                    .firstOrNull { it.isNotEmpty() }
                if (!label.isNullOrEmpty()) return label
            }
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
    private fun refreshCaptureStatusForCurrent() {
        if (!::captureStatus.isInitialized) return
        val id = currentPlanId
        when {
            id.isEmpty() -> captureStatus.visibility = View.GONE
            disconnectedPlans.contains(id) -> { captureStatus.text = "● カメラが見つかりません"; captureStatus.setTextColor(0xFFD32F2F.toInt()); captureStatus.visibility = View.VISIBLE }
            capturingPlans.contains(id)    -> { captureStatus.text = "● 撮影中"; captureStatus.setTextColor(0xFF2E7D32.toInt()); captureStatus.visibility = View.VISIBLE }
            waitingPlans.contains(id) || startingPlans.contains(id) -> { captureStatus.text = "● 撮影開始待ち"; captureStatus.setTextColor(0xFF1565C0.toInt()); captureStatus.visibility = View.VISIBLE }
            else -> captureStatus.visibility = View.GONE
        }
    }

    private fun beginStop(id: String, doStop: () -> Unit) {
        addHistory("user break", id)   // 項目9: ユーザー操作での中止(この後のIDLEを auto end にしない)
        histUserStop.add(id)
        nocamShownWaiting.remove(id)   // 項目11: 中止したら「待機中1回だけ」をリセット(次の開始要求で再び1回出す)
        stoppingPlans.add(id)
        capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id)   // 即時にアイコンを戻す
        clearNoCam(id)
        if (capturingPlans.isEmpty()) stopBlink()
        if (currentPlanId == id) captureStatus.visibility = View.GONE
        refreshPlanList(); updateReadOnly()
        Thread { runCatching { doStop() } }.start()
        // 保険: 一定時間内に IDLE を検知できなくても抑止/集合を掃除し、UIとポーリングを正常化する。
        handler.postDelayed({
            if (stoppingPlans.remove(id)) {
                capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); clearNoCam(id)
                if (capturingPlans.isEmpty()) stopBlink()
                if (currentPlanId == id) captureStatus.visibility = View.GONE
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
        val dlg = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("カメラが見つかりません")
            .setMessage("${cam}が見つかりません。オンラインにしてください。")
            .setCancelable(false)
            .setPositiveButton("継続") { d, _ ->
                d.dismiss(); nocamDialogs.remove(id)
                // 即再探索(取得フェーズの60秒待ちを前倒し)。ネットワークI/Oは別スレッド。
                Thread { if (e == null) HgeNative.nativePokeAcquire(id) else HgeNative.nativeEdgeResearch(e.ip, e.port, id) }.start()
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
                runOnUiThread { onEdgeStopFailed(id, "エッジ端末「${name}」が見つかりません。") }
                return@beginStop
            }
            runOnUiThread { updateEdgeIp(name, e.ip, e.port) }
            // #4: 停止は撮影を止めるだけ。エッジからは削除せず、計画のエッジ選択も勝手に変えない。
            //  → エッジは計画を保有し続ける=ロック維持。編集したいときは「エッジ端末から削除」(#2)で明示的に外す。
            val r = HgeNative.nativeEdgeStop(e.ip, e.port, id)
            if (r != 0) { runOnUiThread { onEdgeStopFailed(id, "停止をエッジ端末へ送れませんでした (code=${r})。") } }
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
            .setMessage("${why}\nエッジ端末では撮影が続いている可能性があります。端末の電源とネットワークを確認して、もう一度お試しください。")
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
                        .setMessage("エッジ端末「${name}」が見つかりません。\n端末の電源とネットワークを確認して、もう一度お試しください。")
                        .setPositiveButton("OK", null).show()
                    refreshPlanList(); updateReadOnly()   // 楽観的に外したロックを実状態へ戻す
                }
                return@Thread
            }
            runOnUiThread { updateEdgeIp(name, e.ip, e.port) }
            runCatching {
                HgeNative.nativeEdgeStop(e.ip, e.port, id)          // 走っていなければ無害
                HgeNative.nativeEdgeDeletePlan(e.ip, e.port, id)    // エッジから削除
            }
        }.start()
        // 即時ロック解除(保有台帳から外す)+過渡集合の掃除。削除失敗なら次スイープで再出現し自己修復。
        edgeHeldByEdge[name]?.remove(id)
        startingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); stoppingPlans.remove(id); clearNoCam(id)
        nocamShownWaiting.remove(id)   // 項目11: エッジから外したら「待機中1回だけ」もリセット
        refreshPlanList(); updateReadOnly()
        Toast.makeText(this, "エッジ端末から削除しました(編集できます)", Toast.LENGTH_SHORT).show()
    }

    private fun confirmRemoveFromEdge(id: String, name: String) {
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("エッジ端末から削除")
            .setMessage("「$name」をエッジ端末から削除しますか？\n(待機中なら停止し、編集できるようになります。計画自体は残ります)")
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
                past.add(id to o.optString("name"))
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

    // ② 未登録カメラの発見 → 所持カメラへ反映。既存(serial一致)は friendly 更新、未定義の同機種枠は serial+friendly を確定
    //  (どちらも自動)。いずれにも該当しない新規個体は「登録しますか？」を出す(拒否済みserialは自動プロンプトしない)。
    private fun reconcileDiscoveredCameras(presenceJson: String) {
        val arr = try { JSONArray(presenceJson) } catch (_: Exception) { return }
        Thread {
            val toPrompt = ArrayList<Triple<String, String, String>>()   // model, serial, friendly
            for (i in 0 until arr.length()) {
                val c = arr.optJSONObject(i) ?: continue
                if (!c.optBoolean("online")) continue
                val serial = c.optString("serial"); if (serial.isEmpty()) continue
                if (declinedCamSerials.contains(serial)) continue        // 「いいえ」済みは自動では聞かない(手動登録は可)
                val model = c.optString("model"); val friendly = c.optString("friendly")
                val r = try { HgeNative.nativeRecordCameraIdentity(model, serial, friendly, false) } catch (_: Exception) { -1 }
                if (r == 2) { toPrompt.add(Triple(model, serial, friendly)) }   // 2=新規個体(未追加) → 登録可否を問う
            }
            if (toPrompt.isNotEmpty()) runOnUiThread { promptRegisterCameras(toPrompt) }
        }.start()
    }

    // 新規個体ごとに「登録しますか？」ダイアログを出す(どの画面でも表示)。登録=所持へ追加、いいえ=以後自動プロンプト抑止。
    private fun promptRegisterCameras(list: List<Triple<String, String, String>>) {
        for ((model, serial, friendly) in list) {
            if (!promptingCamSerials.add(serial)) continue               // 既に表示中のserialは二重に出さない
            val label = if (friendly.isNotEmpty()) friendly else if (model.isNotEmpty()) model else serial
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle("カメラの登録")
                .setMessage("未登録のカメラ「$label」が見つかりました。所持カメラに登録しますか？")
                .setCancelable(false)
                .setPositiveButton("登録") { _, _ ->
                    promptingCamSerials.remove(serial)
                    Thread {
                        try { HgeNative.nativeRecordCameraIdentity(model, serial, friendly, true) } catch (_: Exception) {}
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
                    // 表示中の計画の進捗のみステータス行に出す。
                    if (o.optString("planId") == currentPlanId && currentPlanId.isNotEmpty() && !disconnectedPlans.contains(currentPlanId)) {
                        captureStatus.text = "● 撮影中  ${o.optInt("frame")}/${o.optInt("total")}枚  残り${o.optInt("remainSec")}秒"
                        captureStatus.setTextColor(0xFF2E7D32.toInt()); captureStatus.visibility = View.VISIBLE
                    }
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
                    val msg = o.optString("msg")
                    capState.text = "ERROR $msg"
                    // カメラ未検出・カメラ使用中など、撮影開始の失敗をユーザーへ通知する。
                    if (msg.isNotEmpty()) Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                }
            }
    }

    // スケジュールJSON(静的フィールド+events+windows)から両画面の表示を更新する。
    private fun updatePlanDisplay(json: String) {
        try {
            val o = JSONObject(json)
            capName.text = o.optString("name")
            // 選択中計画の開始/終了をピッカー用カレンダーへ同期(計画切替時に時刻表示を追従)。
            try {
                fmtIso.parse(o.optString("start"))?.let { startCal.time = it }
                fmtIso.parse(o.optString("end"))?.let { endCal.time = it }
                updateTimeButtons()
            } catch (_: Exception) {}
            placeText.text = o.optString("place")
            latlngText.text = o.optString("latlng") + "  標高 " + o.optInt("altitude") + "m"
            cameraText.text = "カメラ: " + o.optString("camera")
            lensText.text = "レンズ: " + o.optString("lens")
            // センサー/焦点距離/画角は撮影シミュレーション画面へ移した(2026-08-08 UI依頼)。
            simGearText = "センサー %.1f×%.1fmm  焦点距離 %d mm  画角 %.0f×%.0f°".format(
                o.optDouble("sensorW"), o.optDouble("sensorH"), o.optInt("focalLength"),
                o.optDouble("fovH"), o.optDouble("fovV"))
            simPage?.setGearText(simGearText)
            intervalText.text = fmtInterval(o.optDouble("interval", 15.0)) + "秒"
            suppressLandscape = true
            landscapeCheck.isChecked = o.optBoolean("landscape")
            suppressLandscape = false
            npfText.text = "NPF %.1f秒   最小周期 %d秒".format(o.optDouble("npf"), o.optInt("minInterval"))
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

    // --- 撮影場所の入力(緯度経度)。登録済みから選択 / テキスト貼り付け / 地図から選択(osmdroid) ---
    private fun showPlaceEditChooser() {
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
                Thread {
                    val r = HgeNative.nativeSetPlanPlace(nm)
                    val sched = HgeNative.nativeScheduleJson()
                    runOnUiThread {
                        if (r == 0) { latestSchedule = sched; updatePlanDisplay(sched); Toast.makeText(this, "撮影場所: $nm", Toast.LENGTH_SHORT).show() }
                        else Toast.makeText(this, "撮影場所の設定に失敗 (code=$r)", Toast.LENGTH_LONG).show()
                    }
                }.start()
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
    private fun applyPlace(lat: Double, lng: Double, name: String) {
        Thread {
            val r = HgeNative.nativeSetPlanLocation(lat, lng, name)
            val sched = HgeNative.nativeScheduleJson()
            runOnUiThread {
                if (r == 0) { latestSchedule = sched; updatePlanDisplay(sched)
                    Toast.makeText(this, "撮影場所を更新: %.4f, %.4f".format(lat, lng), Toast.LENGTH_SHORT).show() }
                else Toast.makeText(this, "撮影場所の設定に失敗 (code=$r)", Toast.LENGTH_LONG).show()
            }
        }.start()
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
            addView(map, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(420)))
        }
        val dlg = androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("地図から選択")
            .setView(box)
            .setPositiveButton("この地点に設定") { _, _ -> onPick(picked.latitude, picked.longitude) }
            .setNegativeButton("キャンセル", null)
            .create()
        dlg.setOnDismissListener { map.onPause(); map.onDetach() }
        dlg.show()
        map.onResume()
    }

    // ============================================================
    //  640 撮影場所リスト(§7.9)。登録した場所を撮影計画で選択する。
    // ============================================================
    private var selPlace: String? = null
    private var placeLat = 0.0
    private var placeLng = 0.0
    private var placeCoordTv: TextView? = null
    private var placeAltEt: EditText? = null
    private var placeMemoEt: EditText? = null
    private var placeAutoCb: CheckBox? = null

    private fun placeArray(json: String): JSONArray = try { JSONArray(json) } catch (e: Exception) { JSONArray() }
    private fun placeNames(): List<String> { val a = placeArray(HgeNative.nativeGetPlaces()); return (0 until a.length()).mapNotNull { a.optJSONObject(it)?.optString("name") } }

    private fun openPlacesList() {
        buildPlacesList(); buildPlaceDetail()
        setInitialSplit(R.id.places_listScroll, R.id.places_container)
        flipper.displayedChild = 11
    }
    private fun leavePlacesList() { stopDirtyWatch(); persistPlaceDetail(false); flipper.displayedChild = 4; buildGearMenu() }

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
                val friendly = o.optString("camFriendly")
                if (model.isEmpty() && serial.isEmpty()) continue   // カメラ未指定の計画は対象外
                // 同一機体の判定: シリアルがあればそれが最優先(機種違いの同シリアル衝突は model と併用)。
                val key = if (serial.isNotEmpty()) "$model#$serial" else model
                val label = buildString {
                    append(model.ifEmpty { "(カメラ未設定)" })
                    if (friendly.isNotEmpty()) append("  $friendly")
                    if (serial.isNotEmpty()) append("  Sn:$serial")
                }
                list.add(Reservation(id, o.optString("name"), key, label, model, serial, planEdgeName(id), s, e))
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
                plan = o.optString("name")
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
        setInitialSplit(R.id.report_listScroll, R.id.report_container)
        flipper.displayedChild = 14
    }

    // 所見コード → 表示文言(dataManager::noteCode と対応)。
    private fun reportNoteText(code: Int): String = when (code) {
        1 -> "露出設定に失敗したコマがあります。カメラの露出がアプリの意図とズレている可能性があります。"
        2 -> "ライブビューの更新が撮影周期に追いついていません。撮影周期を長くすると安定します。"
        3 -> "撮影周期を守れないコマが多いです。シャッター速度に対して周期が短い可能性があります。"
        4 -> "測光できないコマが多いです。露出が据え置かれるため明るさが追従しません。"
        5 -> "カメラの記録が明けないまま準備開始に至ったコマがあります。周期に対して記録が遅いか、ライブビューが止まっています。"
        6 -> "撮影周期に余裕がありません。目安の最短周期を下回ると、遅れやコマ落ちが出ます。"
        7 -> "撮影周期にはまだ余裕があります。周期を短くすると滑らかな微速度になります。"
        8 -> "busy を1コマも測れませんでした。周期が露光でほぼ埋まっています。"
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
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }
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
            val menu: List<Pair<String, () -> Unit>> = listOf("削除" to { confirmDeleteReport(name) })
            box.addView(listRow(o.optString("shotAt").ifEmpty { name }, sub, name == selectedReport,
                { selectedReport = name; buildReportList(); buildReportDetail() }, menu))
            box.addView(thinDivider())
        }
    }

    // エッジに溜まった撮影レポートを引き取る(edgeSweep のワーカースレッドから呼ぶ)。
    // 取得 → レポートとして読めることを確認 → スマホへ保存 → そこで初めてエッジへ削除を指示する。
    // 取得しただけで消すと、保存に失敗したぶんが永久に失われる。削除まで届かなかったものは
    // 次のスイープでまた拾う(同名で上書きするだけなので重複しない)。
    private fun collectEdgeReports(edge: Edge) {
        val arr = try { JSONArray(HgeNative.nativeEdgeReportList(edge.ip, edge.port)) } catch (_: Exception) { JSONArray() }
        val dir = java.io.File(getExternalFilesDir(null), "log")
        if (!dir.exists() && !dir.mkdirs()) return
        var got = 0
        for (i in 0 until arr.length()) {
            val name = arr.optJSONObject(i)?.optString("name").orEmpty()
            if (!name.startsWith("report_") || !name.endsWith(".json")) continue
            val body = try { HgeNative.nativeEdgeReportRead(edge.ip, edge.port, name) } catch (_: Exception) { "" }
            if (body.isEmpty()) continue
            // 途中で切れたものを保存して消させないため、中身を検査してから書く。
            val o = try { JSONObject(body) } catch (_: Exception) { null } ?: continue
            if (!o.has("capture")) continue
            o.put("edge", edge.name)   // どの端末で撮ったかを一覧と内容に出せるようにする
            try { java.io.File(dir, name).writeText(o.toString()) } catch (_: Exception) { continue }
            try { HgeNative.nativeEdgeReportDelete(edge.ip, edge.port, name) } catch (_: Exception) {}
            got++
        }
        if (got > 0) runOnUiThread {
            Toast.makeText(this, "エッジ「${edge.name}」の撮影レポート ${got}件を取得しました", Toast.LENGTH_SHORT).show()
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
        repRow(box, "測光リトライ", "${exp.optInt("meterRetryFrames")} コマ")
        repRow(box, "露出設定リトライ", "${exp.optInt("applyRetryFrames")} コマ")

        repBand(box, "撮影周期")
        repRow(box, "設定 / 実測", "%.1f / %.2f 秒".format(itv.optDouble("setSec"), itv.optDouble("actualSec")))
        repRow(box, "周期どおり", "%d / %d (%.1f%%)".format(tim.optInt("lateOk"), tim.optInt("lateCnt"), tim.optDouble("lateOkPct")),
               "遅れ100ms以内")
        repRow(box, "遅れ 平均/最大", "%.0f / %d ms".format(tim.optDouble("lateAvgMs"), tim.optInt("lateMaxMs")))
        repRow(box, "準備 平均/最大", "%.0f / %d ms".format(tim.optDouble("prepAvgMs"), tim.optInt("prepMaxMs")),
               "測光→露出計算→露出設定")
        repRow(box, "準備が間に合わず", "${tim.optInt("prepOver")} コマ",
               "リード %.1f 秒以内に終える必要がある".format(tim.optInt("leadMs") / 1000.0))

        // ここが「撮影周期をどこまでシャッター速度へ近づけられるか」の答えになる部分。
        repBand(box, "カメラの busy と周期の余裕")
        val busyCnt = bsy.optInt("cnt")
        if (busyCnt > 0) {
            repRow(box, "露光終了→測光可", "平均 %.2f / 最大 %.2f 秒".format(bsy.optDouble("avgMs") / 1000.0, bsy.optInt("maxMs") / 1000.0),
                   "${busyCnt}コマで実測。カメラが記録で塞がっている時間")
        } else {
            repRow(box, "露光終了→測光可", "計測なし", "周期が露光で埋まっていて測る空きが無かった")
        }
        if (bsy.optInt("stuck") > 0) { repRow(box, "明けなかったコマ", "${bsy.optInt("stuck")} コマ") }
        repRow(box, "測光の所要", "平均 %.2f / 最大 %.2f 秒".format(met.optDouble("avgMs") / 1000.0, met.optInt("maxMs") / 1000.0))
        repRow(box, "露出設定の所要", "平均 %.2f / 最大 %.2f 秒".format(apl.optDouble("avgMs") / 1000.0, apl.optInt("maxMs") / 1000.0))
        repRow(box, "この撮影の最長ss", "%.2f 秒".format(lim.optDouble("maxSsSec")))
        val minItv = lim.optDouble("minIntervalSec", -1.0)
        if (minItv >= 0.0) {
            repRow(box, "目安の最短周期", "%.1f 秒".format(minItv),
                   "最長ss + busy最大 + 準備最大 + 余裕1秒。設定周期との差 %.1f 秒".format(lim.optDouble("marginSec")))
        }

        // 撮影開始前の露出合わせ(初期収束)がうまくいったか。ここが済んでいないと1枚目から露出が外れる。
        repBand(box, "撮影前の露出合わせ")
        val cvw = o.optJSONObject("converge") ?: JSONObject()
        val cvOutcome = cvw.optInt("outcome", 2)
        repRow(box, "結果", when (cvOutcome) {
            0 -> "合わせられた"
            1 -> "合わせきれず開始"
            else -> "測光できず開始"
        }, when (cvOutcome) {
            0 -> "目標の範囲に入った(または露出限界に到達した)"
            1 -> "時間内に目標へ届かず、その時点の最良推定で撮り始めた"
            else -> "一度も測光できず、撮影制御方法の基準値のまま撮り始めた"
        })
        repRow(box, "測れた回数", "${cvw.optInt("steps")} 回")
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

    private fun buildPlacesList() {
        val box = findViewById<LinearLayout>(R.id.places_container)
        box.removeAllViews()
        val arr = placeArray(HgeNative.nativeGetPlaces())
        val names = (0 until arr.length()).mapNotNull { arr.optJSONObject(it)?.optString("name") }
        if (selPlace == null || selPlace !in names) selPlace = names.firstOrNull()
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val name = o.optString("name")
            val sub = "%.4f, %.4f  標高 %dm".format(o.optDouble("latitude", 0.0), o.optDouble("longitude", 0.0), o.optDouble("altitude", 0.0).toInt()) +
                      (if (o.optString("memo").isNotEmpty()) "  ${o.optString("memo")}" else "")
            box.addView(listRow(name, sub, name == selPlace,
                onSelect = { selectPlace(name) },
                menuItems = listOf("削除" to {
                    Thread { HgeNative.nativeRemovePlace(name)
                        runOnUiThread { if (selPlace == name) selPlace = null; buildPlacesList(); buildPlaceDetail() } }.start()
                }),
                onRename = { newName -> commitPlaceRename(name, newName) }))
            box.addView(thinDivider())
        }
        box.addView(linkText("＋ 新しい場所の追加") { addPlace() })
    }

    private fun addPlace() {
        Thread {
            HgeNative.nativeAddPlace("")
            val names = placeNames()
            runOnUiThread { selPlace = names.lastOrNull(); buildPlacesList(); buildPlaceDetail() }
        }.start()
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
            val la0 = if (placeLat != 0.0 || placeLng != 0.0) placeLat else 35.681
            val lo0 = if (placeLat != 0.0 || placeLng != 0.0) placeLng else 139.767
            openMapPicker(la0, lo0) { la, lo -> onPlaceCoord(la, lo) }
        })
        btnRow.addView(linkText("✎ 貼り付け") { showPlacePasteDialog("%.6f, %.6f".format(placeLat, placeLng)) { la, lo -> onPlaceCoord(la, lo) } })
        btnRow.addView(linkText("＋ 現在地") { fetchCurrentLocation { la, lo, alt -> if (alt != 0.0) placeAltEt?.setText(alt.toInt().toString()); onPlaceCoord(la, lo) } })
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
            put("altitude", alt); put("autoInsert", auto)
        }.toString()
        Thread {
            HgeNative.nativeSetPlaceDetail(key, json)
            if (rebuild) runOnUiThread { buildPlacesList(); buildPlaceDetail() }
            else if (rebuildList) runOnUiThread { buildPlacesList() }   // 座標だけ更新(詳細の入力欄は保持)
        }.start()
    }

    // 現在地(GPS/ネットワーク)を取得して onGot(lat,lng,alt) を呼ぶ(§7.9)。権限が無ければ要求。
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
        val avail = resources.displayMetrics.widthPixels - dp(24)     // フォーム左右パディング分
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
        tv.setPadding(dp(12), dp(5), dp(8), dp(5))
        return tv
    }

    // 先頭ページ: 概要スケジュール(表示専用)。時刻とイベント(Start/End/日の出/日の入/月の出/月の入)を
    // 日付ごとに時系列表示し、各薄明ページへ移動できるボックスを差し込む。
    // 薄明が2つ以上なら縦2列に分割(日付境で分割。同一日付の朝夕のみのときは昼間=日の入で分割)。1つなら1列。
    private fun renderOverview(o: JSONObject) {
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
        data class Nav(val page: Int, val morning: Boolean)
        val navs = ArrayList<Nav>()
        o.optJSONArray("blocks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val b = arr.getJSONObject(i)
                navs.add(Nav(i + 1, b.optString("axis") != "down"))
            }
        }
        // 移動ボックスの差し込み位置: 朝=日の出(code9)の前、夕方=日の入(code2)の後。
        // 薄明ブロック・日の出/日の入は共に時系列。日付で対応付けると同日内の朝の日付が
        // ずれて誤配置になるため、順番(i番目の朝→i番目の日の出)で対応付ける。
        // 対応するイベントが無ければ End(code12)の前(例: プランがEndで切れる夕方薄明)。
        val before = HashMap<Int, MutableList<Nav>>()
        val after = HashMap<Int, MutableList<Nav>>()
        val endIdx = evs.indexOfLast { it.code == 12 }
        val sunriseIdx = evs.indices.filter { evs[it].code == 9 }   // 日の出(時系列)
        val sunsetIdx = evs.indices.filter { evs[it].code == 2 }    // 日の入(時系列)
        var mi = 0; var si = 0
        for (nv in navs) {
            if (nv.morning) {
                val at = sunriseIdx.getOrNull(mi++) ?: endIdx
                if (at >= 0) before.getOrPut(at) { mutableListOf() }.add(nv)
            } else {
                val at = sunsetIdx.getOrNull(si++)
                if (at != null) after.getOrPut(at) { mutableListOf() }.add(nv)
                else if (endIdx >= 0) before.getOrPut(endIdx) { mutableListOf() }.add(nv)
            }
        }

        // 縦2列にする分割位置 k(events[k] 以降が右列)。薄明が2つ以上のときだけ2列。
        var k = 0
        if (navs.size >= 2) {
            val dateSplit = evs.indexOfFirst { it.md != evs.first().md }  // 日付が変わる位置
            k = when {
                dateSplit > 0 -> dateSplit                                // 日付境で分割
                else -> evs.indexOfFirst { it.code == 2 }                 // 同一日付 → 昼間(日の入)で分割
            }
        }
        val twoCol = k in 1 until evs.size

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

        var cd1 = ""; var cd2 = ""
        for ((idx, ev) in evs.withIndex()) {
            val right = twoCol && idx >= k
            val col = if (right) col2 else col1
            if (right) { if (ev.md != cd2) { cd2 = ev.md; col.addView(makeDateBadge(ev.md)) } }
            else { if (ev.md != cd1) { cd1 = ev.md; col.addView(makeDateBadge(ev.md)) } }
            before[idx]?.forEach { col.addView(makeTwilightBox(it.page, it.morning)) }
            col.addView(makeEventRow(ev.time, ev.label))
            after[idx]?.forEach { col.addView(makeTwilightBox(it.page, it.morning)) }
        }
        equalizeTwilightBoxes(twoCol)   // 全ボックスの幅・高さを揃える(言語非依存)
    }

    // §7.3.2 薄明ページ(横スライド)を再構築する。blocks[] の各要素=1ページ(ScheduleView)。
    // ページ0(フォーム)は残し、以前の薄明ページを差し替える。ページ番号(タイトル)も更新。
    private fun rebuildTwilightPages(o: JSONObject) {
        curSunriseMode = o.optInt("sunriseMode", 0)
        curSunsetMode = o.optInt("sunsetMode", 0)
        for (p in twilightPages) planPager.removeView(p)
        twilightPages.clear()
        schedulePages.clear()
        val ed = !planReadOnly
        val planName = o.optString("name")
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
        findViewById<TextView>(R.id.plan_title).text = "撮影計画  ${cur + 1}/$n"
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
        intArrayOf(R.id.plan_startDate, R.id.plan_startTime, R.id.plan_endTime,
            R.id.plan_gearConst,
            R.id.plan_edgeSearchButton, R.id.searchButton, R.id.connectButton)
            .forEach { findViewById<View>(it).isEnabled = ed }
        intervalText.isEnabled = ed; landscapeCheck.isEnabled = ed
        cameraText.isEnabled = ed; lensText.isEnabled = ed; edgeSpinner.isEnabled = ed
        schedulePages.forEach { it.isEnabled = ed }   // 項目11: compass/elevationView は廃止
        findViewById<LinearLayout>(R.id.plan_ccmButtons).let { for (i in 0 until it.childCount) it.getChildAt(i).isEnabled = ed }
    }

    // 夕日/朝日の帯を挿入(insert=true)/排除(insert=false)。他方の帯モードは保持する。
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
    private fun editGearConst() {
        val o = try { JSONObject(latestSchedule) } catch (_: Exception) { JSONObject() }
        val box = LinearLayout(this); box.orientation = LinearLayout.VERTICAL; box.setPadding(dp(16), dp(8), dp(16), dp(8))
        fun row(label: String, value: String): EditText {
            val t = TextView(this); t.text = label; t.textSize = 12f; box.addView(t)
            val e = EditText(this); e.inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL
            e.setText(value); box.addView(e); return e
        }
        val eW = row("センサー横[mm]", o.optDouble("sensorW").toString())
        val eH = row("センサー縦[mm]", o.optDouble("sensorH").toString())
        val eP = row("センサー横[pixel]", o.optInt("pixelW").toString())
        val eF = row("焦点距離[mm]", o.optInt("focalLength").toString())
        val eN = row("開放F値", o.optDouble("fn").toString())
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("センサー/レンズ定数")
            .setView(box)
            .setPositiveButton("適用") { _, _ ->
                val j = JSONObject()
                eW.text.toString().toDoubleOrNull()?.let { j.put("sensorW", it) }
                eH.text.toString().toDoubleOrNull()?.let { j.put("sensorH", it) }
                eP.text.toString().toIntOrNull()?.let { j.put("pixelW", it) }
                eF.text.toString().toDoubleOrNull()?.let { j.put("focalLength", it) }
                eN.text.toString().toDoubleOrNull()?.let { j.put("fn", it) }
                planExec.execute { HgeNative.nativeSetPlanGearConst(j.toString()) }
            }
            .setNegativeButton("キャンセル", null)
            .show()
    }

    // --- エッジ端末 ---
    private fun selectedEdge(): Edge? {
        val i = edgeSpinner.selectedItemPosition
        return if (i in 1..edges.size) edges[i - 1] else null
    }

    // --- エッジ端末の登録(prefsに永続化。設定で追加・検索で自動登録。オフラインでも選択可) ---
    private fun hgcPrefs() = getSharedPreferences("hgc", MODE_PRIVATE)
    private fun loadRegisteredEdges() {
        edges.clear()
        try {
            val a = JSONArray(hgcPrefs().getString("regEdges", "[]") ?: "[]")
            for (i in 0 until a.length()) {
                val o = a.getJSONObject(i)
                edges.add(Edge(o.optString("name", "エッジ端末"), o.optString("ip"), o.optInt("port", 50506)))
            }
        } catch (_: Exception) {}
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
        selectedEdgeName = ""
        scannedPop = ""; scannedName = ""
        buildEdgeList(); buildEdgeForm()
        setInitialSplit(R.id.edge_listScroll, R.id.edge_container)
        flipper.displayedChild = 15
    }

    // 上段: 登録済み一覧。先頭は新規追加行。タップで設定対象を切り替える。
    private fun buildEdgeList() {
        val box = findViewById<LinearLayout>(R.id.edge_container)
        box.removeAllViews()
        val d = resources.displayMetrics.density
        fun row(label: String, sel: Boolean, bold: Boolean, onTap: () -> Unit, onDelete: (() -> Unit)?) {
            val r = LinearLayout(this); r.orientation = LinearLayout.HORIZONTAL
            r.gravity = Gravity.CENTER_VERTICAL
            if (sel) r.setBackgroundColor(0xFFE3F2FD.toInt())
            val tv = TextView(this).apply {
                text = label
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                setPadding((8 * d).toInt(), (10 * d).toInt(), (8 * d).toInt(), (10 * d).toInt())
                if (bold) setTypeface(null, Typeface.BOLD)
            }
            tv.setOnClickListener { onTap() }
            r.addView(tv)
            if (onDelete != null) {
                r.addView(Button(this).apply { text = "削除"; setOnClickListener { onDelete() } })
            }
            box.addView(r)
        }
        // 新規追加(常に先頭。撮影計画の「新規撮影計画」行と同じ位置づけ)
        row("＋ 新規エッジ端末", selectedEdgeName.isEmpty(), true, {
            selectedEdgeName = ""; scannedPop = ""; scannedName = ""
            buildEdgeList(); buildEdgeForm()
        }, null)
        if (edges.isEmpty()) {
            box.addView(TextView(this).apply {
                text = "(登録なし。上の「＋ 新規エッジ端末」から追加)"
                setPadding((8 * d).toInt(), (6 * d).toInt(), 0, 0)
            })
            return
        }
        edges.toList().forEach { e ->
            val sel = e.name == selectedEdgeName
            row((if (sel) "● " else "○ ") + e.name, sel, sel, {
                selectedEdgeName = e.name
                scannedPop = ""; scannedName = ""
                buildEdgeList(); buildEdgeForm()
            }, {
                AlertDialog.Builder(this)
                    .setTitle("エッジ端末の削除")
                    .setMessage("「" + e.name + "」を登録から削除しますか？(エッジ本体の設定は変わりません)")
                    .setPositiveButton("削除する") { _, _ ->
                        edges.remove(e); saveRegisteredEdges(); refreshEdgeSpinner()
                        if (selectedEdgeName == e.name) selectedEdgeName = ""
                        buildEdgeList(); buildEdgeForm()
                    }
                    .setNegativeButton("やめる", null)
                    .show()
            })
        }
    }

    // 下段: 選択した端末(または新規)の設定フォーム。
    private fun buildEdgeForm() {
        val ctx = this
        val d = resources.displayMetrics.density
        val box = findViewById<LinearLayout>(R.id.edge_form)
        box.removeAllViews()

        fun label(t: String): TextView {
            val v = TextView(ctx).apply { text = t; setPadding(0, (10 * d).toInt(), 0, (2 * d).toInt()) }
            box.addView(v); return v
        }

        box.addView(TextView(ctx).apply {
            text = if (selectedEdgeName.isEmpty()) "新規エッジ端末の登録・設定" else "「" + selectedEdgeName + "」の設定"
            setTypeface(null, Typeface.BOLD)
            textSize = 16f
        })

        label("端末識別名 (半角英数字。エッジのLCDに表示)")
        val nameE = EditText(ctx); applyEdgeNameInput(nameE)
        nameE.setText(if (selectedEdgeName.isNotEmpty()) selectedEdgeName else scannedName)
        box.addView(nameE); edgeNameEt = nameE

        // ネットワークモード。ONでエッジ自身がAP(屋外・ルーター無し)、OFFで既存ネットへ参加。
        val apSwitch = android.widget.Switch(ctx).apply {
            text = "エッジをAPにする (OFF=既存ネットに接続)"
            isChecked = edgeApMode
            setPadding(0, (12 * d).toInt(), 0, 0)
        }
        box.addView(apSwitch)

        // SSID/password。APモードでは「エッジが立てるAPの資格」、STAでは「参加先の資格」。
        // 2026-08-08 UI依頼: どちらを編集しているのかが分かるようラベルを出し分ける。
        val ssidLabel = label("接続先 SSID")
        val ssidE = EditText(ctx); box.addView(ssidE)
        val ssidPickBtn = Button(ctx).apply { text = "SSIDを選択(周辺のWi-Fiから)" }
        box.addView(ssidPickBtn)
        val passLabel = label("接続先 password")
        val passE = EditText(ctx).apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD }
        box.addView(passE)
        edgeSsidEt = ssidE; edgePassEt = passE; edgeSsidLabel = ssidLabel; edgePassLabel = passLabel

        fun applyModeLabels(ap: Boolean) {
            ssidLabel.text = if (ap) "AP SSID (空ならエッジの既定値)" else "接続先 SSID"
            passLabel.text = if (ap) "AP password (空ならエッジの既定値)" else "接続先 password"
            // APモードでは周辺Wi-Fiから選ぶ意味が無い(自分で立てる側なので)。
            ssidPickBtn.isEnabled = !ap
            ssidPickBtn.alpha = if (ap) 0.4f else 1f
        }
        applyModeLabels(edgeApMode)
        apSwitch.setOnCheckedChangeListener { _, ap -> edgeApMode = ap; applyModeLabels(ap) }
        edgeApSwitch = apSwitch
        ssidPickBtn.setOnClickListener { pickWifiSsid { sid -> ssidE.setText(sid) } }

        val popView = TextView(ctx).apply {
            setPadding(0, (12 * d).toInt(), 0, 0)
            text = if (scannedPop.isEmpty()) "PoP: 未取得 — QRをスキャンしてください" else "PoP: " + scannedPop
        }
        box.addView(popView); edgePopView = popView

        box.addView(Button(ctx).apply {
            text = "エッジのQRをスキャン"
            setOnClickListener { requestEdgeQr() }
        })
        box.addView(Button(ctx).apply {
            text = "設定を送信"
            setOnClickListener { sendEdgeProvision() }
        })

        // 新規は「登録だけ」もできる(エッジが手元に無くても計画で選べるようにするため)。
        box.addView(Button(ctx).apply {
            text = "登録だけする(エッジへ送信しない)"
            setOnClickListener {
                val nm = nameE.text.toString().trim()
                if (nm.isEmpty()) { Toast.makeText(ctx, "端末識別名を入力してください", Toast.LENGTH_SHORT).show(); return@setOnClickListener }
                if (!isAsciiEdgeName(nm)) { Toast.makeText(ctx, "端末識別名は半角英数字で入力してください", Toast.LENGTH_LONG).show(); return@setOnClickListener }
                if (edges.none { it.name == nm }) edges.add(Edge(nm, "", 50506))
                saveRegisteredEdges(); refreshEdgeSpinner()
                selectedEdgeName = nm
                buildEdgeList(); buildEdgeForm()
                Toast.makeText(ctx, "「" + nm + "」を登録しました", Toast.LENGTH_SHORT).show()
            }
        })
    }

    // BLEでエッジに start を送ってQRを表示させ、続けてカメラでスキャンする。
    private fun requestEdgeQr() {
        val ctx = this
        ensureBlePermissions {
            edgePopView?.text = "エッジにQR表示を要求中(BLE)..."
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
                    if (scannedName.isNotEmpty() && edgeNameEt?.text.isNullOrEmpty()) edgeNameEt?.setText(scannedName)
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
            Toast.makeText(ctx, "端末識別名は半角英数字で入力してください(エッジ端末で日本語は表示できません)", Toast.LENGTH_LONG).show(); return
        }
        val ssid = edgeSsidEt?.text?.toString() ?: ""
        val pass = edgePassEt?.text?.toString() ?: ""
        val mode = if (edgeApMode) "ap" else "sta"
        // STAのSSIDは「新規登録のとき」だけ必須。登録済みなら空=端末名だけ変更(エッジは接続先を保つ)。
        // APは空ならエッジ側が既定値(HGC-Edge-<MAC下2桁> / 8桁乱数)を用意する。
        if (mode == "sta" && ssid.isEmpty() && selectedEdgeName.isEmpty()) {
            Toast.makeText(ctx, "SSIDを入力してください(新規登録は接続先が必要です)", Toast.LENGTH_LONG).show(); return
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
    // 計画ごとのエッジ選択(prefsに端末"名称"を保存。空=無し=スマホで撮影)。
    // IPはDHCPで変わり事前に不定のため、識別は端末名称で行い、IPは開始時に検索で解決する。
    private fun planEdgeName(planId: String) = if (planId.isEmpty()) "" else (hgcPrefs().getString("pe_$planId", "") ?: "")
    private fun setPlanEdgeName(planId: String, name: String) {
        if (planId.isEmpty()) return
        if (planEdgeName(planId) == name) return   // 変化なしは書かない(誤書き込みの影響を最小化・prefs churn抑制)
        hgcPrefs().edit().putString("pe_$planId", name).apply()
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
                    if (o.optString("name") == name) return Edge(name, o.optString("ip"), o.optInt("port", 50506))
                }
            } catch (_: Exception) {}
        }
        return null   // ブロードキャストで同名端末が見つからない=本当に居ない(キャッシュIPは流用しない)
    }
    // 登録一覧の last-seen IP を更新(stop/poll 用に開始時の解決結果を保持)。
    // 常時スイープからも30秒ごとに呼ばれるため、変化が無ければ prefs へ書かない。
    private fun updateEdgeIp(name: String, ip: String, port: Int) {
        val i = edges.indexOfFirst { it.name == name }
        if (i >= 0) {
            if (edges[i].ip == ip && edges[i].port == port) return   // 変化なし
            edges[i] = Edge(name, ip, port)
        } else edges.add(Edge(name, ip, port))
        saveRegisteredEdges()
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
                  else edges.indexOfFirst { it.name == stored }.let { if (it >= 0) it + 1 else 0 }
        if (edgeSpinner.selectedItemPosition != idx) { try { edgeSpinner.setSelection(idx) } catch (_: Exception) {} }
        if (chosen != null && chosen != stored) {
            Toast.makeText(this, "エッジ端末を変更できませんでした。もう一度選んでください", Toast.LENGTH_SHORT).show()
        }
    }

    private fun refreshEdgeSpinner() {
        val labels = mutableListOf("無し (スマホで撮影)")
        // IPは動的なので名称のみ表示。常時スイープの生存状態を ●=オンライン/○=オフライン で付す(不明=無印)。
        edges.forEach {
            val mark = when (edgeOnline[it.name]) { true -> "● "; false -> "○ "; null -> "" }
            labels.add(mark + it.name)
        }
        val name = planEdgeName(currentPlanId)
        val idx = if (name.isEmpty()) 0 else edges.indexOfFirst { it.name == name }.let { if (it >= 0) it + 1 else 0 }
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
    private fun onEdgesFound(jsonArray: String) {
        try {
            val arr = JSONArray(jsonArray)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val nm = o.optString("name"); if (nm.isEmpty()) continue
                val ip = o.optString("ip"); val port = o.optInt("port", 50506)
                val idx = edges.indexOfFirst { it.name == nm }
                if (idx >= 0) edges[idx] = Edge(nm, ip, port) else edges.add(Edge(nm, ip, port))
            }
        } catch (_: Exception) {}
        saveRegisteredEdges()
        refreshEdgeSpinner()
    }

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
            // ① エッジ書き戻し: エッジが接続確定したカメラの serial/friendly を所持カメラへ反映する
            //  (エッジ撮影ではスマホがカメラに接続しないため、この経路が唯一の識別情報伝播)。serial単位で1回だけ適用。
            val cSerial = o.optString("serial")
            if (cSerial.isNotEmpty() && edgeAppliedSerials.add(cSerial)) {
                val cModel = o.optString("model"); val cFriendly = o.optString("friendly")
                Thread { try { HgeNative.nativeRecordCameraIdentity(cModel, cSerial, cFriendly, true) } catch (_: Exception) {} }.start()
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
                    if (currentPlanId == pid) captureStatus.visibility = View.GONE
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
                    if (currentPlanId == pid) { captureStatus.text = "● カメラが見つかりません(エッジ)"; captureStatus.setTextColor(0xFFD32F2F.toInt()); captureStatus.visibility = View.VISIBLE }
                    showNoCameraDialog(pid)   // Phase3c: 継続/中止ポップアップ(多重抑止あり)
                }
                HgeNative.ST_WAITING, HgeNative.ST_SEARCHING -> {
                    if (waitingPlans.add(pid)) changed = true
                    if (capturingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)   // 未検出→待機へ復帰: 表示中のNOCAMERAダイアログを閉じる
                    if (currentPlanId == pid) { captureStatus.text = "● 撮影開始待ち(エッジ)"; captureStatus.setTextColor(0xFF1565C0.toInt()); captureStatus.visibility = View.VISIBLE }
                }
                HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                    if (capturingPlans.add(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)   // 未検出→撮影へ復帰: 表示中のNOCAMERAダイアログを閉じる
                    if (currentPlanId == pid) { captureStatus.text = "● エッジ撮影中  ${o.optInt("frame")}/${o.optInt("total")}枚  残り${o.optInt("remainSec")}秒"; captureStatus.setTextColor(0xFF2E7D32.toInt()); captureStatus.visibility = View.VISIBLE }
                }
                HgeNative.ST_IDLE, HgeNative.ST_ERROR -> {   // この計画は終了 → 各集合から除去
                    if (capturingPlans.remove(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    clearNoCam(pid)
                    if (currentPlanId == pid) captureStatus.visibility = View.GONE
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
                if (e.ip.isEmpty()) continue   // IP未解決(名称のみ)はスキップ
                Thread {
                    val pj = HgeNative.nativeEdgeProgress(e.ip, e.port, pid)   // 計画別: この計画idの状態だけを取る(1エッジ複数カメラの誤検出防止)
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
    private val edgeSweep = object : Runnable {
        override fun run() {
            updateEdgeApBinding()   // エッジSoftAP接続中はそのNICへバインド維持(Androidの自動離脱を防ぐ)
            Thread {
                val js = try { HgeNative.nativeEdgeSearch(2000) } catch (_: Exception) { "[]" }
                // name → (edge, sessionsフィールド有無, sessions{planId→state}, heldPlansフィールド有無, 保有ロスター)
                data class Found(val edge: Edge, val hasSessions: Boolean, val sessions: Map<String, Int>,
                                 val hasHeld: Boolean, val heldPlans: Set<String>, val reports: Int)
                val found = HashMap<String, Found>()
                try {
                    val arr = JSONArray(js)
                    for (i in 0 until arr.length()) {
                        val o = arr.optJSONObject(i) ?: continue
                        val nm = o.optString("name"); if (nm.isEmpty()) continue
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
                                          o.optInt("reports", 0))
                    }
                } catch (_: Exception) {}
                // UDP無応答の登録エッジ: 連続2回でTCP生存確認(取りこぼし救済)→それも不応答ならオフライン。
                val tcpOnline = ArrayList<String>(); val offline = ArrayList<String>()
                for (ed in edges.toList()) {
                    if (found.containsKey(ed.name)) continue
                    val miss = (edgeMiss[ed.name] ?: 0) + 1
                    edgeMiss[ed.name] = miss
                    if (miss < 2) continue
                    val alive = ed.ip.isNotEmpty() &&
                        (try { HgeNative.nativeEdgeProgress(ed.ip, ed.port, "") } catch (_: Exception) { "" }).isNotEmpty()
                    if (alive) tcpOnline.add(ed.name) else offline.add(ed.name)
                }
                runOnUiThread {
                    var uiDirty = false
                    for ((nm, f) in found) {
                        edgeMiss[nm] = 0
                        if (edgeOnline[nm] != true) { edgeOnline[nm] = true; uiDirty = true }
                        updateEdgeIp(nm, f.edge.ip, f.edge.port)   // DHCPのIP変動を常時追従
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
    private fun reconcileEdgeRoster(ed: Edge, heldPlans: Set<String>) {
        val localIds = try {
            val pa = JSONArray(HgeNative.nativeListPlans())
            (0 until pa.length()).mapNotNull { pa.optJSONObject(it)?.optString("id")?.ifEmpty { null } }.toHashSet()
        } catch (_: Exception) { HashSet<String>() }
        val newHeld = heldPlans.filterTo(HashSet()) { localIds.contains(it) }
        if (edgeHeldByEdge[ed.name] != newHeld) {
            edgeHeldByEdge[ed.name] = newHeld
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
                val now = Calendar.getInstance()
                val s = fmtIso.format(now.time)
                val off = TimeZone.getDefault().getOffset(now.timeInMillis) / 60000
                Thread { try { HgeNative.nativeEdgeSyncTime(e.ip, e.port, s, off) } catch (_: Exception) {} }.start()
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
                if (po.optString("id") == id) return po.optString("name")
            }
        } catch (_: Exception) {}
        return ""
    }

    // エッジへ計画を送って撮影開始する。ブロッキングI/Oを含むため edgeExec(専用バックグラウンド)から呼ぶこと。
    // 項目5: 送る計画JSON(planJson)は呼び出し側が planId から取得して渡す。選択中の計画(グローバル状態)に
    //  依存しないので planExec を占有せず、送信中も計画選択が詰まらない。
    private fun startOnEdge(e: Edge, planId: String, planJson: String) {
        // 時刻同期(C_TIME)はエッジ端末の時計を「現在時刻」に合わせるためのもの。現在時刻を送る。
        val nowCal = Calendar.getInstance()
        val s = fmtIso.format(nowCal.time)
        val off = TimeZone.getDefault().getOffset(nowCal.timeInMillis) / 60000
        val name = planNameFor(planId)   // 対象planIdの名前を同期取得(非同期キャッシュ latestSchedule は使わない)
        val nameBmp = makeNameBitmapBytes(if (name.isEmpty()) "撮影計画" else name)
        run {
            // 【2026-08-06 廃止】開始前のカメラIP通知はやめた(理由は pushPresenceToActiveEdges の跡地を参照)。
            //  エッジは自分でカメラを見つけるので、スマホが見ているIPを渡す必要がない。
            //  探索に1秒余分にかかっても、間違ったネットワークのIPを渡すより良い。
            val r = HgeNative.nativeEdgeStart(e.ip, e.port, s, off, nameBmp, planId, planJson)
            // 開始が失敗コードを返しても、エッジ側では実際に走っている場合がある:
            //  ・既に同じ計画がエッジで走行中 → C_ACTION が INVALID_STATE で NAK(-4)
            //  ・2台順次開始のETP競合や ACK 取りこぼしで NAK/タイムアウト
            // このとき従来は「開始失敗」として待機集合から外し“開始前アイコン”に戻していた(=エッジ撮影中
            // なのにスマホは開始前)。失敗時はこの計画の実状態を問い合わせ、走っていれば開始成功として扱う。
            // ネットワークI/Oはこのスレッド上で行う(UIを固めない)。
            val running = if (r == 0) true else {
                val pj = try { HgeNative.nativeEdgeProgress(e.ip, e.port, planId) } catch (_: Exception) { "" }
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
                    if (currentPlanId == planId) { captureStatus.text = "● エッジ端末へ転送・撮影開始"; captureStatus.visibility = View.VISIBLE }
                    ensureEdgePoll()
                } else {
                    capturingPlans.remove(planId); waitingPlans.remove(planId); disconnectedPlans.remove(planId)
                    refreshPlanList(); updateReadOnly()
                    Toast.makeText(this, "エッジ端末 開始できませんでした (code=$r)", Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    // エッジ計画の停止。停止確定(IDLE)まで抑止しつつ非同期停止する共通経路(beginStop)へ委譲。
    // これにより NOCAMERA ダイアログの無限再表示や、停止途中のポーリング競合を防ぐ。
    private fun stopOnEdge(e: Edge, planId: String) {
        beginStop(planId) { HgeNative.nativeEdgeStop(e.ip, e.port, planId) }
    }
}
