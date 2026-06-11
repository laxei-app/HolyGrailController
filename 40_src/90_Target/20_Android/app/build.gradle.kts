plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

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
        versionCode = 1
        versionName = "0.1"

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

    // Kotlin/UI ソースは 40_src/10_UI/20_Android に置く(ユーザー指定)
    sourceSets["main"].java.srcDirs("../../../10_UI/20_Android")
    // UI リソース(VectorDrawable等)も UI フォルダ配下に置く。既定の res にも残す(レイアウト等)。
    sourceSets["main"].res.srcDir("../../../10_UI/20_Android/res")

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
}
