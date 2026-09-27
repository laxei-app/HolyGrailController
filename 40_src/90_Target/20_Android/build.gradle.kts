// HolyGrail Controller Android (開発ステップ2.1 MVP) ルートビルド。
plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
    // Firebase(2026-09-27)。google-services は app/google-services.json を読んで
    //  プロジェクトの設定をビルドへ埋める。crashlytics はネイティブのシンボルを上げる役。
    id("com.google.gms.google-services") version "4.4.2" apply false
    id("com.google.firebase.crashlytics") version "3.0.2" apply false
}
