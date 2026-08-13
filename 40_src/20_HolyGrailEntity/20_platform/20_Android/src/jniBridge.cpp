// Kotlin(UI) と holyGrailEntity(extern "C") を繋ぐ JNI ブリッジ。
// 通知コールバックはワーカースレッドから呼ばれるため、AttachCurrentThread して Java へ転送する。

#include <jni.h>
#include <string>
#include <vector>
#include <cstring>

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

// エッジとの BLE 通信は Android の API なので Kotlin 側にしかない。ネイティブ(edgeClient)から
// そこへ呼び返すために VM を共有する。extern "C" の外に置く(宣言は commonAndroid.h)。
JavaVM* hgeJavaVm(void) { return g_vm; }

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
Java_app_laxei_holygrail_HgeNative_nativeResumeCapture(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_resumeCapture();
}

// 遅延アームのポンプ(§7.4)。予約(将来窓)計画の開始スレッドを期日に生成する。UIタイマから数秒毎に呼ぶ。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativePump(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_pump();
}

// --- 並行撮影(計画id指定) ---
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCaptureStartPlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = id ? env->GetStringUTFChars(id, nullptr) : nullptr;
	jint r = hge_captureStartPlan(s ? s : "");
	if (s) { env->ReleaseStringUTFChars(id, s); }
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCaptureStopPlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = id ? env->GetStringUTFChars(id, nullptr) : nullptr;
	jint r = hge_captureStopPlan(s ? s : "");
	if (s) { env->ReleaseStringUTFChars(id, s); }
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetStatePlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = id ? env->GetStringUTFChars(id, nullptr) : nullptr;
	jint r = hge_getStatePlan(s ? s : "");
	if (s) { env->ReleaseStringUTFChars(id, s); }
	return r;
}

// スマホ直接撮影で NOCAMERA の計画に即再探索を促す(継続ボタン)。planId 空=全取得フェーズ。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativePokeAcquire(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = id ? env->GetStringUTFChars(id, nullptr) : nullptr;
	jint r = hge_pokeAcquire(s ? s : "");
	if (s) { env->ReleaseStringUTFChars(id, s); }
	return r;
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

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetPlanJson(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getPlanJson(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF(""); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getPlanJson(buf.data(), &len) != 0) { return env->NewStringUTF(""); }
	return env->NewStringUTF(buf.data());
}

// 撮影計画(cs)JSONを現在の編集計画へ復元する(変更の取り消し用)。保存はしない。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanJson(JNIEnv* env, jobject /*thiz*/, jstring json)
{
	const char* s = json ? env->GetStringUTFChars(json, nullptr) : nullptr;
	jint r = -1;
	if (s != nullptr) { r = hge_setPlanJson(s, static_cast<int32_t>(std::strlen(s))); env->ReleaseStringUTFChars(json, s); }
	return r;
}

// 撮影シミュレーション(画面360)。恒星リストを一度読み込む。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSimLoadStars(JNIEnv* env, jobject /*thiz*/, jstring json)
{
	const char* s = json ? env->GetStringUTFChars(json, nullptr) : nullptr;
	jint r = hge_simLoadStars(s ? s : "");
	if (s) { env->ReleaseStringUTFChars(json, s); }
	return r;
}

