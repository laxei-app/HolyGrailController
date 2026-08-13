#ifndef _COMMON_ANDROID_H_
#define _COMMON_ANDROID_H_
// Android(NDK) プラットフォーム共通ヘッダ。

#include <android/log.h>

#define HGE_LOG_TAG "HolyGrail"

#include <jni.h>
// JNI_OnLoad で受け取った JavaVM。ネイティブから Kotlin を呼び返すのに使う
// (エッジとの BLE 通信は Android の API なので Kotlin 側にしか書けない)。実体は jniBridge.cpp。
JavaVM* hgeJavaVm(void);

#endif // _COMMON_ANDROID_H_
