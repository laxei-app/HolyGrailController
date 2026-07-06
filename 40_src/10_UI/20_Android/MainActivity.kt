package app.laxei.holygrail

import android.Manifest
import android.app.DatePickerDialog
import android.app.TimePickerDialog
import android.content.pm.PackageManager
import android.content.res.ColorStateList
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
    private lateinit var placeText: TextView
    private lateinit var latlngText: TextView
    private lateinit var cameraText: TextView
    private lateinit var lensText: TextView
    private lateinit var intervalText: TextView
    private lateinit var sensorText: TextView
    private lateinit var npfText: TextView
    private lateinit var landscapeCheck: CheckBox
    private var suppressLandscape = false   // updatePlanDisplay でのチェック設定が native を呼ばないように
    private lateinit var dirText: TextView
    private lateinit var compass: CompassView
    private lateinit var elevationView: ElevationView
    private lateinit var planSchedule: LinearLayout
    private lateinit var planListScroll: ScrollView
    private lateinit var planListContainer: LinearLayout
    private var currentPlanId = ""          // 編集対象の計画 id
    // 計画の選択・改名・各種編集(g_plan/g_editIdを触る操作)は単一スレッドで直列化し競合を防ぐ
    // (例: 改名と別計画選択が並走すると選択がファイルから古い名前を読み戻して改名が無効化される)。
    private val planExec = java.util.concurrent.Executors.newSingleThreadExecutor()
    private val capturingPlans = mutableSetOf<String>()  // 実撮影中(撮影窓内)の計画 id 群=カメラ点滅
    private val disconnectedPlans = mutableSetOf<String>() // カメラ未検出(NOCAMERA/旧DISCONNECTED)の計画 id 群=✖点灯
    private val waitingPlans = mutableSetOf<String>()    // 撮影要求済・撮影窓前で待機中(カメラOK)の計画 id 群=カメラ点灯
    private val nocamDialogShown = mutableSetOf<String>() // カメラ未検出ポップアップを表示済みの計画 id(多重表示抑止。Phase3)
    private var scheduleView: ScheduleView? = null   // §7.3.2 スケジュール表示/編集ビュー
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
    private lateinit var moonInitEditor: ExposureEditor // 月 開始時露出(単一)
    private lateinit var editLimit: LimitEditor         // 自動露出 露出限界(優先度+明暗を一体化)
    private lateinit var moonLimit: LimitEditor         // 月 露出限界(同上)

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
    private var suppressEdgeSel = false          // スピナーをプログラムで設定する間は選択保存を抑止
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
    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == BLE_PERM_REQ) {
            if (grantResults.isNotEmpty() && grantResults.all { it == PackageManager.PERMISSION_GRANTED }) pendingBleAction?.invoke()
            else Toast.makeText(this, "BLE権限が必要です", Toast.LENGTH_LONG).show()
            pendingBleAction = null
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
        loadColors()
        loadExpoValues()
        buildExposureEditors()
        loadRegisteredEdges()   // 設定で登録したエッジ端末(オフラインでも選択可)
        refreshEdgeSpinner()

        wireListeners()
        restorePlan()    // 保存済み計画があれば復元、無ければ出荷時計画を表示(再生成しない)
        refreshPlanList()   // 複数計画リスト(分割バー上)を構築
        restoreEdgeState()  // 再起動時: エッジが撮影中なら状態を復元(item9)
        resumePhoneCapture()  // 再起動時: スマホ直結で撮影中だった計画を再開(item2)
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
            for (ed in found) {
                val pj = HgeNative.nativeEdgeProgress(ed.ip, ed.port)
                if (pj.isEmpty()) continue
                try {
                    val o = JSONObject(pj)
                    val st = o.optInt("state")
                    if (st == HgeNative.ST_CAPTURING || st == HgeNative.ST_SEARCHING || st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_WAITING || st == HgeNative.ST_NOCAMERA) {
                        val nm = o.optString("name")
                        var pid = ""
                        try {
                            val pa = JSONArray(HgeNative.nativeListPlans())
                            for (k in 0 until pa.length()) { val po = pa.getJSONObject(k); if (po.optString("name") == nm) { pid = po.optString("id"); break } }
                        } catch (_: Exception) {}
                        val disc = (st == HgeNative.ST_DISCONNECTED || st == HgeNative.ST_NOCAMERA)
                        val wait = (st == HgeNative.ST_WAITING || st == HgeNative.ST_SEARCHING)
                        runOnUiThread {
                            // 復元: このエッジ(名称)を登録一覧へ統合し、復元中の計画のエッジ名称として設定する。
                            updateEdgeIp(ed.name, ed.ip, ed.port)
                            if (pid.isNotEmpty()) setPlanEdgeName(pid, ed.name)
                            refreshEdgeSpinner()
                            if (pid.isNotEmpty()) {
                                when { disc -> disconnectedPlans.add(pid); wait -> waitingPlans.add(pid); else -> { capturingPlans.add(pid); startBlink() } }
                                refreshPlanList(); updateReadOnly()
                            }
                            when {
                                disc -> { captureStatus.text = "● カメラが見つかりません(エッジ)"; captureStatus.setTextColor(0xFFD32F2F.toInt()) }
                                wait -> { captureStatus.text = "● 撮影開始待ち(エッジ)"; captureStatus.setTextColor(0xFF1565C0.toInt()) }
                                else -> { captureStatus.text = "● エッジ撮影中(復元)"; captureStatus.setTextColor(0xFF2E7D32.toInt()) }
                            }
                            captureStatus.visibility = View.VISIBLE
                            ensureEdgePoll()
                        }
                        // 複数エッジが並行撮影中の場合もあるため return せず、見つかった全エッジを復元する。
                    }
                } catch (_: Exception) {}
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
        cameraText = findViewById(R.id.plan_cameraText)
        lensText = findViewById(R.id.plan_lensText)
        intervalText = findViewById(R.id.plan_intervalText)
        sensorText = findViewById(R.id.plan_sensorText)
        npfText = findViewById(R.id.plan_npfText)
        landscapeCheck = findViewById(R.id.plan_landscape)
        dirText = findViewById(R.id.plan_dirText)
        compass = findViewById(R.id.plan_compass)
        elevationView = findViewById(R.id.plan_elevation)
        planSchedule = findViewById(R.id.plan_scheduleContainer)
        planListScroll = findViewById(R.id.plan_listScroll)
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

    private fun wireListeners() {
        // 撮影方向(方位磁石)/仰角(カメラの絵)を離した時にEntityへ反映しスケジュール再生成。
        compass.onCommit = { az -> pushDirectionToEntity(az, elevationView.angle) }
        elevationView.onCommit = { el -> pushDirectionToEntity(compass.azimuth, el) }
        startDate.setOnClickListener { pickDate(startCal) }
        startTime.setOnClickListener { pickTime(startCal) }
        endDate.setOnClickListener { pickDate(endCal) }
        endTime.setOnClickListener { pickTime(endCal) }
        resetButton.setOnClickListener {
            val now = Calendar.getInstance()
            startCal.timeInMillis = now.timeInMillis
            endCal.timeInMillis = now.timeInMillis
            endCal.add(Calendar.HOUR_OF_DAY, 2)
            updateTimeButtons(); pushTimesToEntity()
        }
        findViewById<Button>(R.id.plan_saveButton).setOnClickListener {
            planExec.execute {
                val r = HgeNative.nativeSavePlan()
                runOnUiThread {
                    Toast.makeText(this, if (r == 0) "撮影計画を保存しました" else "保存に失敗しました", Toast.LENGTH_SHORT).show()
                }
            }
        }
        capStopButton.setOnClickListener {
            val e = selectedEdge()
            if (e == null) {
                HgeNative.nativeCaptureStop()
                flipper.displayedChild = 0
            } else {
                stopOnEdge(e, currentPlanId)
            }
        }
        edgeSearchButton.setOnClickListener {
            Thread {
                val js = HgeNative.nativeEdgeSearch(2000)
                runOnUiThread { onEdgesFound(js) }
            }.start()
        }
        // スピナーでエッジを選ぶと、表示中の計画にその選択を保存する(計画ごとにエッジを指定)。
        edgeSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: android.widget.AdapterView<*>?, view: View?, pos: Int, idl: Long) {
                if (suppressEdgeSel) return
                val nm = if (pos in 1..edges.size) edges[pos - 1].name else ""
                setPlanEdgeName(currentPlanId, nm)
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
        findViewById<ImageView>(R.id.gmenu_back).setOnClickListener { flipper.displayedChild = 0 }
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
        // 撮影計画(330)のカメラ/レンズをタップで所持から選択する。
        cameraText.setOnClickListener { choosePlanCamera() }
        lensText.setOnClickListener { choosePlanLens() }
        findViewById<Button>(R.id.cmenu_night).setOnClickListener { openCcmEdit("night") }
        findViewById<Button>(R.id.cmenu_sunrise).setOnClickListener { openCcmEdit("sunrise") }
        findViewById<Button>(R.id.cmenu_sunset).setOnClickListener { openCcmEdit("sunset") }
        findViewById<Button>(R.id.cmenu_day).setOnClickListener { openCcmEdit("day") }
        findViewById<Button>(R.id.cmenu_moon).setOnClickListener { openMoonEdit() }
        findViewById<ImageView>(R.id.edit_back).setOnClickListener { stopDirtyWatch(); persistCcmEdit(); flipper.displayedChild = if (editingPlanCcm) 0 else 5 }
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
        setupDivider(R.id.moon_presetDivider, R.id.moon_presetScroll)
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
        // 月の影響への対処
        findViewById<ImageView>(R.id.moon_back).setOnClickListener { stopDirtyWatch(); persistMoonEdit(); flipper.displayedChild = if (editingPlanCcm) 0 else 5 }
        findViewById<Button>(R.id.moon_save).visibility = View.GONE   // 取消はエディタ先頭行へ移動
        setupValueSlider(R.id.moon_startlum_seek, 30, gradient = true, thumbRes = R.drawable.ic_moon) {
            findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", it * 0.1)
        }
        setupValueSlider(R.id.moon_ev_seek, 100, gradient = true, thumbRes = R.drawable.ic_moon) {
            findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -it * 0.1)
        }
        setupValueSlider(R.id.moon_extcoef_seek, 50) {
            findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + it * 0.01)
        }
        setupValueSlider(R.id.moon_skycoef_seek, 100) {
            findViewById<TextView>(R.id.moon_skycoef_val).text = "$it%"
        }
        val moonModes = arrayOf("補正しない", "画角全体の明るさで自動補正", "月に露出を合わせる")
        val ma = ArrayAdapter(this, android.R.layout.simple_spinner_item, moonModes)
        ma.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        val moonSpinner = findViewById<Spinner>(R.id.moon_mode)
        moonSpinner.adapter = ma
        moonSpinner.onItemSelectedListener = object : android.widget.AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: android.widget.AdapterView<*>?, v: View?, pos: Int, id: Long) {
                // 0=補正しない(中身なし) / 1=画角全体で自動補正 / 2=月に露出を合わせる
                findViewById<View>(R.id.moon_auto_section).visibility = if (pos == 1) View.VISIBLE else View.GONE
                findViewById<View>(R.id.moon_exp_section).visibility = if (pos == 2) View.VISIBLE else View.GONE
                findViewById<View>(R.id.moon_limit_section).visibility = if (pos == 0) View.GONE else View.VISIBLE
            }
            override fun onNothingSelected(p: android.widget.AdapterView<*>?) {}
        }
    }

    private fun openMoonEdit() {
        val o = ccmJson?.optJSONObject("moon") ?: return
        findViewById<TextView>(R.id.moon_title).text = "月の影響への対処" + (if (editingPlanCcm) "（この計画）" else "（初期値）")
        applyHeaderColor(R.id.moon_header, R.id.moon_title, 5)   // 月のシステム共通色
        val showMoonPreset = !editingPlanCcm
        findViewById<View>(R.id.moon_presetScroll).visibility = if (showMoonPreset) View.VISIBLE else View.GONE
        findViewById<View>(R.id.moon_presetDivider).visibility = if (showMoonPreset) View.VISIBLE else View.GONE
        findViewById<Spinner>(R.id.moon_mode).setSelection(o.optInt("mode", 0))
        val slP = (o.optDouble("startLuminance", 0.0) * 10).toInt().coerceIn(0, 30)
        setSliderProgress(R.id.moon_startlum_seek, slP)
        findViewById<TextView>(R.id.moon_startlum_val).text = String.format("+%.1fev", slP * 0.1)
        val evP = (-o.optDouble("ev", 0.0) * 10).toInt().coerceIn(0, 100)
        setSliderProgress(R.id.moon_ev_seek, evP)
        findViewById<TextView>(R.id.moon_ev_val).text = String.format("%.1fev", -evP * 0.1)
        val ecP = ((o.optDouble("extinctionCoef", 0.2) - 0.1) * 100).toInt().coerceIn(0, 50)
        setSliderProgress(R.id.moon_extcoef_seek, ecP)
        findViewById<TextView>(R.id.moon_extcoef_val).text = String.format("%.2f", 0.1 + ecP * 0.01)
        val skP = o.optDouble("skyBrightnessCoef", 100.0).toInt().coerceIn(0, 100)
        setSliderProgress(R.id.moon_skycoef_seek, skP)
        findViewById<TextView>(R.id.moon_skycoef_val).text = "$skP%"
        findViewById<CheckBox>(R.id.moon_atmext).isChecked = o.optBoolean("atmosphericExtinction", false)
        findViewById<CheckBox>(R.id.moon_geocorr).isChecked = o.optBoolean("geocentricCorrection", false)
        moonInitEditor.set(o.optJSONObject("initialExposure"))
        // 月撮影時露出限界: 暗所側は夜間撮影の設定値(limitBright)で固定。
        val nightLimit = ccmJson?.optJSONObject("night")?.optJSONObject("limitBright")
        moonLimit.set(o.optJSONObject("limitBright"), o.optJSONObject("limitDark"),
            o.optJSONArray("priority"), o.optJSONObject("initial"),
            moonMode = true, nightLimit = nightLimit)
        val onPick: (() -> Unit)? = if (editingPlanCcm) ({
            showPresetPicker("moon") { preset ->
                val all = ccmJson ?: return@showPresetPicker
                val merged = JSONObject(preset.toString()); merged.put("type", 5)
                all.put("moon", merged); openMoonEdit()
            }
        }) else null
        val cancelBtn = addPresetTopRow(R.id.moon_content, { cancelMoonEdit() }, onPick)
        startDirtyWatch(cancelBtn) { buildMoonEditJson()?.toString() ?: "" }      // item8
        flipper.displayedChild = 4
    }

    // 月編集の取り消し(保存済みから再読込)。
    private fun cancelMoonEdit() {
        if (!editingPlanCcm) { loadPresets(presetType); loadEditorOnly(); rebuildPresetList(); return }
        ccmJson = try { JSONObject(HgeNative.nativeGetPlanCcm()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        openMoonEdit()
    }

    // 現在の月エディタ内容から JSON を組み立てる(保存せず・破壊しない)。dirty 比較にも使う。
    private fun buildMoonEditJson(): JSONObject? {
        val all = ccmJson ?: return null
        val src = all.optJSONObject("moon") ?: return null
        val o = JSONObject(src.toString())
        o.put("color", ccmBgMap[5] ?: 0)   // 色はシステム共通(転送用に派生値を入れる)
        o.put("mode", findViewById<Spinner>(R.id.moon_mode).selectedItemPosition)
        o.put("startLuminance", sliderProgress(R.id.moon_startlum_seek) * 0.1)
        o.put("ev", -sliderProgress(R.id.moon_ev_seek) * 0.1)
        o.put("extinctionCoef", 0.1 + sliderProgress(R.id.moon_extcoef_seek) * 0.01)
        o.put("skyBrightnessCoef", sliderProgress(R.id.moon_skycoef_seek).toDouble())
        o.put("atmosphericExtinction", findViewById<CheckBox>(R.id.moon_atmext).isChecked)
        o.put("geocentricCorrection", findViewById<CheckBox>(R.id.moon_geocorr).isChecked)
        o.put("priority", moonLimit.getPriority())
        o.put("initialExposure", moonInitEditor.get())
        o.put("limitBright", moonLimit.getBright())
        o.put("limitDark", moonLimit.getDark())
        o.put("initial", moonLimit.getInitial())
        return o
    }

    private fun persistMoonEdit() {
        val all = ccmJson ?: return
        val o = buildMoonEditJson() ?: return
        all.put("moon", o)
        if (editingPlanCcm) HgeNative.nativeSetPlanCcm(all.toString()) else savePresetFromEditor(o)
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
    private fun openGearMenu() { buildGearMenu(); flipper.displayedChild = 5 }

    private fun gearBand(box: LinearLayout, title: String) {
        val tv = TextView(this); tv.text = title; tv.textSize = 14f; tv.setTypeface(null, Typeface.BOLD)
        tv.setTextColor(Color.WHITE); tv.setBackgroundColor(Color.parseColor("#5C6BC0"))
        tv.setPadding(dp(12), dp(6), dp(12), dp(6))
        box.addView(tv)
    }
    private fun gearItem(box: LinearLayout, title: String, onClick: () -> Unit) {
        val tv = TextView(this); tv.text = title; tv.textSize = 16f
        tv.setPadding(dp(28), dp(12), dp(12), dp(12))
        tv.setOnClickListener { onClick() }
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
        gearBand(box, "撮影制御方法 初期値")
        gearItem(box, "月の影響への対処") { openPresetScreen("moon") }
        gearItem(box, "夜間撮影") { openPresetScreen("night") }
        gearItem(box, "朝日撮影") { openPresetScreen("sunrise") }
        gearItem(box, "夕日撮影") { openPresetScreen("sunset") }
        gearItem(box, "日中撮影") { openPresetScreen("day") }
        gearBand(box, "自動露出")
        gearItem(box, "露出平滑化") { openSmoothingScreen() }
        gearBand(box, "色の設定")
        gearColorItem(box, "moon", "月の影響への対処")
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
        gearItem(box, "エッジ端末の登録") { manageEdges() }
        gearItem(box, "エッジ端末設定") { openEdgeSettings() }
    }

    // ---------- 8.2 エッジ端末設定(QR+PoP プロビジョニング) ----------
    // エッジが表示する QR {"n":端末名,"pop":乱数} を読み取り、端末識別名/SSID/password を
    // PoP 由来鍵で暗号化して BLE 送信する(送信本体は M3)。ここでは入力とQR読取まで。
    private fun openEdgeSettings() {
        val ctx = this
        val d = resources.displayMetrics.density
        val pad = (16 * d).toInt()
        val box = LinearLayout(ctx); box.orientation = LinearLayout.VERTICAL; box.setPadding(pad, pad, pad, 0)
        fun field(label: String, init: String, pw: Boolean): EditText {
            box.addView(TextView(ctx).apply { text = label; setPadding(0, (8 * d).toInt(), 0, 0) })
            val e = EditText(ctx); e.setText(init)
            if (pw) e.inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            box.addView(e); return e
        }
        val nameE = field("端末識別名", scannedName, false)
        val ssidE = field("接続先 SSID", "", false)
        val passE = field("接続先 password", "", true)
        // ネットワークモード: OFF=既存ネットに参加(STA。SSID/pass使用) / ON=エッジ自身がAP(屋外・ルーター無し)。
        // APではエッジ固有のAP資格を使うのでSSID/passは不要(エッジのLCDにQR表示)。
        val apSwitch = android.widget.Switch(ctx).apply {
            text = "エッジをAPにする (OFF=既存ネットに接続)"
            setPadding(0, (12 * d).toInt(), 0, 0)
        }
        apSwitch.setOnCheckedChangeListener { _, ap ->
            ssidE.isEnabled = !ap; passE.isEnabled = !ap		// AP時はSSID/pass入力を無効化
        }
        box.addView(apSwitch)
        val popView = TextView(ctx).apply {
            setPadding(0, (12 * d).toInt(), 0, 0)
            text = if (scannedPop.isEmpty()) "PoP: 未取得 — QRをスキャンしてください" else "PoP: $scannedPop"
        }
        box.addView(popView)
        val scanBtn = Button(ctx).apply { text = "エッジのQRをスキャン" }
        box.addView(scanBtn)

        val dlg = AlertDialog.Builder(ctx)
            .setTitle("エッジ端末設定 (§8.2)")
            .setView(ScrollView(ctx).apply { addView(box) })
            .setPositiveButton("送信", null)
            .setNegativeButton("閉じる", null)
            .create()

        scanBtn.setOnClickListener {
            val options = GmsBarcodeScannerOptions.Builder()
                .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
                .build()
            val scanner = GmsBarcodeScanning.getClient(ctx, options)
            scanner.startScan()
                .addOnSuccessListener { barcode ->
                    val contents = barcode.rawValue ?: ""
                    try {
                        val o = JSONObject(contents)
                        scannedPop = o.optString("pop", "")
                        scannedName = o.optString("n", "")
                        if (scannedName.isNotEmpty() && nameE.text.isNullOrEmpty()) nameE.setText(scannedName)
                        popView.text = "PoP: $scannedPop"
                        android.util.Log.i("EdgeProv", "QR decoded: name=$scannedName pop=$scannedPop raw=$contents")
                        Toast.makeText(ctx, "QR読取 OK: $scannedName / PoP=$scannedPop", Toast.LENGTH_LONG).show()
                    } catch (e: Exception) {
                        android.util.Log.w("EdgeProv", "QR parse failed raw=$contents")
                        Toast.makeText(ctx, "QR内容が不正: $contents", Toast.LENGTH_LONG).show()
                    }
                }
                .addOnCanceledListener {
                    Toast.makeText(ctx, "QRスキャンを中止しました", Toast.LENGTH_SHORT).show()
                }
                .addOnFailureListener { e ->
                    android.util.Log.w("EdgeProv", "scan failed: ${e.message}")
                    Toast.makeText(ctx, "スキャン失敗: ${e.message}", Toast.LENGTH_LONG).show()
                }
        }

        dlg.setOnShowListener {
            dlg.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                if (scannedPop.isEmpty()) {
                    Toast.makeText(ctx, "先にQRスキャンでPoPを取得してください", Toast.LENGTH_SHORT).show()
                    return@setOnClickListener
                }
                val name = nameE.text.toString()
                val ssid = ssidE.text.toString()
                val pass = passE.text.toString()
                val mode = if (apSwitch.isChecked) "ap" else "sta"
                // STA時のみSSID必須(AP時はエッジ固有AP資格を使うため不要)。
                if (mode == "sta" && ssid.isEmpty()) { Toast.makeText(ctx, "SSIDを入力してください", Toast.LENGTH_SHORT).show(); return@setOnClickListener }
                val json = JSONObject().put("name", name).put("ssid", ssid).put("pass", pass).put("mode", mode).toString()
                ensureBlePermissions {
                    popView.text = "BLE送信中..."
                    EdgeBle(ctx,
                        log = { m -> runOnUiThread { popView.text = m } },
                        result = { ok, m -> runOnUiThread {
                            android.util.Log.i("EdgeProv", "provision result ok=$ok msg=$m")
                            Toast.makeText(ctx, m, Toast.LENGTH_LONG).show()
                            popView.text = m
                            if (ok) dlg.dismiss()
                        } }
                    ).provision(scannedPop, json)
                }
            }
        }
        dlg.show()
    }

    // ---------- 602 色の設定(文字色/背景色。jaredrummler ColorPicker) ----------
    private fun colorTypeName(k: String) = when (k) {
        "night" -> "夜間撮影"; "sunrise" -> "朝日撮影"; "sunset" -> "夕日撮影"; "day" -> "日中撮影"
        "moon" -> "月の影響への対処"; "preNight" -> "夜間前移行"; "postNight" -> "夜間後移行"; else -> k
    }
    private fun openColorSetting(typeKey: String) { colorType = typeKey; buildColorScreen(); flipper.displayedChild = 10 }

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
    }

    private fun leaveColorScreen() { saveColorScreen(); flipper.displayedChild = 5; buildGearMenu() }

    // ---------- 630 露出平滑化(自動露出 全体設定。settings.json) ----------
    private fun openSmoothingScreen() {
        loadSmoothingScreen()
        startDirtyWatch(findViewById(R.id.smooth_cancel)) {
            "${sliderProgress(R.id.smooth_hyst_seek)},${sliderProgress(R.id.smooth_ma_seek)}"
        }
        flipper.displayedChild = 11
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
    private fun leaveSmoothingScreen() { stopDirtyWatch(); saveSmoothingScreen(); flipper.displayedChild = 5; buildGearMenu() }
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
    private fun openInitialMoon() {
        editingPlanCcm = false
        ccmJson = try { JSONObject(HgeNative.nativeGetCcmDefaults()) } catch (e: Exception) { null }
        if (ccmJson == null) return
        openMoonEdit()
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

    private fun presetListId() = if (presetType == "moon") R.id.moon_presetList else R.id.edit_presetList
    private fun presetScrollId() = if (presetType == "moon") R.id.moon_presetScroll else R.id.edit_presetScroll

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
        if (presetType == "moon") {
            ccmJson!!.put("moon", p)
            preferredTypeCcm("night")?.let { ccmJson!!.put("night", it) }
            openMoonEdit()
        } else {
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
    private fun persistCurrentPreset() { if (presetType == "moon") persistMoonEdit() else persistCcmEdit() }

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

    // インストール同梱の assets/master/*.json を /master へコピーする(無ければ)。
    // osfile のベース = getExternalFilesDir(null) なので dataManager は /master で読める。
    // 将来サーバ更新は filesDir 側を上書きする(本タスクでは未実装)。
    private fun copyMasterAssets(baseDir: java.io.File) {
        try {
            val dir = java.io.File(baseDir, "master")
            if (!dir.exists()) dir.mkdirs()
            for (name in listOf("cameras.json", "lenses.json")) {
                val out = java.io.File(dir, name)
                if (out.exists() && out.length() > 0L) continue   // 既にあれば残す(更新は別途)
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

    private fun openCameraList() { buildCameraList(); buildCameraDetail(); setInitialSplit(R.id.cameralist_listScroll, R.id.cameralist_container); flipper.displayedChild = 6 }
    private fun openCameraAdd()  { checkedCamAdd.clear(); buildCameraAdd(); flipper.displayedChild = 7 }
    private fun openLensList()   { buildLensList(); buildLensDetail(); setInitialSplit(R.id.lenslist_listScroll, R.id.lenslist_container); flipper.displayedChild = 8 }
    private fun openLensAdd()    { checkedLensAdd.clear(); expandedMakers.clear(); buildLensAdd(); flipper.displayedChild = 9 }

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
                    val headerH = root.getChildAt(0).height
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
            val parts = mutableListOf<String>()
            if (cam.optString("friendly").isNotEmpty()) parts.add("愛称:${cam.optString("friendly")}")
            if (cam.optString("serial").isNotEmpty()) parts.add("S/N:${cam.optString("serial")}")
            val sub = if (parts.isEmpty()) cam.optString("maker") else parts.joinToString("  ")
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
        addCancelButton(box, atTop = true) { buildCameraDetail() }   // 分割バー直下に右寄せ
        box.addView(editRow("メーカー", "maker", cam.optString("maker")))
        box.addView(editRow("モデル", "model", cam.optString("model")))
        // 名称はリストの行でインライン編集する(分割バー画面共通の動作)。詳細からは除外。
        box.addView(editRow("愛称", "friendly", cam.optString("friendly")))
        box.addView(editRow("シリアルNo.", "serial", cam.optString("serial")))
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

    private fun leaveCameraList() { persistCameraDetail(false); flipper.displayedChild = 5 }

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
        addCancelButton(box, atTop = true) { buildLensDetail() }   // 分割バー直下に右寄せ
        box.addView(editRow("メーカー", "maker", l.optString("maker")))
        // 名称(モデル)はリストの行でインライン編集する(分割バー画面共通の動作)。詳細からは除外。
        val cb = CheckBox(this); cb.text = "電子接点あり"; cb.isChecked = l.optBoolean("hasContact", true); lensContact = cb; box.addView(cb)
        box.addView(editRow2("F値", "fn", l.optDouble("fn", 0.0).toString(), "fnMax", l.optDouble("fnMax", 0.0).toString(), "〜", "", true))
        box.addView(editRow("焦点距離", "focalLength", l.optDouble("focalLength", 0.0).toString(), true))
        val note = TextView(this); note.text = "ズームの場合は撮影計画実行時の焦点距離を設定してください。"
        note.textSize = 12f; note.setTextColor(Color.GRAY); note.setPadding(0, dp(8), 0, dp(8)); box.addView(note)
    }

    private fun leaveLensList() { persistLensDetail(false); flipper.displayedChild = 5 }

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
        if (key == "moon") openMoonEdit() else openCcmEdit(key)
    }

    private val ccmTypeToKey = mapOf(1 to "night", 2 to "sunrise", 3 to "sunset", 4 to "day", 5 to "moon")
    private val ccmTypeName = mapOf(1 to "夜間撮影", 2 to "朝日撮影", 3 to "夕日撮影", 4 to "日中撮影",
                                    5 to "月の影響", 6 to "夜間前移行", 7 to "夜間後移行")

    // 撮影制御方法の編集ボタン(全5種)を常に定位置(スケジュールの下)に並べる。タップで編集画面へ。
    // 3列グリッド(夜間/朝日/夕日 と 日中/月)。色分けして「ボタンらしい」見た目にする。
    private fun buildCcmEditButtons() {
        val parent = findViewById<LinearLayout>(R.id.plan_ccmButtons)
        parent.removeAllViews()
        val groups = listOf(listOf(1, 2, 3), listOf(4, 5))
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
        fixEditor = ExposureEditor(findViewById(R.id.edit_fix_container))
        moonInitEditor = ExposureEditor(findViewById(R.id.moon_init_container))
        editLimit = LimitEditor(findViewById(R.id.edit_limit_container))
        moonLimit = LimitEditor(findViewById(R.id.moon_limit_container))
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
        private var moonMode = false                      // 月撮影時露出限界(暗所=夜間値で固定・明所のみ編集・仕様6d/e)
        private val cards = mutableListOf<View>()
        private val dividers = mutableListOf<View>()
        private val darkTvs = HashMap<Int, TextView>(); private val brightTvs = HashMap<Int, TextView>()
        private val initTvs = HashMap<Int, TextView>()
        private var dragFrom = -1

        fun set(brightObj: JSONObject?, darkObj: JSONObject?, prio: JSONArray?, initialObj: JSONObject?,
                moonMode: Boolean = false, nightLimit: JSONObject? = null, dayMode: Boolean = false) {
            order.clear()
            if (prio != null) for (i in 0 until prio.length()) order.add(prio.optInt(i))
            if (order.sorted() != listOf(0, 1, 2)) { order.clear(); order.addAll(listOf(0, 1, 2)) }
            this.moonMode = moonMode
            for (t in 0..2) {
                // 月モードでは暗所限界=夜間撮影の設定値(固定)。通常は limitBright。
                darkPlace[t] = (if (moonMode) nightLimit?.optString(keys[t]) else brightObj?.optString(keys[t])) ?: ""
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
            if (!moonMode) {   // 月モードは基準チェックも暗所/基準/明所の見出しも無し(明所限界のみ)
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
            }
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
            if (moonMode) {
                valRow.addView(brightTv)   // 明所限界のみ(センター付近)
                val rspacer = View(this@MainActivity); rspacer.layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.5f)
                valRow.addView(rspacer)
                brightTvs[t] = brightTv
            } else {
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
            // 月モード: 左=グレー●(夜間値で固定)/右=月。通常: 左=月(暗所)/右=太陽(明所)。
            if (moonMode) rs.setCustomThumbDrawablesForValues(R.drawable.thumb_dot_gray, R.drawable.ic_moon)
            else rs.setCustomThumbDrawablesForValues(R.drawable.ic_moon, R.drawable.ic_sun)
            val di = vals.indexOf(darkPlace[t]).let { if (it < 0) 0 else it }   // 暗所限界(月モードは夜間値=固定位置)
            val bi = vals.indexOf(brightPlace[t]).let { if (it < 0) vals.size - 1 else it }
            rs.values = listOf(minOf(di, bi).toFloat(), maxOf(di, bi).toFloat())
            var lock = false
            rs.addOnChangeListener { _, _, _ ->
                if (lock) return@addOnChangeListener
                val v = rs.values
                if (moonMode) {
                    if (v[0].toInt() != di) {   // 左(暗所=夜間値)が動いたら固定位置へ戻す
                        lock = true
                        rs.values = listOf(di.toFloat(), maxOf(v[1], di.toFloat()))
                        lock = false
                    }
                    brightPlace[t] = vals.getOrElse(rs.values[1].toInt()) { "" }
                } else {
                    darkPlace[t] = vals.getOrElse(v[0].toInt()) { "" }
                    brightPlace[t] = vals.getOrElse(v[1].toInt()) { "" }
                }
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

    private fun pickDate(cal: Calendar) {
        DatePickerDialog(this, { _, y, m, d ->
            cal.set(Calendar.YEAR, y); cal.set(Calendar.MONTH, m); cal.set(Calendar.DAY_OF_MONTH, d)
            clampPlanRange(cal === startCal); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.YEAR), cal.get(Calendar.MONTH), cal.get(Calendar.DAY_OF_MONTH)).show()
    }

    private fun pickTime(cal: Calendar) {
        TimePickerDialog(this, { _, h, min ->
            cal.set(Calendar.HOUR_OF_DAY, h); cal.set(Calendar.MINUTE, min); cal.set(Calendar.SECOND, 0)
            clampPlanRange(cal === startCal); updateTimeButtons(); pushTimesToEntity()
        }, cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE), true).show()
    }

    // 開始/終了の間隔を 最小1分〜最大24時間 に収め、逆転(終了≦開始)を防ぐ。
    // startChanged=true(開始を編集) のときは終了を、false(終了を編集) のときは開始を動かす。
    private fun clampPlanRange(startChanged: Boolean) {
        val minMs = 60_000L              // 最小1分
        val maxMs = 24L * 60L * 60_000L  // 最大24時間
        val span = endCal.timeInMillis - startCal.timeInMillis
        if (span in minMs..maxMs) return
        if (startChanged) {
            // 開始を基準に終了を範囲内へ寄せる。
            val clamped = if (span < minMs) minMs else maxMs
            endCal.timeInMillis = startCal.timeInMillis + clamped
        } else {
            // 終了を基準に開始を範囲内へ寄せる。
            val clamped = if (span < minMs) minMs else maxMs
            startCal.timeInMillis = endCal.timeInMillis - clamped
        }
    }

    private fun updateTimeButtons() {
        startDate.text = fmtDate.format(startCal.time)
        startTime.text = fmtTime.format(startCal.time)
        endDate.text = fmtDate.format(endCal.time)
        endTime.text = fmtTime.format(endCal.time)
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

    // 撮影方向/仰角をEntityへ渡してスケジュールを再生成させる(結果はEV_SCHEDULEで反映)。
    private fun pushDirectionToEntity(az: Float, el: Float) {
        dirText.text = "撮影方向 %.1f°   仰角 %.1f°".format(az, el)
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
            // 選択中かつ非実行中の計画 → 名前タップで編集可能(キーボード)。確定で改名。
            tv.isFocusableInTouchMode = true; tv.isFocusable = true
            tv.setOnEditorActionListener { v, actionId, _ ->
                if (actionId == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) {
                    val nm = v.text.toString().trim()
                    planListScroll.isFocusableInTouchMode = true
                    planListScroll.requestFocus()   // 隣行へ飛ばずキーボードを閉じる
                    (getSystemService(INPUT_METHOD_SERVICE) as android.view.inputmethod.InputMethodManager)
                        .hideSoftInputFromWindow(v.windowToken, 0)
                    if (nm.isNotEmpty()) planExec.execute {
                        val r = HgeNative.nativeRenamePlan(id, nm)   // item5: 重複なら ERR_NAME_DUP
                        runOnUiThread { if (r == HgeNative.ERR_NAME_DUP) showNameInUse(nm); refreshPlanList() }
                    }
                    true
                } else false
            }
        } else {
            // 未選択(または撮影中)の行 → タップで選択のみ(キーボードは出さない)。
            tv.isFocusable = false; tv.isFocusableInTouchMode = false
            tv.setOnClickListener { selectPlanRow(id) }
        }
        row.addView(tv)
        // ⋮ コンテキストメニュー(削除/コピーを追加)
        val menu = ImageView(this)
        menu.layoutParams = LinearLayout.LayoutParams(dp(32), dp(32))
        menu.setImageResource(R.drawable.ic_menu)
        menu.setOnClickListener { planContextMenu(it, id, name) }
        row.addView(menu)
        return row
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
            runOnUiThread { currentPlanId = cur; refreshPlanList() }
        }
    }

    private fun startPlan(id: String) {
        // この計画に指定されたエッジ端末の"名称"(無ければスマホで撮影)。
        val name = planEdgeName(id)
        if (name.isEmpty()) {
            // スマホで撮影。
            planExec.execute {
                HgeNative.nativeSelectPlan(id)
                runOnUiThread {
                    currentPlanId = id; waitingPlans.add(id)   // まずは待機(点灯)。実状態はEV_STATEで即補正される
                    HgeNative.nativeCaptureStartPlan(id)
                    refreshPlanList(); updateReadOnly()
                }
            }
            return
        }
        // エッジで撮影。IPは動的なので、開始時に端末名称で検索して現在のIPを解決する。見つからなければ開始しない。
        Thread {
            val e = discoverEdgeByName(name)
            runOnUiThread {
                if (e == null) {
                    Toast.makeText(this, "エッジ端末(${name})が見つからないため撮影を開始できません", Toast.LENGTH_LONG).show()
                    return@runOnUiThread
                }
                updateEdgeIp(name, e.ip, e.port)   // 解決したIPを保持(停止/状態確認に使う)
                planExec.execute {
                    HgeNative.nativeSelectPlan(id)
                    runOnUiThread {
                        currentPlanId = id; waitingPlans.add(id)   // まずは待機(点灯)。実状態はedgePollで補正される
                        startOnEdge(e, id)
                        refreshPlanList(); updateReadOnly()
                    }
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

    private fun showNoCameraDialog(id: String) {
        if (nocamDialogShown.contains(id)) return   // 既に表示中/継続中は出さない
        nocamDialogShown.add(id)
        val cam = planCameraLabel(id)
        val e = planEdge(id)   // null=スマホ直接
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("カメラが見つかりません")
            .setMessage("${cam}が見つかりません。オンラインにしてください。")
            .setCancelable(false)
            .setPositiveButton("継続") { dlg, _ ->
                dlg.dismiss()
                // 即再探索(取得フェーズの60秒待ちを前倒し)。ネットワークI/Oは別スレッド。
                Thread { if (e == null) HgeNative.nativePokeAcquire(id) else HgeNative.nativeEdgeResearch(e.ip, e.port, id) }.start()
                // 猶予後もまだ未検出なら再度ポップアップ(継続の間は nocamDialogShown を維持して多重表示を防ぐ)。
                handler.postDelayed({
                    if (disconnectedPlans.contains(id)) { nocamDialogShown.remove(id); showNoCameraDialog(id) }
                }, 12000)
            }
            .setNegativeButton("中止") { dlg, _ ->
                dlg.dismiss()
                val e2 = planEdge(id)
                capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); nocamDialogShown.remove(id)
                if (capturingPlans.isEmpty()) stopBlink()
                refreshPlanList(); updateReadOnly()
                Thread { if (e2 == null) HgeNative.nativeCaptureStopPlan(id) else stopOnEdge(e2, id) }.start()
            }
            .show()
    }

    private fun confirmStop(id: String) {
        // 338.撮影中止 ダイアログ
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("撮影中止")
            .setMessage("撮影を中止しますか？")
            .setPositiveButton("中止する") { dlg, _ ->
                dlg.dismiss()   // 中止選択で即ダイアログを閉じる(停止処理の完了は待たない。指示4)
                val e = planEdge(id)   // この計画のエッジ(無ければスマホ)
                capturingPlans.remove(id); waitingPlans.remove(id); disconnectedPlans.remove(id); nocamDialogShown.remove(id)
                if (capturingPlans.isEmpty()) stopBlink()
                refreshPlanList(); updateReadOnly()
                // 停止指示はネットワークI/Oを伴うためバックグラウンドで実行(UIを固めない)。
                Thread { if (e == null) HgeNative.nativeCaptureStopPlan(id) else stopOnEdge(e, id) }.start()
            }
            .setNegativeButton("続ける", null)
            .show()
    }

    private fun planContextMenu(anchor: View, id: String, name: String) {
        val pm = PopupMenu(this, anchor)
        pm.menu.add("コピーを追加")
        pm.menu.add("削除")
        pm.setOnMenuItemClickListener { mi ->
            when (mi.title) {
                "コピーを追加" -> planExec.execute {
                    HgeNative.nativeCopyPlan(id)
                    val c = HgeNative.nativeCurrentPlanId()
                    runOnUiThread {
                        // エッジ端末の指定(Android prefs管理)はコピー元から引き継ぐ。他項目はentityが複製済み。
                        if (c.isNotEmpty()) setPlanEdgeName(c, planEdgeName(id))
                        currentPlanId = c; refreshPlanList()
                    }
                }
                "削除" -> confirmDeletePlan(id, name)
            }
            true
        }
        pm.show()
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
        HgeNative.nativeSetListener(null)
        HgeNative.nativeCaptureStop()
        HgeNative.nativeTerm()
        releaseMulticastLock()
        super.onDestroy()
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
        runOnUiThread {
            when (event) {
                HgeNative.EV_STATE -> {
                    val o = JSONObject(json)
                    val st = o.optInt("state", HgeNative.ST_IDLE)
                    val pid = o.optString("planId")
                    capState.text = "state: ${HgeNative.stateName(st)}"
                    if (pid.isNotEmpty()) {
                        // 状態→3集合(撮影中=点滅 / 待機=点灯 / 未検出=✖)。各計画はいずれか一つ。
                        when (st) {
                            HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                                capturingPlans.add(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid); nocamDialogShown.remove(pid)
                                startBlink()
                            }
                            HgeNative.ST_WAITING, HgeNative.ST_SEARCHING -> {
                                waitingPlans.add(pid); capturingPlans.remove(pid); disconnectedPlans.remove(pid); nocamDialogShown.remove(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                            }
                            HgeNative.ST_NOCAMERA, HgeNative.ST_DISCONNECTED -> {
                                disconnectedPlans.add(pid); capturingPlans.remove(pid); waitingPlans.remove(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                                showNoCameraDialog(pid)   // Phase3c: 継続/中止ポップアップ(多重抑止あり)
                            }
                            HgeNative.ST_IDLE, HgeNative.ST_ERROR -> {
                                capturingPlans.remove(pid); waitingPlans.remove(pid); disconnectedPlans.remove(pid)
                                nocamDialogShown.remove(pid)
                                if (capturingPlans.isEmpty()) stopBlink()
                            }
                        }
                        refreshPlanList(); updateReadOnly()
                    }
                    // 表示中の計画の状態をステータス表示(未検出=赤 / 撮影中=緑 / 待機=青)。
                    if (disconnectedPlans.contains(currentPlanId)) {
                        captureStatus.text = "● カメラが見つかりません"; captureStatus.setTextColor(0xFFD32F2F.toInt()); captureStatus.visibility = View.VISIBLE
                    } else if (capturingPlans.contains(currentPlanId)) {
                        captureStatus.text = "● 撮影中 (${HgeNative.stateName(st)})"; captureStatus.setTextColor(0xFF2E7D32.toInt()); captureStatus.visibility = View.VISIBLE
                    } else if (waitingPlans.contains(currentPlanId)) {
                        captureStatus.text = "● 撮影開始待ち"; captureStatus.setTextColor(0xFF1565C0.toInt()); captureStatus.visibility = View.VISIBLE
                    } else { captureStatus.visibility = View.GONE }
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
                HgeNative.EV_ERROR -> {
                    val o = JSONObject(json)
                    val msg = o.optString("msg")
                    capState.text = "ERROR $msg"
                    // カメラ未検出・カメラ使用中など、撮影開始の失敗をユーザーへ通知する。
                    if (msg.isNotEmpty()) Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
                }
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
            sensorText.text = "センサー %.1f×%.1fmm  焦点距離 %d mm  画角 %.0f×%.0f°".format(
                o.optDouble("sensorW"), o.optDouble("sensorH"), o.optInt("focalLength"),
                o.optDouble("fovH"), o.optDouble("fovV"))
            intervalText.text = "${o.optInt("interval")}秒"
            suppressLandscape = true
            landscapeCheck.isChecked = o.optBoolean("landscape")
            suppressLandscape = false
            npfText.text = "NPF %.1f秒   最小周期 %d秒".format(o.optDouble("npf"), o.optInt("minInterval"))
            // 撮影方向/仰角ウィジェットへ反映(setterは無音=コールバックを呼ばない)
            val az = o.optDouble("azimuth", 90.0).toFloat()
            val el = o.optDouble("elevation", 10.0).toFloat()
            compass.setAzimuth(az)
            compass.setFov(o.optDouble("fovH", 80.0).toFloat())
            compass.setMarkers(
                o.optDouble("sunriseAz", Double.NaN).toFloat(),
                o.optDouble("sunsetAz", Double.NaN).toFloat(),
                o.optDouble("moonriseAz", Double.NaN).toFloat(),
                o.optDouble("moonsetAz", Double.NaN).toFloat())
            elevationView.setAngle(el)
            elevationView.setFov(o.optDouble("fovV", 50.0).toFloat())
            dirText.text = "撮影方向 %.1f°   仰角 %.1f°".format(az, el)
            capGear.text = o.optString("camera") + " / " + o.optString("lens")
            capDir.text = dirText.text
            renderScheduleView(o)                  // §7.3.2 太陽高度帯+境目編集ビュー(計画画面)
            renderSchedule(capSchedule, o, true)   // 撮影画面は従来のイベント時系列
        } catch (_: Exception) {}
    }

    // 型→色テーブルキー。
    private fun keyType(key: String): Int = when (key) {
        "night" -> 1; "sunrise" -> 2; "sunset" -> 3; "day" -> 4; "moon" -> 5; "preNight" -> 6; "postNight" -> 7; else -> 0
    }
    private val colorTypeNames = mapOf(1 to "night", 2 to "sunrise", 3 to "sunset", 4 to "day", 5 to "moon", 6 to "preNight", 7 to "postNight")

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
    private fun renderScheduleView(o: JSONObject) {
        val sv = scheduleView ?: ScheduleView(this).also {
            it.onTapType = { t -> ccmTypeToKey[t]?.let { k -> openPlanCcmEdit(k) } }
            it.onMoveBoundary = { before, after, occ, altDeg, rising ->
                Thread { HgeNative.nativeSetBoundaryByAlt(before, after, occ, altDeg, rising) }.start()
            }
            it.onSetBand = { rising, insert -> setBand(rising, insert) }
            it.onNeedTwoFinger = { Toast.makeText(this, "スケジュール変更は2本指で", Toast.LENGTH_SHORT).show() }
            scheduleView = it
            planSchedule.removeAllViews()
            planSchedule.addView(it, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        curSunriseMode = o.optInt("sunriseMode", 0)
        curSunsetMode = o.optInt("sunsetMode", 0)
        val blocks = ArrayList<ScheduleView.Block>()
        o.optJSONArray("blocks")?.let { arr ->
            for (i in 0 until arr.length()) {
                val b = arr.getJSONObject(i)
                val segs = ArrayList<ScheduleView.Seg>()
                b.optJSONArray("segments")?.let { sa ->
                    for (k in 0 until sa.length()) {
                        val s = sa.getJSONObject(k); val ty = s.optInt("type")
                        val col = if (ty in 1..7) ccmColor(ty) else 0
                        val tc = if (ty in 1..7) ccmTextColor(ty) else 0xFF212121.toInt()
                        val nm = ccmTypeName[ty] ?: s.optString("name")   // 種別の和名を表示
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
                blocks.add(ScheduleView.Block(b.optString("title"), b.optString("axis") == "down",
                    b.optString("date"), segs, marks))
            }
        }
        sv.setData(blocks)
    }

    // 撮影要求済(撮影中/待機/未検出)の計画を表示しているときは一切編集できない(item7)。各操作部の有効/無効を切替。
    private fun updateReadOnly() {
        planReadOnly = capturingPlans.contains(currentPlanId) || waitingPlans.contains(currentPlanId) || disconnectedPlans.contains(currentPlanId)
        val ed = !planReadOnly
        intArrayOf(R.id.plan_startDate, R.id.plan_startTime, R.id.plan_endDate, R.id.plan_endTime,
            R.id.plan_resetButton, R.id.plan_saveButton, R.id.plan_gearConst,
            R.id.plan_edgeSearchButton, R.id.searchButton, R.id.connectButton)
            .forEach { findViewById<View>(it).isEnabled = ed }
        intervalText.isEnabled = ed; landscapeCheck.isEnabled = ed
        cameraText.isEnabled = ed; lensText.isEnabled = ed; edgeSpinner.isEnabled = ed
        compass.isEnabled = ed; elevationView.isEnabled = ed; scheduleView?.isEnabled = ed
        findViewById<LinearLayout>(R.id.plan_ccmButtons).let { for (i in 0 until it.childCount) it.getChildAt(i).isEnabled = ed }
        findViewById<View>(R.id.plan_saveButton).alpha = if (ed) 1f else 0.4f
    }

    // 夕日/朝日の帯を挿入(insert=true)/排除(insert=false)。他方の帯モードは保持する。
    private fun setBand(rising: Boolean, insert: Boolean) {
        var sr = curSunriseMode
        var ss = curSunsetMode
        val m = if (insert) 1 else 2
        if (rising) sr = m else ss = m
        Thread { HgeNative.nativeSetBandMode(sr, ss) }.start()
    }

    // 撮影周期をキーボード入力(秒)。最小周期(最長ss+2)未満は警告して反映しない。
    private fun editInterval() {
        val cur = try { JSONObject(latestSchedule).optInt("interval", 15) } catch (_: Exception) { 15 }
        val et = EditText(this)
        et.inputType = InputType.TYPE_CLASS_NUMBER
        et.setText(cur.toString())
        androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("撮影周期(秒)")
            .setView(et)
            .setPositiveButton("OK") { _, _ ->
                val sec = et.text.toString().toDoubleOrNull() ?: return@setPositiveButton
                planExec.execute {
                    val r = HgeNative.nativeSetPlanInterval(sec)
                    runOnUiThread {
                        if (r != 0) Toast.makeText(this, "先にシャッター速度を変更してください(最小周期未満)", Toast.LENGTH_LONG).show()
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
    private fun saveRegisteredEdges() {
        val a = JSONArray()
        edges.forEach { a.put(JSONObject().apply { put("name", it.name); put("ip", it.ip); put("port", it.port) }) }
        hgcPrefs().edit().putString("regEdges", a.toString()).apply()
    }
    // 計画ごとのエッジ選択(prefsに端末"名称"を保存。空=無し=スマホで撮影)。
    // IPはDHCPで変わり事前に不定のため、識別は端末名称で行い、IPは開始時に検索で解決する。
    private fun planEdgeName(planId: String) = if (planId.isEmpty()) "" else (hgcPrefs().getString("pe_$planId", "") ?: "")
    private fun setPlanEdgeName(planId: String, name: String) { if (planId.isNotEmpty()) hgcPrefs().edit().putString("pe_$planId", name).apply() }
    private fun planEdge(planId: String): Edge? {
        val name = planEdgeName(planId)
        return if (name.isEmpty()) null else (edges.firstOrNull { it.name == name } ?: Edge(name, "", 50506))
    }
    // 端末名称に一致するエッジをネットワーク検索して現在のIPを解決する(見つからなければ null)。
    private fun discoverEdgeByName(name: String): Edge? {
        // ① UDP検索で現在IPを解決(最も確実だが、混雑WiFiでは応答UDPを取りこぼすことがある)。
        val js = HgeNative.nativeEdgeSearch(2500)
        try {
            val arr = JSONArray(js)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                if (o.optString("name") == name) return Edge(name, o.optString("ip"), o.optInt("port", 50506))
            }
        } catch (_: Exception) {}
        // ② B-1: UDP検索が取りこぼした場合のフォールバック。登録一覧の last-seen IP がまだ ETP(TCP)で
        //    応答するなら、それを採用して開始する(混雑WiFiでの検索不発による「開始できません」を回避)。
        //    IPがDHCPで変わっていれば probe は無応答 → null(=本当に見つからない)で従来どおりトースト。
        val cached = edges.firstOrNull { it.name == name && it.ip.isNotEmpty() }
        if (cached != null) {
            val pj = try { HgeNative.nativeEdgeProgress(cached.ip, cached.port) } catch (_: Exception) { "" }
            if (pj.isNotEmpty()) return cached
        }
        return null
    }
    // 登録一覧の last-seen IP を更新(stop/poll 用に開始時の解決結果を保持)。
    private fun updateEdgeIp(name: String, ip: String, port: Int) {
        val i = edges.indexOfFirst { it.name == name }
        if (i >= 0) edges[i] = Edge(name, ip, port) else edges.add(Edge(name, ip, port))
        saveRegisteredEdges()
    }

    private fun refreshEdgeSpinner() {
        val labels = mutableListOf("無し (スマホで撮影)")
        edges.forEach { labels.add(it.name) }   // IPは動的なので名称のみ表示
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, labels)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        suppressEdgeSel = true
        edgeSpinner.adapter = adapter
        val name = planEdgeName(currentPlanId)
        val idx = if (name.isEmpty()) 0 else edges.indexOfFirst { it.name == name }.let { if (it >= 0) it + 1 else 0 }
        try { edgeSpinner.setSelection(idx) } catch (_: Exception) {}
        suppressEdgeSel = false
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

    // エッジ端末の登録/管理(設定)。名称+IPで手動登録、削除。オフラインでも登録でき計画で選べる。
    private fun manageEdges() {
        val ctx = this; val d = resources.displayMetrics.density; val pad = (16 * d).toInt()
        val box = LinearLayout(ctx); box.orientation = LinearLayout.VERTICAL; box.setPadding(pad, pad, pad, 0)
        fun rebuild() {
            box.removeAllViews()
            box.addView(TextView(ctx).apply { text = "登録済みエッジ端末"; setTypeface(null, Typeface.BOLD) })
            if (edges.isEmpty()) box.addView(TextView(ctx).apply { text = "(なし)"; setPadding(0, (4 * d).toInt(), 0, 0) })
            edges.toList().forEach { e ->
                val row = LinearLayout(ctx); row.orientation = LinearLayout.HORIZONTAL; row.gravity = Gravity.CENTER_VERTICAL
                val sub = if (e.ip.isEmpty()) "" else " (前回 ${e.ip})"
                row.addView(TextView(ctx).apply { text = e.name + sub; layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f) })
                row.addView(Button(ctx).apply { text = "削除"; setOnClickListener { edges.remove(e); saveRegisteredEdges(); refreshEdgeSpinner(); rebuild() } })
                box.addView(row)
            }
            box.addView(TextView(ctx).apply { text = "追加(端末名称で登録。IPは撮影開始時に自動検索)"; setTypeface(null, Typeface.BOLD); setPadding(0, (14 * d).toInt(), 0, 0) })
            val nameE = EditText(ctx).apply { hint = "端末名称(初期設定で付けた名前)" }; box.addView(nameE)
            box.addView(Button(ctx).apply {
                text = "登録"
                setOnClickListener {
                    val nm = nameE.text.toString().trim()
                    if (nm.isEmpty()) { Toast.makeText(ctx, "端末名称を入力してください", Toast.LENGTH_SHORT).show(); return@setOnClickListener }
                    if (edges.none { it.name == nm }) edges.add(Edge(nm, "", 50506))
                    saveRegisteredEdges(); refreshEdgeSpinner(); rebuild()
                }
            })
        }
        rebuild()
        val dlg = AlertDialog.Builder(ctx).setTitle("エッジ端末の登録").setView(ScrollView(ctx).apply { addView(box) }).setPositiveButton("閉じる", null).create()
        dlg.setCanceledOnTouchOutside(false)   // 入力フォームが誤タップで閉じないように
        dlg.show()
    }

    // エッジ端末の状態を約10秒ごとに問い合わせ、スマホ表示へ反映する(仕様8.2)。
    // エッジ側で停止された場合もここで検知してスマホUIを更新する(既知ギャップの解消)。
    // 撮影中/待機/未検出の「エッジ計画」を列挙する。計画ごとに別エッジを指定でき並行撮影も可能なので、
    // 表示中の1台ではなく“アクティブな全エッジ計画”を対象にする。phone直は planEdge==null で除外される。
    private fun activeEdgePlans(): List<String> =
        (capturingPlans + waitingPlans + disconnectedPlans).filter { planEdge(it) != null }

    // 1つのエッジ計画の進捗JSONを状態集合へ反映する。表示中の計画(currentPlanId)ならステータス行も更新。
    // ST_IDLE/ST_ERROR ならその計画を各集合から除く(=撮影終了)。
    private fun reconcileEdgePlan(pid: String, pj: String) {
        if (pj.isEmpty()) return
        try {
            val o = JSONObject(pj)
            val st = o.optInt("state")
            var changed = false
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
                    nocamDialogShown.remove(pid)
                    if (currentPlanId == pid) { captureStatus.text = "● 撮影開始待ち(エッジ)"; captureStatus.setTextColor(0xFF1565C0.toInt()); captureStatus.visibility = View.VISIBLE }
                }
                HgeNative.ST_CAPTURING, HgeNative.ST_STOPPING -> {
                    if (capturingPlans.add(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    nocamDialogShown.remove(pid)
                    if (currentPlanId == pid) { captureStatus.text = "● エッジ撮影中  ${o.optInt("frame")}/${o.optInt("total")}枚  残り${o.optInt("remainSec")}秒"; captureStatus.setTextColor(0xFF2E7D32.toInt()); captureStatus.visibility = View.VISIBLE }
                }
                HgeNative.ST_IDLE, HgeNative.ST_ERROR -> {   // この計画は終了 → 各集合から除去
                    if (capturingPlans.remove(pid)) changed = true
                    if (waitingPlans.remove(pid)) changed = true
                    if (disconnectedPlans.remove(pid)) changed = true
                    nocamDialogShown.remove(pid)
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
                    val pj = HgeNative.nativeEdgeProgress(e.ip, e.port)
                    runOnUiThread { reconcileEdgePlan(pid, pj) }
                }.start()
            }
            handler.postDelayed(this, 10000)   // 約10秒間隔(アクティブが続く限り再スケジュール)
        }
    }

    // edgePoll を単一インスタンスで(再)起動する。多重 postDelayed による二重ループを防ぐ。
    private fun ensureEdgePoll() { handler.removeCallbacks(edgePoll); handler.postDelayed(edgePoll, 2000) }

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

    private fun startOnEdge(e: Edge, planId: String) {
        // 時刻同期(C_TIME)はエッジ端末の時計を「現在時刻」に合わせるためのもの。現在時刻を送る。
        val nowCal = Calendar.getInstance()
        val s = fmtIso.format(nowCal.time)
        val off = TimeZone.getDefault().getOffset(nowCal.timeInMillis) / 60000
        val name = try { JSONObject(latestSchedule).optString("name") } catch (_: Exception) { "" }
        val nameBmp = makeNameBitmapBytes(if (name.isEmpty()) "撮影計画" else name)
        Thread {
            // 発見中のオンラインカメラをエッジへ通知(IP直結ヒント)。エッジは (model,serial) で本人確認して直結する。
            try {
                val arr = org.json.JSONArray(HgeNative.nativeSearchDevicesList())
                val push = org.json.JSONArray()
                for (i in 0 until arr.length()) {
                    val d = arr.optJSONObject(i) ?: continue
                    if (d.optString("ip").isEmpty()) continue
                    push.put(JSONObject()
                        .put("serial", d.optString("serial"))
                        .put("model", d.optString("model"))
                        .put("ip", d.optString("ip"))
                        .put("online", true))
                }
                if (push.length() > 0) HgeNative.nativeEdgeCameraInfo(e.ip, e.port, push.toString())
            } catch (_: Exception) {}
            val r = HgeNative.nativeEdgeStart(e.ip, e.port, s, off, nameBmp, planId)
            runOnUiThread {
                if (r == 0) {
                    // waitingPlans には startPlan で追加済み。以降は edgePoll(全エッジ計画対象)が各計画の状態を反映する。
                    if (currentPlanId == planId) { captureStatus.text = "● エッジ端末へ転送・撮影開始"; captureStatus.visibility = View.VISIBLE }
                    ensureEdgePoll()
                } else {
                    capturingPlans.remove(planId); waitingPlans.remove(planId); disconnectedPlans.remove(planId)
                    refreshPlanList(); updateReadOnly()
                    Toast.makeText(this, "エッジ端末 開始失敗 (code=$r)", Toast.LENGTH_LONG).show()
                }
            }
        }.start()
    }

    private fun stopOnEdge(e: Edge, planId: String) {
        Thread {
            HgeNative.nativeEdgeStop(e.ip, e.port, planId)
            runOnUiThread {
                // この計画だけを各集合から除去する(他のエッジ計画は継続)。
                capturingPlans.remove(planId); waitingPlans.remove(planId); disconnectedPlans.remove(planId); nocamDialogShown.remove(planId)
                if (capturingPlans.isEmpty()) stopBlink()
                if (currentPlanId == planId) captureStatus.visibility = View.GONE
                refreshPlanList(); updateReadOnly()
                if (activeEdgePlans().isEmpty()) handler.removeCallbacks(edgePoll)   // 他に撮影中のエッジ計画が無ければポール停止
            }
        }.start()
    }
}
