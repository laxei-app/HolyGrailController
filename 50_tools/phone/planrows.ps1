# スマホUI実機テスト用ヘルパー(セッションをまたいで再利用する)
#
# 【なぜ必要か】
#  ・撮影計画リストは「開始時刻の降順」に並び替わる。座標決め打ちでタップすると、時刻を変更した
#    直後などに順序が入れ替わり、意図した計画と別の行を操作してしまう。
#    実際にこれで検証結果を2度無効化し、存在しない不具合(「エッジ設定が勝手に戻る」)を疑ってしまった。
#  ・uiautomator dump は撮影中のアイコン点滅などで "could not get idle state" になり頻繁に失敗する。
#  → 端末上の plan_*.json から開始時刻を読み、行位置を自前で算出する。
#
# 使い方:
#   . .\planrows.ps1
#   Get-Plans | Format-Table Row,Name,Start,Y
#   $r = (Get-Plans | Where-Object Name -eq 'テストr10')
#   & $adb shell input tap 52 $r.Y      # 行左の開始/停止アイコン
#   & $adb shell input tap 240 $r.Y     # 行を選択
#   & $adb shell input tap 1026 $r.Y    # 行右の ⋮ メニュー
$adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$PLANDIR = "/sdcard/Android/data/app.laxei.holygrail/files/plan"
$ROW0 = 386      # 1行目の中心Y(device px, 1080x2400)
$PITCH = 96      # 行の高さ

function Get-Plans {
    $files = (& $adb shell ls $PLANDIR) -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -like 'plan_*.json' }
    $list = @()
    foreach ($f in $files) {
        $j = (& $adb exec-out cat "$PLANDIR/$f") -join ''
        $nm = [regex]::Match($j, '"name":"([^"]*)","nightFixedExposure"')
        if (-not $nm.Success) { $nm = [regex]::Match($j, '"name":"([^"]*)"[^}]*"nightFixed') }
        $all = [regex]::Matches($j, '"start":\{"day":(\d+),"hour":(\d+),"min":(\d+),"month":(\d+),"sec":(\d+),"year":(\d+)\}')
        if ($all.Count -eq 0) { continue }
        $s = $all[$all.Count - 1]   # plan.start(最後に現れるもの)が計画の開始時刻
        $dt = Get-Date -Year $s.Groups[6].Value -Month $s.Groups[4].Value -Day $s.Groups[1].Value `
                       -Hour $s.Groups[2].Value -Minute $s.Groups[3].Value -Second 0
        $list += [pscustomobject]@{ File = $f; Name = $nm.Groups[1].Value; Start = $dt }
    }
    $i = 0
    $list | Sort-Object Start -Descending | ForEach-Object {   # 画面は降順(実機で確認)
        $_ | Add-Member -NotePropertyName Row -NotePropertyValue $i -PassThru |
             Add-Member -NotePropertyName Y -NotePropertyValue ($ROW0 + $PITCH * $i) -PassThru
        $i++
    }
}

# ダイアログが出ているかを「画面のディム(暗転)」で判定する。
#  uiautomator dump が不安定なとき用。タイトルバーの青が暗くなることを利用する。
#  実測: ダイアログ無し RGB=(48,99,186) / あり RGB=(33,67,126) → 青成分 150 を閾値にする。
function Test-DialogOpen {
    Add-Type -AssemblyName System.Drawing
    $tmp = Join-Path $env:TEMP "hgc_dlgchk.png"
    & $adb exec-out screencap -p > $tmp
    $b = [System.Drawing.Bitmap]::FromFile($tmp)
    $p = $b.GetPixel(450, 180)
    $b.Dispose()
    return ($p.B -lt 150)
}
