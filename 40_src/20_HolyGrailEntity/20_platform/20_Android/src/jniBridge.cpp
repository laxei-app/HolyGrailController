// Kotlin(UI) と holyGrailEntity(extern "C") を繋ぐ JNI ブリッジ。
// 通知コールバックはワーカースレッドから呼ばれるため、AttachCurrentThread して Java へ転送する。

#include <jni.h>
#include <string>
#include <vector>

#include "holyGrailEntity.h"
#include "osFile.h"
#include "commonAndroid.h"

namespace
{
	JavaVM*   g_vm = nullptr;
	jobject   g_listener = nullptr;	// HgeListener のグローバル参照
	jmethodID g_onEvent  = nullptr;	// void onHgeEvent(int event, String json)

	// Entity からの通知 → Java の listener へ転送
	void bridgeNotify(int32_t event, const char* json, int32_t /*len*/, void* /*user*/)
	{
		if (g_vm == nullptr || g_listener == nullptr || g_onEvent == nullptr) { return; }

		JNIEnv* env = nullptr;
		bool attached = false;
		if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
		{
			if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { return; }
			attached = true;
		}
		jstring js = env->NewStringUTF(json ? json : "");
		env->CallVoidMethod(g_listener, g_onEvent, static_cast<jint>(event), js);
		env->DeleteLocalRef(js);
		if (env->ExceptionCheck()) { env->ExceptionClear(); }
		if (attached) { g_vm->DetachCurrentThread(); }
	}
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
	g_vm = vm;
	return JNI_VERSION_1_6;
}

// ログ保存先(アプリ外部ファイル領域)を設定する。hge_init より前に呼ぶこと。
JNIEXPORT void JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetLogDir(JNIEnv* env, jobject /*thiz*/, jstring dir)
{
	if (dir == nullptr) { return; }
	const char* d = env->GetStringUTFChars(dir, nullptr);
	osfile::setBaseDir(d ? d : "");
	env->ReleaseStringUTFChars(dir, d);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeInit(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_init();
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeTerm(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_term();
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeVersion(JNIEnv* env, jobject /*thiz*/)
{
	return env->NewStringUTF(hge_version());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCaptureStart(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_captureStart();
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCaptureStop(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_captureStop();
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetState(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_getState();
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSearchDevices(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_searchDevices();
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeConnectManual(JNIEnv* env, jobject /*thiz*/, jstring host)
{
	if (host == nullptr) { return -1; }
	const char* h = env->GetStringUTFChars(host, nullptr);
	jint r = hge_connectManual(h);
	env->ReleaseStringUTFChars(host, h);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeScheduleJson(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getScheduleJson(nullptr, &len);		// 必要バイト数を取得
	if (len <= 0) { return env->NewStringUTF(""); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getScheduleJson(buf.data(), &len) != 0) { return env->NewStringUTF(""); }
	return env->NewStringUTF(buf.data());
}

// listener(HgeListener) を登録/解除する。
JNIEXPORT void JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetListener(JNIEnv* env, jobject /*thiz*/, jobject listener)
{
	if (g_listener != nullptr)
	{
		env->DeleteGlobalRef(g_listener);
		g_listener = nullptr;
		g_onEvent = nullptr;
	}
	if (listener != nullptr)
	{
		g_listener = env->NewGlobalRef(listener);
		jclass cls = env->GetObjectClass(listener);
		g_onEvent = env->GetMethodID(cls, "onHgeEvent", "(ILjava/lang/String;)V");
		hge_setNotify(bridgeNotify, nullptr);
	}
	else
	{
		hge_setNotify(nullptr, nullptr);
	}
}

} // extern "C"
