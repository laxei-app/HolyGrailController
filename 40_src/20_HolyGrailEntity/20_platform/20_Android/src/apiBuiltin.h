#ifndef _API_BUILTIN_H_
#define _API_BUILTIN_H_
// スマホ内蔵カメラの apiBase 実装(2026-09-05)。
//
// 【キヤノン機との違いをここへ集める】
//  ・露出は連続に設定できる。上位は「目盛りのテーブル」で動くので、ここで**合成する**
//    (刻みは kStepStops。1か所で変えられるようにしてある)
//  ・撮った画像はその場で手に入る。CCAPI 実装が苦労した「新しい画像の登録通知を待つ」
//    「記録中のファイルを掴む」「サムネイル取得の回数制限」は、いずれも存在しない
//  ・認証も締め出しも無い
//
// 【Android 専用】Camera2 は Kotlin にしか無いので、撮影そのものは builtinBridge 経由で
//  呼び返す。このファイルは Android のプラットフォーム層にあり、エッジのビルドには入らない。
#include "apiBase.h"
#include "cameraData.h"
#include "exposureMath.h"
#include <string>
#include <vector>

class apiBuiltin : public apiBase
{
public:
	// 合成する目盛りの刻み[段]。**ここだけ変えれば粗さが変わる**(2026-09-05 ユーザー指示)。
	//  スマホは連続に設定できるので、キヤノン機の 1/3 段より細かくして性能を引き出す。
	static constexpr double kStepStops = 1.0 / 12.0;

	// device.serialno に入れる識別子の頭。所持カメラはこれで一意に管理される。
	static const char* kSerialPrefix;	// "BUILTIN:"

	apiBuiltin(void) {}
	~apiBuiltin(void) override {}

	// 諸元を読み、設定可能値のテーブルを合成する。カメラは開かない(開くのは撮る直前)。
	errCode init(class device& device) override;
	// 身元だけ。内蔵カメラは認証も締め出しも無いので init と同じで害が無い。
	errCode identify(class device& device) override { return this->init(device); }

	// 撮影を始める合図。ネットワークのカメラでは接続を張る工程だが、内蔵カメラでは
	//  カメラを開くことに当たる。ここで開けないと以降どの手も通らないので、早く気づけるよう
	//  この時点で開いてしまう。
	errCode startShooting(void) override;

	errCode getSettings(cmdt::shotRange& settings) override;
	errCode setFNumber(const std::string& fNumber) override;
	errCode setSS(const std::string& ss) override;
	errCode setIso(const std::string& iso) override;
	errCode rdyShutter(const cmdt::shotSet& shotSet) override;
	errCode actShutter(void) override;

	// 撮影モードの概念が無い(要求ごとにマニュアル露出を載せる)。開け閉めだけ受け持つ。
	errCode setupShootingModeManual(void) override;
	errCode restoreShootingMode(void) override;
	errCode keepAlive(void) override { return ERR_HGC_OK; }	// 切れる線が無い

	// 諸元は端末から取れる。撮る前から分かるので EXIF を待たない。
	errCode readSensorSpec(double& sensorWmm, double& sensorHmm, uint32_t& pixelW) override;

	// 直前に撮った1コマから場面の明るさを測る(撮影画像フィードバック)。
	errCode meterScene(const hgc::exposure& shotExp, meterResult& out,
	                   const std::function<bool()>& keepGoing) override;
	// まだ1コマも撮っていないときの測光。ここでは自由に撮ってよい。
	errCode meterHere(meterResult& out, const std::function<bool()>& keepGoing) override;

	// 撮影画像フィードバック系なので、露光が閉じ次第すぐ測ってよい。
	meterTiming meterTimingHint(void) const override
	{ meterTiming t; t.afterShutterClose = true; t.leadMs = 0; return t; }

	// この実装が撮った最後の JPEG(撮影レポートやセンサー諸元の補完で使う)。
	const std::vector<uint8_t>& lastJpeg(void) const { return lastJpeg_; }

	// カメラ id("0","1",…)。detectBuiltin が作った device から取る。
	const std::string& cameraId(void) const { return id_; }

private:
	// 値の文字列と、カメラへ渡す実数の対応。文字列は上位(テーブル/ログ/計画)が使う形。
	static std::string ssText(double sec);
	static std::string isoText(int iso);
	static std::string fnText(double fn);

	// 範囲から 1/kStepStops 段刻みの並びを作る。両端は必ず含める。
	void buildTables(void);

	// いま載っている露出で1枚撮り始める(露光の終わりは待たない)。
	bool shootStart(void);
	// 撮り始めた1枚を受け取る。露光の長さから待ち時間を決める。
	bool shootTake(std::vector<uint8_t>& out);
	// いまの露出の露光時間[秒](待ち時間の見積もりに使う)。
	double curSsSec(void) const;
	// JPEG から輝度の中央値とリニア輝度を出す。
	bool measure(const std::vector<uint8_t>& jpeg, meterResult& out) const;

	std::string id_;			// Camera2 のカメラ id
	std::string name_;			// 表示名
	// 諸元(取れなければ 0)
	double   sensorW_ = 0.0, sensorH_ = 0.0;
	uint32_t pixelW_  = 0,   pixelH_  = 0;
	int      isoMin_  = 0,   isoMax_  = 0;
	long long expMinNs_ = 0, expMaxNs_ = 0;
	std::vector<double> apertures_;
	bool     manual_  = false;	// マニュアル露出が使える端末か

	// 合成した設定可能値と、その APEX テーブル。
	//  テーブルは測光値を「露出非依存の場面の明るさ」へ割り戻すのに要る(apiBase には無い)。
	std::vector<std::string> ssList_, isoList_, fnList_;
	expo::expoTables tables_;

	// いま載せている露出(要求ごとに渡すので、ここが唯一の状態)
	std::string curSs_, curIso_, curFn_;

	std::vector<uint8_t> lastJpeg_;	// 直前に撮った JPEG(測光の材料)
	bool opened_ = false;
};

#endif // _API_BUILTIN_H_