// 撮影シミュレーション。params から画角内の天体を投影した JSON を返す。
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSimulateSky(JNIEnv* env, jobject /*thiz*/, jstring params)
{
	const char* p = params ? env->GetStringUTFChars(params, nullptr) : nullptr;
	int32_t len = 0;
	hge_simulateSky(p ? p : "{}", nullptr, &len);
	if (len <= 0) { if (p) { env->ReleaseStringUTFChars(params, p); } return env->NewStringUTF("{\"objects\":[]}"); }
	std::vector<char> buf(static_cast<size_t>(len));
	hge_simulateSky(p ? p : "{}", buf.data(), &len);
	if (p) { env->ReleaseStringUTFChars(params, p); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSavePlan(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_savePlan();
}

// --- 複数撮影計画(§7.4) ---
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeListPlans(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_listPlansJson(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF("[]"); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_listPlansJson(buf.data(), &len) != 0) { return env->NewStringUTF("[]"); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeNewPlan(JNIEnv* env, jobject /*thiz*/, jstring presetName)
{
	const char* p = presetName ? env->GetStringUTFChars(presetName, nullptr) : nullptr;
	jint r = hge_newPlan(p ? p : "");
	if (p) { env->ReleaseStringUTFChars(presetName, p); }
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCopyPlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = env->GetStringUTFChars(id, nullptr);
	jint r = hge_copyPlan(s ? s : "");
	env->ReleaseStringUTFChars(id, s);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeDeletePlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = env->GetStringUTFChars(id, nullptr);
	jint r = hge_deletePlan(s ? s : "");
	env->ReleaseStringUTFChars(id, s);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSelectPlan(JNIEnv* env, jobject /*thiz*/, jstring id)
{
	const char* s = env->GetStringUTFChars(id, nullptr);
	jint r = hge_selectPlan(s ? s : "");
	env->ReleaseStringUTFChars(id, s);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeCurrentPlanId(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getCurrentPlanId(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF(""); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getCurrentPlanId(buf.data(), &len) != 0) { return env->NewStringUTF(""); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanTimes(JNIEnv* env, jobject /*thiz*/,
                                                      jstring start_, jstring end_, jint offMin)
{
	const char* s = env->GetStringUTFChars(start_, nullptr);
	const char* e = env->GetStringUTFChars(end_, nullptr);
	jint r = hge_setPlanTimes(s ? s : "", e ? e : "", offMin);
	env->ReleaseStringUTFChars(start_, s);
	env->ReleaseStringUTFChars(end_, e);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanDirection(JNIEnv* /*env*/, jobject /*thiz*/,
                                                          jdouble azimuth, jdouble elevation)
{
	return hge_setPlanDirection(azimuth, elevation);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanInterval(JNIEnv* /*env*/, jobject /*thiz*/, jdouble seconds)
{
	return hge_setPlanInterval(seconds);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanLandscape(JNIEnv* /*env*/, jobject /*thiz*/, jint landscape)
{
	return hge_setPlanLandscape(landscape);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanGearConst(JNIEnv* env, jobject /*thiz*/, jstring json_)
{
	const char* j = env->GetStringUTFChars(json_, nullptr);
	jint r = hge_setPlanGearConstJson(j ? j : "");
	env->ReleaseStringUTFChars(json_, j);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetBandMode(JNIEnv* /*env*/, jobject /*thiz*/,
                                                     jint sunriseMode, jint sunsetMode)
{
	return hge_setBandMode(sunriseMode, sunsetMode);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetBoundary(JNIEnv* env, jobject /*thiz*/,
                                                     jint beforeType, jint afterType, jint occ, jstring whenIso)
{
	const char* w = env->GetStringUTFChars(whenIso, nullptr);
	jint r = hge_setBoundary(beforeType, afterType, occ, w ? w : "");
	env->ReleaseStringUTFChars(whenIso, w);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetBoundaryByAlt(JNIEnv* /*env*/, jobject /*thiz*/,
                                                          jint beforeType, jint afterType, jint occ, jdouble altDeg, jint rising)
{
	return hge_setBoundaryByAlt(beforeType, afterType, occ, altDeg, rising);
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeClearScheduleEdits(JNIEnv* /*env*/, jobject /*thiz*/)
{
	return hge_clearScheduleEdits();
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetCcmDefaults(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getCcmDefaultsJson(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF(""); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getCcmDefaultsJson(buf.data(), &len) != 0) { return env->NewStringUTF(""); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetPlanCcm(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getPlanCcmJson(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF(""); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getPlanCcmJson(buf.data(), &len) != 0) { return env->NewStringUTF(""); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanCcm(JNIEnv* env, jobject /*thiz*/, jstring json_)
{
	const char* j = env->GetStringUTFChars(json_, nullptr);
	jint r = hge_setPlanCcmJson(j ? j : "", j ? static_cast<int32_t>(std::strlen(j)) : 0);
	env->ReleaseStringUTFChars(json_, j);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetExpoValues(JNIEnv* env, jobject /*thiz*/)
{
	int32_t len = 0;
	hge_getExpoValuesJson(nullptr, &len);
	if (len <= 0) { return env->NewStringUTF("{}"); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_getExpoValuesJson(buf.data(), &len) != 0) { return env->NewStringUTF("{}"); }
	return env->NewStringUTF(buf.data());
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSunAltitudeTimes(JNIEnv* env, jobject /*thiz*/, jint altitudeDeg)
{
	int32_t len = 0;
	hge_sunAltitudeTimes(altitudeDeg, nullptr, &len);
	if (len <= 0) { return env->NewStringUTF("{}"); }
	std::vector<char> buf(static_cast<size_t>(len));
	if (hge_sunAltitudeTimes(altitudeDeg, buf.data(), &len) != 0) { return env->NewStringUTF("{}"); }
	return env->NewStringUTF(buf.data());
}

// --- 機材マスタ・所持機材(データ構造仕様書43 §5.5〜5.9 / §7.6) ---
namespace
{
	// hge の「バッファ規約」getter を呼んで Java String にする共通処理。
	jstring callBufGetter(JNIEnv* env, int32_t (*fn)(char*, int32_t*))
	{
		int32_t len = 0;
		fn(nullptr, &len);
		if (len <= 0) { return env->NewStringUTF("[]"); }
		std::vector<char> buf(static_cast<size_t>(len));
		if (fn(buf.data(), &len) != 0) { return env->NewStringUTF("[]"); }
		return env->NewStringUTF(buf.data());
	}
	// name 引数1つの hge コマンドを呼ぶ共通処理。
	jint callNameCmd(JNIEnv* env, jstring name_, int32_t (*fn)(const char*))
	{
		if (name_ == nullptr) { return -1; }
		const char* n = env->GetStringUTFChars(name_, nullptr);
		jint r = fn(n ? n : "");
		env->ReleaseStringUTFChars(name_, n);
		return r;
	}
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetMasterCameras(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getMasterCamerasJson); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetMasterLenses(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getMasterLensesJson); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetOwnedCameras(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getOwnedCamerasJson); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetOwnedLenses(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getOwnedLensesJson); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeAddOwnedCamera(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_addOwnedCamera); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeAddOwnedLens(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_addOwnedLens); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRemoveOwnedCamera(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_removeOwnedCamera); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRemoveOwnedLens(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_removeOwnedLens); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetOwnedCameraAutoInsert(JNIEnv* env, jobject /*thiz*/,
                                                                  jstring name, jint autoInsert)
{
	if (name == nullptr) { return -1; }
	const char* n = env->GetStringUTFChars(name, nullptr);
	jint r = hge_setOwnedCameraAutoInsert(n ? n : "", autoInsert);
	env->ReleaseStringUTFChars(name, n);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanCamera(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_setPlanCamera); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanLocation(JNIEnv* env, jobject /*thiz*/, jdouble lat, jdouble lng, jstring name)
{
	const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
	jint r = hge_setPlanLocation(static_cast<double>(lat), static_cast<double>(lng), n);
	if (n) { env->ReleaseStringUTFChars(name, n); }
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRenamePlan(JNIEnv* env, jobject /*thiz*/, jstring id, jstring name)
{
	const char* i = id   ? env->GetStringUTFChars(id, nullptr)   : nullptr;
	const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
	jint r = hge_renamePlan(i ? i : "", n ? n : "");
	if (i) { env->ReleaseStringUTFChars(id, i); }
	if (n) { env->ReleaseStringUTFChars(name, n); }
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanLens(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_setPlanLens); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetOwnedCameraDetail(JNIEnv* env, jobject /*thiz*/, jstring orig, jstring json)
{
	const char* o = env->GetStringUTFChars(orig, nullptr);
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setOwnedCameraDetail(o ? o : "", j ? j : "");
	env->ReleaseStringUTFChars(orig, o);
	env->ReleaseStringUTFChars(json, j);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetOwnedLensDetail(JNIEnv* env, jobject /*thiz*/, jstring orig, jstring json)
{
	const char* o = env->GetStringUTFChars(orig, nullptr);
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setOwnedLensDetail(o ? o : "", j ? j : "");
	env->ReleaseStringUTFChars(orig, o);
	env->ReleaseStringUTFChars(json, j);
	return r;
}

// --- 撮影場所(§7.9) ---
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetPlaces(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getPlacesJson); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeAddPlace(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_addPlace); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRemovePlace(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_removePlace); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlaceAutoInsert(JNIEnv* env, jobject /*thiz*/, jstring name, jint autoInsert)
{
	if (name == nullptr) { return -1; }
	const char* n = env->GetStringUTFChars(name, nullptr);
	jint r = hge_setPlaceAutoInsert(n ? n : "", autoInsert);
	env->ReleaseStringUTFChars(name, n);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlaceDetail(JNIEnv* env, jobject /*thiz*/, jstring orig, jstring json)
{
	const char* o = env->GetStringUTFChars(orig, nullptr);
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setPlaceDetail(o ? o : "", j ? j : "");
	env->ReleaseStringUTFChars(orig, o);
	env->ReleaseStringUTFChars(json, j);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPlanPlace(JNIEnv* env, jobject /*thiz*/, jstring name)
{ return callNameCmd(env, name, hge_setPlanPlace); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSearchDevicesList(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_searchDevicesListJson); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativePresenceStart(JNIEnv* /*env*/, jobject /*thiz*/)
{ return hge_presenceStart(); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativePresenceStop(JNIEnv* /*env*/, jobject /*thiz*/)
{ return hge_presenceStop(); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativePresenceJson(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_presenceJson); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetColors(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getColorsJson); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetColors(JNIEnv* env, jobject /*thiz*/, jstring json)
{
	if (json == nullptr) { return -1; }
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setColorsJson(j ? j : "");
	env->ReleaseStringUTFChars(json, j);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetSmoothing(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_getSmoothingJson); }

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetSmoothing(JNIEnv* env, jobject /*thiz*/, jstring json)
{
	if (json == nullptr) { return -1; }
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setSmoothingJson(j ? j : "");
	env->ReleaseStringUTFChars(json, j);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativePruneOldLogs(JNIEnv* /*env*/, jobject /*thiz*/, jint offMin)
{ return hge_pruneOldLogs(offMin); }

// --- 撮影レポート ---
JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeReportList(JNIEnv* env, jobject /*thiz*/)
{ return callBufGetter(env, hge_reportListJson); }

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeReportJson(JNIEnv* env, jobject /*thiz*/, jstring name)
{
	if (name == nullptr) { return env->NewStringUTF(""); }
	const char* n = env->GetStringUTFChars(name, nullptr);
	int32_t len = 0;
	hge_reportJson(n ? n : "", nullptr, &len);
	std::string out;
	if (len > 0) { std::vector<char> buf(static_cast<size_t>(len)); if (hge_reportJson(n ? n : "", buf.data(), &len) == 0) { out = buf.data(); } }
	env->ReleaseStringUTFChars(name, n);
	return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRemoveReport(JNIEnv* env, jobject /*thiz*/, jstring name)
{
	if (name == nullptr) { return -1; }
	const char* n = env->GetStringUTFChars(name, nullptr);
	jint r = hge_removeReport(n ? n : "");
	env->ReleaseStringUTFChars(name, n);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetCcmPresets(JNIEnv* env, jobject /*thiz*/, jstring type)
{
	const char* t = env->GetStringUTFChars(type, nullptr);
	int32_t len = 0;
	hge_getCcmPresetsJson(t ? t : "", nullptr, &len);
	std::string out = "[]";
	if (len > 0) { std::vector<char> buf(static_cast<size_t>(len)); if (hge_getCcmPresetsJson(t ? t : "", buf.data(), &len) == 0) { out = buf.data(); } }
	env->ReleaseStringUTFChars(type, t);
	return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetCcmPreset(JNIEnv* env, jobject /*thiz*/, jstring type, jstring orig, jstring json)
{
	const char* t = env->GetStringUTFChars(type, nullptr);
	const char* o = env->GetStringUTFChars(orig, nullptr);
	const char* j = env->GetStringUTFChars(json, nullptr);
	jint r = hge_setCcmPreset(t ? t : "", o ? o : "", j ? j : "");
	env->ReleaseStringUTFChars(type, t); env->ReleaseStringUTFChars(orig, o); env->ReleaseStringUTFChars(json, j);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRemoveCcmPreset(JNIEnv* env, jobject /*thiz*/, jstring type, jstring name)
{
	const char* t = env->GetStringUTFChars(type, nullptr);
	const char* n = env->GetStringUTFChars(name, nullptr);
	jint r = hge_removeCcmPreset(t ? t : "", n ? n : "");
	env->ReleaseStringUTFChars(type, t); env->ReleaseStringUTFChars(name, n);
	return r;
}

JNIEXPORT jstring JNICALL
Java_app_laxei_holygrail_HgeNative_nativeGetPreferredCcm(JNIEnv* env, jobject /*thiz*/, jstring type)
{
	const char* t = env->GetStringUTFChars(type, nullptr);
	int32_t len = 0;
	hge_getPreferredCcm(t ? t : "", nullptr, &len);
	std::string out;
	if (len > 0) { std::vector<char> buf(static_cast<size_t>(len)); if (hge_getPreferredCcm(t ? t : "", buf.data(), &len) == 0) { out = buf.data(); } }
	env->ReleaseStringUTFChars(type, t);
	return env->NewStringUTF(out.c_str());
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeSetPreferredCcm(JNIEnv* env, jobject /*thiz*/, jstring type, jstring name)
{
	const char* t = env->GetStringUTFChars(type, nullptr);
	const char* n = env->GetStringUTFChars(name, nullptr);
	jint r = hge_setPreferredCcm(t ? t : "", n ? n : "");
	env->ReleaseStringUTFChars(type, t); env->ReleaseStringUTFChars(name, n);
	return r;
}

JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeAddOwnedDetected(JNIEnv* /*env*/, jobject /*thiz*/, jint index)
{ return hge_addOwnedDetected(index); }

// 発見/接続したカメラ識別情報を所持カメラへ反映する。allowAdd=1で未一致は自動追加、0なら追加せず区分のみ返す。
JNIEXPORT jint JNICALL
Java_app_laxei_holygrail_HgeNative_nativeRecordCameraIdentity(JNIEnv* env, jobject /*thiz*/, jstring model, jstring serial, jstring friendly, jboolean allowAdd)
{
	const char* m = model    ? env->GetStringUTFChars(model,    nullptr) : nullptr;
	const char* s = serial   ? env->GetStringUTFChars(serial,   nullptr) : nullptr;
	const char* f = friendly ? env->GetStringUTFChars(friendly, nullptr) : nullptr;
	jint r = hge_recordCameraIdentity(m ? m : "", s ? s : "", f ? f : "", allowAdd ? 1 : 0);
	if (m) { env->ReleaseStringUTFChars(model,    m); }
	if (s) { env->ReleaseStringUTFChars(serial,   s); }
	if (f) { env->ReleaseStringUTFChars(friendly, f); }
	return r;
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
