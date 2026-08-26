import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// アプリのバージョン(2026-08-08 UI依頼)。
//  major/minor は 40_src/version.properties でエッジ端末と共有し、手で更新する。
//  phonePatch はスマホをビルドするたびに +1 する(パッチはエッジと独立に進む)。
// 版数を上げるのは実際に成果物を作るときだけにしたいので、タスク名に assemble/install/build を
// 含むときだけ加算する(./gradlew tasks 等では動かさない)。
val versionPropsFile = file("../../../version.properties")
fun readVersionProps(): Properties {
    val p = Properties()
    versionPropsFile.inputStream().use { p.load(it) }
    return p
}
fun bumpPhonePatch(): Int {
    val p = readVersionProps()
    var patch = (p.getProperty("phonePatch") ?: "0").trim().toInt()
    val building = gradle.startParameter.taskNames.any {
        it.contains("assemble", true) || it.contains("install", true) || it.contains("build", true)
    }
    if (building) {
        patch += 1
        // コメントを保ちたいので Properties.store ではなく該当行だけ置換する。
        val text = versionPropsFile.readText(Charsets.UTF_8)
            .replace(Regex("(?m)^phonePatch[ \t]*=.*$"), "phonePatch=$patch")
        versionPropsFile.writeText(text, Charsets.UTF_8)
    }
    return patch
}
val vProps = readVersionProps()
val hgcMajor = (vProps.getProperty("major") ?: "0").trim().toInt()
val hgcMinor = (vProps.getProperty("minor") ?: "0").trim().toInt()
val hgcPatch = bumpPhonePatch()
val hgcVersionName = "$hgcMajor.$hgcMinor.$hgcPatch"

android {
    namespace = "app.laxei.holygrail"
    compileSdk = 34

    // インストール済みNDK(29.0.14206865)を使う。未指定だとAGP既定版を探し
    // 「NDK not installed」になるため固定する。
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "app.laxei.holygrail"
        minSdk = 26
        targetSdk = 34
        // versionCode は必ず単調増加させる。パッチだけを使うと、マイナーを上げて
        // パッチを 0 に戻したときに番号が下がり、端末が INSTALL_FAILED_VERSION_DOWNGRADE で
        // 受け付けなくなる(0.1.0 の versionCode=1 < 既存 0.0.17 の 17)。
        versionCode = (hgcMajor * 1000000 + hgcMinor * 1000 + hgcPatch).coerceAtLeast(1)
        versionName = hgcVersionName

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                // astronomy.c は fast-math 不可のため最適化フラグに注意
            }
        }
        ndk {
            // エミュレータ(x86_64)と実機(arm64)を対象
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Kotlin/UI ソース・UIリソースは 40_src/10_UI/20_Android に置く(ユーザー指定)。
    // ビルド対象(90_Target)には AndroidManifest.xml と cpp(CMake) のみを残す。
    sourceSets["main"].java.srcDirs("../../../10_UI/20_Android")
    sourceSets["main"].res.setSrcDirs(listOf("../../../10_UI/20_Android/res"))

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("com.jaredrummler:colorpicker:1.1.0")   // 色の設定(文字/背景)のカラーピッカー
    implementation("com.google.android.gms:play-services-code-scanner:16.1.0")  // エッジ設定QR(PoP)スキャン §8.2.2(GMSコードスキャナ。Camera2ベースで実機堅牢)
    implementation("org.osmdroid:osmdroid-android:6.1.20")  // 撮影場所の地図選択(OpenStreetMap。APIキー不要・無料)

    // USB書き込み(EspFlasher)の枠組み・パケット組み立て・圧縮を、実機もスマホも使わずに
    //  確かめるための単体試験。テストは app/src/test/java 配下(既定の場所)に置く。
    testImplementation("junit:junit:4.13.2")
    // 単体試験では android.jar が中身の無い差し替え版になり org.json が例外を投げるので、
    //  本物を試験の側だけに載せる(目録の読み取りを実機なしで確かめるため)。
    testImplementation("org.json:json:20240303")
}
