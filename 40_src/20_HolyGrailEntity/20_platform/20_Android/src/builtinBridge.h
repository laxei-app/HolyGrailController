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


	// 端末のカメラを所持カメラへ足す(既にあるものは触らない)。戻り=見つけた台数。
	//  初回起動で一度だけ呼ばれ、所持カメラ・所持レンズ・ひな形を作る。並びや周期の規則は
	//  このとき apiBuiltin が答えた値で決まり、以後は差し替えない(データを作り直せば新しくなる)。
	//  実装は builtinRegister.cpp。内蔵カメラは在否監視に乗らないので、ここが唯一の登録経路。
	//  namesJson: スマホ用の撮影制御方法初期値の名前(型ごと。UI の言語で)。
	int registerAll(const std::string& namesJson);

	// 端末が持つカメラの一覧。JSON 配列 [{"id","name","facing"}]。取れなければ "[]"。
	std::string listJson(void);
	// 論理カメラの配下にぶら下がっている物理カメラ(超広角・望遠など)。触らずに素性だけ見る。
	std::string physicalsJson(void);
	// 1台の諸元。JSON。取れなければ "{}"。
	std::string describeJson(const std::string& id);
	// 開く。logicalId=入口の論理カメラ / physId=実際に使う物理カメラ。"" =成功。
	//  raw=RAW で受け取って自前で現像する(足し合わせができる)。端末が RAW を出せなければ JPEG に落ちる。
	std::string open(const std::string& logicalId, const std::string& physId, bool raw);
	// 閉じる(撮影の終わりに呼ぶ)。
	void close(void);
	// 露出を載せて1枚撮り**始める**。露光の終わりは待たない(キヤノンの CCAPI と同じ振る舞い)。
	//  iso<=0 / expNs<=0 / aperture<=0 は「触らない」。
	//  frames>1 は expNs のコマをその数だけ続けて撮り、線形で足して1枚にする(RAW のときだけ)。
	bool capture(const std::string& logicalId, const std::string& physId,
	             int iso, long long expNs, double aperture, int timeoutMs, int frames, bool raw);
	// 端末の熱の状態(PowerManager の THERMAL_STATUS_*。0=平常 … 6=停止直前)。-1=分からない。
	int thermalStatus(void);
	// 直前のコマを実際に撮った物理カメラ id(端末の申告。空=分からない)。
	std::string activePhysicalId(void);
	// 直近の1コマの経過(要求枚数/露光・届いた画像と結果・失敗理由・現像時間・到着時刻)。調査用。
	std::string captureReport(void);
	// 直前に撮り始めた1枚を受け取る(まだ露光中なら待つ)。
	bool takeImage(int timeoutMs, std::vector<uint8_t>& out);

	// ── 動画の書き出し ──────────────────────────────────────
	// 撮ったコマを1枚ずつ足していく。**撮影の終わりに必ず videoFinish を呼ぶこと**
	//  (MP4 は最後に閉じないと再生できない)。
	//  planName=ファイル名の頭に使う計画名(撮影側から渡す。UI に頼ると再起動後の再開で抜ける)。
	std::string videoStart(int fps, const std::string& planName);	// 戻り=ギャラリーでの名前。"" =失敗
	bool        videoAddJpeg(const std::vector<uint8_t>& jpeg);
	std::string videoFinish(void);								// 出来上がりの場所("" =失敗)
}

#endif // _BUILTIN_BRIDGE_H_
