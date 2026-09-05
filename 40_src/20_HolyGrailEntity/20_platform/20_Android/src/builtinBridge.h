#ifndef _BUILTIN_BRIDGE_H_
#define _BUILTIN_BRIDGE_H_
// スマホ内蔵カメラ(Camera2)への呼び返し。Camera2 は Kotlin にしか無いので、
// HgeNative の静的メソッドへ JNI で渡す(BLE の edgeClient.cpp と同じ形)。
//
// ここは**素通しの窓口**であり、判断は一切しない。露出の決め方も測光の解釈も
// apiBuiltin 側に置く。Android 以外ではこのファイルごとビルドされない。
#include <cstdint>
#include <string>
#include <vector>

namespace builtinCam
{
	// 【クラスは Java から呼ばれたときに捕まえる(2026-09-05 実機で判明)】
	//  ネイティブスレッドから FindClass すると、そのスレッドにはアプリのクラスローダが
	//  無いので**アプリのクラスが見つからない**(システムのローダしか使えない)。
	//  探索は撮影とは別のスレッドで走るため、そのままでは常に空振りしていた。
	//  Java から呼ばれる入口(nativeInit)で捕まえて大域参照に持つ。
	void bindClass(void* env);	// JNIEnv*。JNI に依存させないため void* で受ける


	// 端末が持つカメラの一覧。JSON 配列 [{"id","name","facing"}]。取れなければ "[]"。
	std::string listJson(void);
	// 1台の諸元。JSON。取れなければ "{}"。
	std::string describeJson(const std::string& id);
	// 開く。"" =成功、それ以外は理由。
	std::string open(const std::string& id);
	// 閉じる(撮影の終わりに呼ぶ)。
	void close(void);
	// 露出を載せて1枚撮り、JPEG を out へ入れる。iso<=0 / expNs<=0 / aperture<=0 は「触らない」。
	bool capture(const std::string& id, int iso, long long expNs, double aperture,
	             int timeoutMs, std::vector<uint8_t>& out);
}

#endif // _BUILTIN_BRIDGE_H_
