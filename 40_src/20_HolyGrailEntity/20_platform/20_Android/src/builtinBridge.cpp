#include "builtinBridge.h"
#include <jni.h>
#include "commonAndroid.h"	// hgeJavaVm(JNI_OnLoad が覚えた VM を借りる)

namespace
{
	const char* kCls = "app/laxei/holygrail/HgeNative";
	jclass g_cls = nullptr;	// 大域参照。bindClass が Java 側のスレッドで作る

	// 呼び返しの定型。attach と後始末をここへ閉じ込める(edgeClient.cpp と同じ形)。
	class attach
	{
	public:
		JNIEnv* env = nullptr;
		jclass  cls = nullptr;
		attach(void)
		{
			JavaVM* vm = hgeJavaVm();
			if (vm == nullptr) { return; }
			if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK)
			{
				if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { env = nullptr; return; }
				attached_ = true;
			}
			// 捕まえてあればそれを使う。無いときだけ FindClass を試す
			//  (Java から呼ばれた流れならこれでも見つかる)。
			if (g_cls != nullptr) { cls = g_cls; return; }
			cls = env->FindClass(kCls);
			if (env->ExceptionCheck()) { env->ExceptionClear(); cls = nullptr; }
			local_ = (cls != nullptr);
		}
		~attach()
		{
			if (env != nullptr && cls != nullptr && local_) { env->DeleteLocalRef(cls); }
			if (attached_) { JavaVM* vm = hgeJavaVm(); if (vm != nullptr) { vm->DetachCurrentThread(); } }
		}
		bool ok(void) const { return env != nullptr && cls != nullptr; }
	private:
		bool attached_ = false;
		bool local_    = false;	// cls が局所参照なら壊す(大域参照は残す)
	};

	// String を返す静的メソッドを呼ぶ。例外や null は fallback を返す。
	std::string callString(const char* name, const char* sig, jstring arg, const char* fallback)
	{
		attach a;
		if (!a.ok()) { return fallback; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, name, sig);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return fallback; }
		jobject r = (arg != nullptr)
		          ? a.env->CallStaticObjectMethod(a.cls, mid, arg)
		          : a.env->CallStaticObjectMethod(a.cls, mid);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
		if (arg != nullptr) { a.env->DeleteLocalRef(arg); }
		if (r == nullptr) { return fallback; }
		const char* p = a.env->GetStringUTFChars(static_cast<jstring>(r), nullptr);
		std::string out = (p != nullptr) ? p : fallback;
		if (p != nullptr) { a.env->ReleaseStringUTFChars(static_cast<jstring>(r), p); }
		a.env->DeleteLocalRef(r);
		return out;
	}
}

namespace builtinCam
{
	void bindClass(void* envp)
	{
		if (g_cls != nullptr || envp == nullptr) { return; }
		JNIEnv* env = static_cast<JNIEnv*>(envp);
		jclass c = env->FindClass(kCls);
		if (env->ExceptionCheck()) { env->ExceptionClear(); c = nullptr; }
		if (c == nullptr) { return; }
		g_cls = static_cast<jclass>(env->NewGlobalRef(c));
		env->DeleteLocalRef(c);
	}

	std::string listJson(void)
	{
		return callString("builtinList", "()Ljava/lang/String;", nullptr, "[]");
	}

	std::string physicalsJson(void)
	{
		return callString("builtinPhysicals", "()Ljava/lang/String;", nullptr, "[]");
	}

