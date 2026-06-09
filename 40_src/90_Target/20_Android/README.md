# HolyGrail Controller — Android (開発ステップ2.1 MVP)

固定データの撮影計画で EOS-R10 を CCAPI 制御し、HolyGrail タイムラプス撮影の開始/停止を行う最小実装。

## 構成
- **プロジェクト/ビルド**: `40_src/90_Target/20_Android`(このフォルダ)
- **Kotlin/UI ソース**: `40_src/10_UI/20_Android`(`HgeNative.kt` / `MainActivity.kt`)
- **ネイティブ(C++)**:
  - Entity 共通部: `40_src/20_HolyGrailEntity/10_common/src`
  - Android プラットフォーム層: `40_src/20_HolyGrailEntity/20_platform/20_Android/src`(net/ossc/tool/debugOut/jniBridge)
  - 天文ライブラリ: `40_src/20_HolyGrailEntity/10_common/lib/astronomy`
  - `.so` ビルドは `app/src/main/cpp/CMakeLists.txt`

## ビルド・実行
1. Android Studio で **このフォルダ(`40_src/90_Target/20_Android`)を開く**。
2. SDK Manager で **NDK** と **CMake** を導入(externalNativeBuild が使用)。
3. エミュレータ(x86_64)または実機(arm64)を選択。
4. Run。アプリ起動後「撮影開始」で固定データのスケジュールに沿って撮影する。

## テスト時の注意
- カメラ(EOS-R10)を **PC と同一LAN** に接続し CCAPI を有効化しておく(エミュレータから到達可能にする)。
- CCAPI は平文HTTPのため `usesCleartextTraffic="true"` を設定済み。
- 固定撮影計画は「開始=現在時刻、終了=2時間後」で生成される(`holyGrailEntity.cpp` の `loadFixedPlanImpl`)。場所は東京、機材は EOS-R10 + 16mm の既定値。

## 既知の制約(MVP)
- 撮影計画は固定データ。画面は330/400相当を1画面に簡略化。
- ライブビュー測光と静止画露出の対応はカメラ実機での較正が必要(自動露出の初期補正の仮定あり)。
- リニア移行は夜間固定露出へ 1/3 段ずつ収束する簡易実装。
- 「月の影響への対処」は未実装(ステップ2.1の対象外)。
