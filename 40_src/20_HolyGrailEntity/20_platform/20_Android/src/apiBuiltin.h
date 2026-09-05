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

	// 【加算で作る長秒露光の上限[秒](2026-09-06 ユーザー決定)】センサーの1コマの上限が何秒でも、
	//  内蔵カメラはここまで設定できる。上限を超える ss は、上限以下のコマを続けて撮って
	//  RAW を線形で足して1枚にする(rawStack)。RAW を出せない端末はセンサーの上限まで。
	static constexpr double kMaxStackSsSec = 48.0;

	// 【撮影周期の下限の規則(2026-09-06 ユーザー決定)】最小周期 = 最長ss × 1.25(余裕 0 秒)。
	//  露光の後に加算・現像・JPEG 化(実測 0.6〜0.9 秒)と測光が続くぶん。8.3 秒なら 10.4 秒で
	//  キヤノン機の規則(ss+2)とほぼ同じ、24 秒なら 30 秒、48 秒なら 60 秒。
	//  共通部分はこの値を所持カメラの記録から読むだけで、内蔵カメラかどうかを判断しない。
	static constexpr double kMinIntervalFactor    = 1.25;
	static constexpr double kMinIntervalMarginSec = 0.0;

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
	// 計画名を受け取る(動画のファイル名に使う)。撮影側が渡すので、再起動後の再開でも抜けない。
	void setSessionLabel(const std::string& label) override { sessionLabel_ = label; }
	// 端末の中に居るので在否監視の対象にしない(ネットワークの向こうに居ない)。
	bool networked(void) const override { return false; }
	// 所持カメラの記録へ、内蔵カメラの性質(撮影周期の規則)を書く。登録時に一度だけ。
	void fillCameraProfile(hgc::camera& cam) override
	{ cam.intervalFactor = kMinIntervalFactor; cam.intervalMargin = kMinIntervalMarginSec; }
	errCode restoreShootingMode(void) override;
	errCode keepAlive(void) override { return ERR_HGC_OK; }	// 切れる線が無い

	// 諸元は端末から取れる。撮る前から分かるので EXIF を待たない。
	errCode readSensorSpec(double& sensorWmm, double& sensorHmm, uint32_t& pixelW, uint32_t& pixelH) override;
	// 端末の熱の状態を「カメラの温度」として返す(2026-09-06)。長秒の加算は熱が心配なので、
	//  5分ごとの状態確認に乗せてログと通知へ出す。
	errCode readDeviceStatus(deviceStatus& out) override;

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

	// スマホ用のひな形を組み立てるための諸元。レンズが交換できないので、
	//  焦点距離と開放F値は「カメラの一部」として端末が答える。
	double focalMm(void) const { return focalMm_; }
	// センサーの面積[mm2]。どのカメラが星向きかを機種名に頼らず選ぶのに使う。
	double sensorArea(void) const { return sensorW_ * sensorH_; }
	double aperture(void) const { return apertures_.empty() ? 0.0 : apertures_.front(); }
	// センサー1コマの最長露光[秒](端末の申告)。これを超える ss は加算で作る。
	double maxSsSec(void) const { return (expMaxNs_ > 0) ? (static_cast<double>(expMaxNs_) / 1e9) : 0.0; }
	// 設定できる最長の ss[秒](加算込み)。RAW が出せれば kMaxStackSsSec、出せなければセンサーの上限。
	double maxSettableSsSec(void) const;
	// ss[秒] を撮るのに要るコマ数(1=足さない)。
	int stackFrames(double sec) const;
	// 設定可能値(ひな形の露出をこの並びへ吸着させる)。
	const std::vector<std::string>& isoList(void) const { return isoList_; }
	const std::vector<std::string>& ssList(void)  const { return ssList_; }
	const std::vector<std::string>& fnList(void)  const { return fnList_; }

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

	// 【入口と実体を分けて持つ(2026-09-05)】超広角などは論理カメラの配下にいて単体では
	//  開けない。論理カメラを入口にして物理カメラを名指しする。device.urlAccess には
	//  "論理/物理" の形で入っている(detectBuiltin が作る)。
	std::string logicalId_;		// 入口になる論理カメラ id
	std::string id_;			// 実際に使う物理カメラ id
	std::string name_;			// 表示名
	// 諸元(取れなければ 0)
	double   sensorW_ = 0.0, sensorH_ = 0.0, focalMm_ = 0.0;
	uint32_t pixelW_  = 0,   pixelH_  = 0;
	int      isoMin_  = 0,   isoMax_  = 0;
	long long expMinNs_ = 0, expMaxNs_ = 0;
	std::vector<double> apertures_;
	bool     manual_  = false;	// マニュアル露出が使える端末か
	// 星を消すノイズリダクションを切れるか(切れれば端末の映像処理をそのまま使える)。
	bool     nrOff_ = false, nrMinimal_ = false, edgeOff_ = false, rawOk_ = false;

	// 合成した設定可能値と、その APEX テーブル。
	//  テーブルは測光値を「露出非依存の場面の明るさ」へ割り戻すのに要る(apiBase には無い)。
	std::vector<std::string> ssList_, isoList_, fnList_;
	expo::expoTables tables_;

	// いま載せている露出(要求ごとに渡すので、ここが唯一の状態)
	std::string curSs_, curIso_, curFn_;

	// 撮った画像を残す(2026-09-05)。キヤノン機はカメラ側のSDに残るが、内蔵カメラには
	//  「カメラ側」が無いので、自分で書かないと何も残らない。
	//  【将来】最終的な成果物は端末上で作る動画なので、1コマずつの画像は中間物になる。
	//   動画の書き出しが入ったら、ここは既定で切る(確認したいときだけ残す)想定。
	void saveShot(const std::vector<uint8_t>& jpeg);
	// 撮り始めた1枚をまだ受け取っていなければ受け取って保存する。
	//  【必ず毎コマ呼ぶ】固定露出の窓では測光が呼ばれないので、測光に任せると誰も回収せず、
	//   受け取り口(ImageReader)の枠が埋まって撮れなくなる。
	void collectPending(void);
	int  shotSeq_ = 0;

	std::vector<uint8_t> lastJpeg_;	// 直前に撮った JPEG(測光の材料)
	bool opened_ = false;
	bool physWarned_ = false;	// 狙いと違うセンサーで撮れた警告を出したか(1回だけ)
	int  lastThermal_ = -1;		// 直前に記録した熱の状態(変わったときだけログに残す)
	std::string sessionLabel_;	// 計画名(動画のファイル名の頭)
};

#endif // _API_BUILTIN_H_
