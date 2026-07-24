# スマホ直結で撮影を一発開始するヘルパー(セッションをまたいで再利用)。
#
# 【何をするか】
#   計画名を指定すると: 行を特定 → 選択 → エッジ端末「無し(スマホで撮影)」→ 終了時刻を設定
#   → RECカチンコで開始 → 「カメラが見つかりません」なら継続 → HGCaptureログで撮影確認。
#   前回(2026-07-24)これを手作業でやって遠回りしたので手順を固定化した。詳細な意味は
#   記憶 phone-shoot-operation.md 参照。
#
# 【使い方】
#   . .\planrows.ps1                              # Get-Plans / Test-DialogOpen を読み込む
#   .\start_shoot.ps1 -Name CoreS3VatR10 -End 21:00            # ドライラン(タップしない/座標だけ表示)
#   .\start_shoot.ps1 -Name CoreS3VatR10 -End 21:00 -Go        # 実行(タップする)
#   2台なら -Name ごとに2回。開始は過去でOK・RECで即開始し終了時刻まで撮る。
#
# 【重要】撮影中は uiautomator dump が不安定なので使わない。行位置は planrows.ps1(端末の
#   plan_*.json から算出)で引く。ダイアログ有無は Test-DialogOpen(画面ディム)で見る。
#   座標は 1080x2400・計画一覧が数件・詳細シートが先頭スクロール状態での実測値(2026-07-24)。
#   端末/レイアウトが変わったら $C の座標だけ直せばよい。各手順でスクショを $env:TEMP に残す。

param(
    [Parameter(Mandatory)][string]$Name,   # 計画名(例 CoreS3VatR10)
    [Parameter(Mandatory)][string]$End,    # 終了時刻 "HH:mm" (例 21:00)
    [switch]$Go                            # 付けたときだけ実際にタップ。既定はドライラン
)

$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$SCR = Join-Path $env:TEMP "hgc_shoot"; if (-not (Test-Path $SCR)) { New-Item -ItemType Directory $SCR | Out-Null }

# --- 実測座標(device px 1080x2400)。レイアウトが変わったらここだけ直す ---
$C = @{
    rowSelectX = 240      # 行を選択(x)。yは planrows の Y
    rowRecX    = 52       # 行左のRECカチンコ=開始(x)
    edgeDrop   = @(400,1276)   # エッジ端末ドロップダウンを開く
    edgeNone   = @(190,1306)   # 開いた直後の先頭「無し(スマホで撮影)」
    endField   = @(954,826)    # 撮影終了 時刻フィールド
    kbIcon     = @(162,1614)   # 時刻ピッカー左下のキーボード入力アイコン
    hourField  = @(156,1316)   # 時フィールド(キーボード表示前の位置)。タップ→時入力→自動で分へ
    okBtn      = @(894,1124)   # ピッカーOK(ソフトキーボード表示中の位置)
    keepBtn    = @(894,1355)   # 「カメラが見つかりません」ダイアログの[継続]
    scrollTop  = @(540,900,540,1500)  # 詳細シートを先頭へ(下スワイプ)
}

function Tap($xy, $label) {
    Write-Host ("  tap {0,4},{1,4}  {2}" -f $xy[0], $xy[1], $label)
    if ($Go) { & $adb shell input tap $xy[0] $xy[1] }
}
function Shot($tag) { if ($Go) { & $adb exec-out screencap -p > (Join-Path $SCR "$tag.png") } }

# 1) 対象計画の行を特定(開始時刻降順で並ぶので名前で引く)
if (-not (Get-Command Get-Plans -ErrorAction SilentlyContinue)) { . (Join-Path $PSScriptRoot 'planrows.ps1') }
$p = Get-Plans | Where-Object Name -eq $Name
if (-not $p) { Write-Error "計画 '$Name' が見つかりません。Get-Plans で名前を確認してください。"; return }
$y = $p.Y
Write-Host ("計画 '{0}' Row={1} Y={2}  終了={3}  {4}" -f $Name, $p.Row, $y, $End, ($(if($Go){'[実行]'}else{'[ドライラン]'})))

if (-not ($End -match '^(\d{1,2}):(\d{2})$')) { Write-Error "End は HH:mm 形式で"; return }
$hh = $Matches[1]; $mm = $Matches[2]

# 2) 行を選択して詳細シートを先頭へ
Tap @($C.rowSelectX,$y) '行を選択'; if ($Go) { Start-Sleep -Milliseconds 800 }
if ($Go) { & $adb shell input swipe $C.scrollTop[0] $C.scrollTop[1] $C.scrollTop[2] $C.scrollTop[3]; Start-Sleep -Milliseconds 500 }
Shot '1_selected'

# 3) エッジ端末=無し(スマホ直結)
Tap $C.edgeDrop 'エッジ端末ドロップダウン'; if ($Go) { Start-Sleep -Milliseconds 700 }
Tap $C.edgeNone '「無し(スマホで撮影)」'; if ($Go) { Start-Sleep -Milliseconds 700 }
Shot '2_phonedirect'

# 4) 終了時刻を設定(キーボード入力: 時→自動で分)
Tap $C.endField '終了時刻フィールド'; if ($Go) { Start-Sleep -Milliseconds 800 }
Tap $C.kbIcon   'キーボード入力へ';   if ($Go) { Start-Sleep -Milliseconds 700 }
Tap $C.hourField '時フィールド';       if ($Go) { Start-Sleep -Milliseconds 500 }
Write-Host "  text $hh (時)"; if ($Go) { & $adb shell input text $hh; Start-Sleep -Milliseconds 500 }
Write-Host "  text $mm (分)"; if ($Go) { & $adb shell input text $mm; Start-Sleep -Milliseconds 500 }
Tap $C.okBtn 'OK'; if ($Go) { Start-Sleep -Milliseconds 800 }
Shot '3_endtime'

# 5) RECで開始
Tap @($C.rowRecX,$y) 'RECカチンコ=開始'; if ($Go) { Start-Sleep -Seconds 7 }
Shot '4_after_rec'

# 6) 「カメラが見つかりません」なら継続(初回探索のタイムアウトなだけのことが多い)
if ($Go) {
    if (Test-DialogOpen) {
        Write-Host "  → ダイアログ検出(おそらくカメラ未検出)。[継続]を押す"
        Tap $C.keepBtn '[継続]'; Start-Sleep -Seconds 6
    }
    Shot '5_final'
    # 7) 撮影確認
    Write-Host "`n--- HGCapture(直近) ---"
    (& $adb logcat -d -t 300 | Select-String 'HGCapture|見つかりません' | Select-Object -Last 6) | ForEach-Object { $_.Line }
    Write-Host "`nスクショ: $SCR"
    Write-Host "アイコンを確認: 緑カメラ=撮影中 / 黄色×=未検出。スマホ直結は21:00までアプリ前面・電源・Wi-Fi維持。"
} else {
    Write-Host "`n(ドライラン。実行は -Go を付ける)"
}