	std::string describeJson(const std::string& id)
	{
		attach a;
		if (!a.ok()) { return "{}"; }
		jstring js = a.env->NewStringUTF(id.c_str());
		// callString が arg を消費する。ここで attach を二重に作らないよう直に書く。
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "builtinDescribe",
		                                         "(Ljava/lang/String;)Ljava/lang/String;");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		std::string out = "{}";
		if (mid != nullptr)
		{
			jobject r = a.env->CallStaticObjectMethod(a.cls, mid, js);
			if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
			if (r != nullptr)
			{
				const char* p = a.env->GetStringUTFChars(static_cast<jstring>(r), nullptr);
				if (p != nullptr) { out = p; a.env->ReleaseStringUTFChars(static_cast<jstring>(r), p); }
				a.env->DeleteLocalRef(r);
			}
		}
		a.env->DeleteLocalRef(js);
		return out;
	}

	std::string open(const std::string& logicalId, const std::string& physId)
	{
		attach a;
		if (!a.ok()) { return "jni not ready"; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "builtinOpen",
		                 "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return "builtinOpen not found"; }
		jstring jl = a.env->NewStringUTF(logicalId.c_str());
		jstring jp = a.env->NewStringUTF(physId.c_str());
		jobject r  = a.env->CallStaticObjectMethod(a.cls, mid, jl, jp);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
		std::string out = "open failed";
		if (r != nullptr)
		{
			const char* p = a.env->GetStringUTFChars(static_cast<jstring>(r), nullptr);
			if (p != nullptr) { out = p; a.env->ReleaseStringUTFChars(static_cast<jstring>(r), p); }
			a.env->DeleteLocalRef(r);
		}
		a.env->DeleteLocalRef(jl); a.env->DeleteLocalRef(jp);
		return out;
	}

	std::string activePhysicalId(void)
	{
		return callString("builtinActivePhysical", "()Ljava/lang/String;", nullptr, "");
	}

	void close(void)
	{
		attach a;
		if (!a.ok()) { return; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "builtinClose", "()V");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return; }
		a.env->CallStaticVoidMethod(a.cls, mid);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); }
	}

	bool capture(const std::string& logicalId, const std::string& physId,
	             int iso, long long expNs, double aperture, int timeoutMs)
	{
		attach a;
		if (!a.ok()) { return false; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "builtinCapture",
		                 "(Ljava/lang/String;Ljava/lang/String;IJDI)Z");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return false; }
		jstring jl = a.env->NewStringUTF(logicalId.c_str());
		jstring jp = a.env->NewStringUTF(physId.c_str());
		jboolean r = a.env->CallStaticBooleanMethod(a.cls, mid, jl, jp,
		                                            static_cast<jint>(iso),
		                                            static_cast<jlong>(expNs),
		                                            static_cast<jdouble>(aperture),
		                                            static_cast<jint>(timeoutMs));
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = JNI_FALSE; }
		a.env->DeleteLocalRef(jl); a.env->DeleteLocalRef(jp);
		return r == JNI_TRUE;
	}

	std::string videoStart(const std::string& path, int fps)
	{
		attach a;
		if (!a.ok()) { return "jni not ready"; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "videoStart",
		                                         "(Ljava/lang/String;I)Ljava/lang/String;");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return "videoStart not found"; }
		jstring js = a.env->NewStringUTF(path.c_str());
		jobject r  = a.env->CallStaticObjectMethod(a.cls, mid, js, static_cast<jint>(fps));
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
		std::string out = "video start failed";
		if (r != nullptr)
		{
			const char* p = a.env->GetStringUTFChars(static_cast<jstring>(r), nullptr);
			if (p != nullptr) { out = p; a.env->ReleaseStringUTFChars(static_cast<jstring>(r), p); }
			a.env->DeleteLocalRef(r);
		}
		a.env->DeleteLocalRef(js);
		return out;
	}

	bool videoAddJpeg(const std::vector<uint8_t>& jpeg)
	{
		if (jpeg.empty()) { return false; }
		attach a;
		if (!a.ok()) { return false; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "videoAddJpeg", "([B)Z");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return false; }
		jbyteArray ja = a.env->NewByteArray(static_cast<jsize>(jpeg.size()));
		a.env->SetByteArrayRegion(ja, 0, static_cast<jsize>(jpeg.size()),
		                          reinterpret_cast<const jbyte*>(jpeg.data()));
		jboolean r = a.env->CallStaticBooleanMethod(a.cls, mid, ja);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = JNI_FALSE; }
		a.env->DeleteLocalRef(ja);
		return r == JNI_TRUE;
	}

	std::string videoFinish(void)
	{
		attach a;
		if (!a.ok()) { return ""; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "videoFinish", "()Ljava/lang/String;");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return ""; }
		jobject r = a.env->CallStaticObjectMethod(a.cls, mid);
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
		std::string out;
		if (r != nullptr)
		{
			const char* p = a.env->GetStringUTFChars(static_cast<jstring>(r), nullptr);
			if (p != nullptr) { out = p; a.env->ReleaseStringUTFChars(static_cast<jstring>(r), p); }
			a.env->DeleteLocalRef(r);
		}
		return out;
	}

	bool takeImage(int timeoutMs, std::vector<uint8_t>& out)
	{
		out.clear();
		attach a;
		if (!a.ok()) { return false; }
		jmethodID mid = a.env->GetStaticMethodID(a.cls, "builtinTakeImage", "(I)[B");
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); mid = nullptr; }
		if (mid == nullptr) { return false; }
		jobject r = a.env->CallStaticObjectMethod(a.cls, mid, static_cast<jint>(timeoutMs));
		if (a.env->ExceptionCheck()) { a.env->ExceptionClear(); r = nullptr; }
		if (r == nullptr) { return false; }
		jbyteArray ba = static_cast<jbyteArray>(r);
		const jsize n = a.env->GetArrayLength(ba);
		if (n > 0)
		{
			out.resize(static_cast<size_t>(n));
			a.env->GetByteArrayRegion(ba, 0, n, reinterpret_cast<jbyte*>(out.data()));
		}
		a.env->DeleteLocalRef(r);
		return !out.empty();
	}
}
