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


	// 端末のカメラを所持カメラへ足す(既にあるものは触らない)。戻り=足した台数。
	//  実装は builtinRegister.cpp。内蔵カメラは在否監視に乗らないので、ここが唯一の登録経路。
	int registerAll(void);

	// 端末が持つカメラの一覧。JSON 配列 [{"id","name","facing"}]。取れなければ "[]"。
	std::string listJson(void);
	// 1台の諸元。JSON。取れなければ "{}"。
	std::string describeJson(const std::string& id);
	// 開く。"" =成功、それ以外は理由。
	std::string open(const std::string& id);
	// 閉じる(撮影の終わりに呼ぶ)。
	void close(void);
	// 露出を載せて1枚撮り**始める**。露光の終わりは待たない(キヤノンの CCAPI と同じ振る舞い)。
	//  iso<=0 / expNs<=0 / aperture<=0 は「触らない」。
	bool capture(const std::string& id, int iso, long long expNs, double aperture, int timeoutMs);
	// 直前に撮り始めた1枚を受け取る(まだ露光中なら待つ)。
	bool takeImage(int timeoutMs, std::vector<uint8_t>& out);

	// ── 動画の書き出し ──────────────────────────────────────
	// 撮ったコマを1枚ずつ足していく。**撮影の終わりに必ず videoFinish を呼ぶこと**
	//  (MP4 は最後に閉じないと再生できない)。
	std::string videoStart(const std::string& path, int fps);	// "" =成功
	bool        videoAddJpeg(const std::vector<uint8_t>& jpeg);
	std::string videoFinish(void);								// 出来上がりの場所("" =失敗)
}

#endif // _BUILTIN_BRIDGE_H_
