#ifndef _API_BASE_H_
#define _API_BASE_H_
#include "common.h"
#include "device.h"
#include "hgcCommon.h"		// hgc::exposure(測光露出の申告に使う)
#include "exposureMath.h"	// expo::expoTables(APEX換算。カメラ非依存の数学)
#include <functional>
#include <string>

class apiBase
{
public:
	// 撮影の設定可能な値

public:
	apiBase(void) {};
	virtual ~apiBase(void) {};
	virtual errCode init(class device& device) = 0;
	virtual errCode startShooting(void) { return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode rdyShutter(const cmdt::shotSet& shotSet) { return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode actShutter(void)						{ return ERR_HGC_NOT_SUPPORTED; }
	// 露出を1項目ずつ設定する(周期正確化のタイマ方式で、変更のあった項目だけを適用するため)。
	virtual errCode setFNumber(const std::string& fNumber)	{ (void)fNumber; return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode setSS(const std::string& ss)			{ (void)ss;      return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode setIso(const std::string& iso)			{ (void)iso;     return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode getSettings(cmdt::shotRange& settings)	{ return ERR_HGC_NOT_SUPPORTED; }
	virtual errCode rdyMetering(void)						{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode alzMetering(cmdt::HISTOGRAM& hist)		{ return ERR_HGC_NOT_SUPPORTED; };
	// 撮影開始時にカメラを当アプリ都合(マニュアル露出)に設定し、終了時に元へ戻す(仕様8/CCAPI)。
	virtual errCode setupShootingModeManual(void)			{ return ERR_HGC_NOT_SUPPORTED; };
	virtual errCode restoreShootingMode(void)				{ return ERR_HGC_NOT_SUPPORTED; };
	// 接続維持用の無害なGET。撮影窓まで待機中などに定期送出し、無通信でカメラの
	// Wi-Fi/CCAPIセッションがタイムアウト切断するのを防ぐ。return ERR_HGC_OK で到達。
	virtual errCode keepAlive(void)							{ return ERR_HGC_NOT_SUPPORTED; };

	// === 測光(場面のリニア輝度の取得) ===
	// 「測光して場面の明るさを得る」という機能はどのカメラでも同じで、**どう測るか**が
	// カメラに依存する。そのため測り方の実装詳細はこの層(apiBaseの実装クラス)に完全に閉じ、
	// 上位(captureRunner)は結果 sceneRef(露出非依存の場面の明るさ)だけを使う。
	//
	// 【上位が知ってよいこと・知ってはいけないこと(2026-08-13 徹底)】
	//  知ってよい : sceneRef / usable / meterExp / 診断の各msと段数。これだけで露出制御は書ける。
	//  知らない   : ライブビューを使うのか撮影画像のサムネイルを使うのか、そのために
	//               カメラの露出を触るのか、何回リトライするのか、といった一切の手段。
	//  したがって別メーカー/別方式のカメラを足すときは、この2関数(meterScene/meterHere)と
	//  meterTimingHint を実装するだけでよく、captureRunner には一行も手を入れない。
	// CCAPI 実装(apiCanonCCAPI)は「直前に撮れた画像のサムネイルから輝度を出す」方式である。
	struct meterResult
	{
		bool          ok       = false;	// 測光値が得られたか(false=露出は据え置きを推奨)
		// 得られた測光値を「露出制御の根拠にしてよいか」(2026-08-07)。
		//  ok は「測れたか」だけを表す。測れていても中央値が使える帯を外れていれば、その値は
		//  信用できないので露出を動かしてはいけない。両者を分けたのは、外れた値も**ログには
		//  残したい**ため(ok=false にすると Y=/LVHIST が出ず、何が見えていたか追えなくなる)。
		//  false のとき呼び出し側は露出を据え置く(測光失敗と同じ扱い)。
		bool          usable   = true;	// 露出制御に使ってよい値か(false=値はログのみ・露出は据え置き)
		// 【カメラが撮れていない疑いの申告(2026-08-13)】
		//  直前のコマの撮影結果が、待っても現れなかった。シャッターは受け付けられたのに画像が
		//  記録されない状態(実測: EOS R10 がサムネイル取得を重ねると陥る)を上位へ伝える。
		//  上位はこれが続いたら「カメラがオンラインでない」として提示する。
		//  ・「測れなかった」(ok=false)とは別物である。取得や復号でつまずいただけなら画像は
		//    記録されているので、こちらは false のままにすること。
		//  ・撮影結果を手がかりにしない測り方(ライブビュー等)の実装は常に false でよい。
		bool          shotMissing = false;
		double        sceneRef = -1.0;	// 露出非依存の「場面の明るさ」(=linear/2^測光段)。投影・ev0の基礎
		double        linear   = -1.0;	// 測光露出で写るリニア輝度(中央値)
		double        x        = -1.0;	// ヒストグラム中央値(0..1 sRGB)
		hgc::exposure meterExp;			// 実際に測光した露出(ev0の逆算はこれを使う)
		// --- カメラ露出状態の申告(差分適用キャッシュとの整合用。露出を触ったら必ず申告する) ---
		// 測光のために実装がカメラの露出を動かした場合、**何を乗せて帰ってきたか**をここへ入れる。
		// 空の軸=触れていない。呼び出し側はこれで「カメラに何が乗っているか」の記憶を更新する
		// (更新しないと、次の差分適用で本来送るべき軸を送らず、露出がずれたまま撮ってしまう)。
		hgc::exposure appliedExp;
		bool          applyFailed = false;	// 露出を送ったが失敗(遅延適用の恐れ→呼び出し側は次の適用で全軸を再送)
		// --- 診断(ログ用) ---
		// meterSsUsed / pinned / asIsLinear は旧LV方式(測光ssへ一時切替する方式)の診断だった。
		// サムネイル測光では使わないが、ログの列とそれを読む解析スクリプトを壊さないよう残す。
		std::string   meterSsUsed;		// 測光に使った特別な露出(空=撮影露出のまま測った)
		double        p99 = -1.0, pMax = -1.0;	// ヒストの明るい側(99%点/最大ビン)
		// --- 測光統計量の比較用(2026-08-08 診断。制御には使わない) ---
		// 【なぜ足すか】夜明けに露出の追従が約1時間遅れた。原因は測光に使っている中央値が
		//  実写より最大4.35段暗く出ることで(2026-08-08 朝R10 実測)、統計量を変えれば早く
		//  反応するのか、するとしてどれだけかを、ログに中央値/p99/pMx しか無いため
		//  算出できなかった。候補を並べて残し、後から机上で比較できるようにする。
		//  制御は従来どおり中央値(linear)だけを使う。
		double        meanLin  = -1.0;	// ヒストグラム全体のリニア平均(定数倍に素直=土俵合わせ向き)
		double        p75      = -1.0;	// 75%点(sRGB位置 0..1。p99 と同じ流儀)
		double        p90      = -1.0;	// 90%点(同上)
		double        satRatio = -1.0;	// 最上位ビンの画素割合(0..1)。飽和して情報が無い度合い
		uint32_t      histSum  = 0;		// ヒスト内容チェックサム(前コマ一致=古いフレーム検出)
		uint64_t      lvTimeMs = 0;		// フレームのカメラ側取得時刻[ms]
		int           staleSkip = 0;	// 古いフレームを捨てた回数
		int           tries     = 0;	// 取得試行回数
		int           settleMs  = -1;	// 測光のために待った時間[ms](-1=待っていない)
		int           rdyMs     = -1;	// 測光全体の実測[ms](下の内訳の合計)
		// 内訳(2026-07-28 計測用): 遅くなっているのがカメラの記録待ちか通信かを切り分けるため。
		int           waitMs    = -1;	// 新しい画像の登録通知を待った時間[ms]
		int           fetchMs   = -1;	// サムネイル取得(HTTP GET)の時間[ms]
		int           decodeMs  = -1;	// JPEG復号+ヒストグラム計算の時間[ms]
		int           fetchTries = 0;	// サムネイル取得の試行回数(1=一発成功)
		bool          pinned    = false;	// 張り付き検出(旧LV方式の診断。現方式では常に false)
		double        asIsLinear = -1.0;	// 同上(現方式では常に -1)
		// 失敗した工程(0=成功)。番号の意味は実装が決める。ログで原因を特定するために残す。
		// apiCanonCCAPI: 1=登録通知が来ない 2=ホスト不明 3=サムネイル取得失敗 4=復号失敗
		//                5=ヒストグラムが空 / 20〜23=初期収束(20:出発点の露出が無い
		//                21:測光露出を適用できない 22:LVヒストが取れない 23:張り付きを抜けられない)
		int           failStage = 0;
		// 新規画像待ちの内訳(診断用)。failStage=1(通知が来ない)のとき、どこでつまずいたかを残す。
		//  0=該当なし  1=event/polling が使えない  6=通知の取得が失敗した  7=時間内に来なかった
		int           waitStep  = 0;
		int           waitHttp  = 0;	// その通信のHTTPステータス(0=応答なし/接続失敗)
		std::string   waitBody;			// 応答本文の先頭(CCAPIは理由をここに返す)
	};
	// 【撮影中の測光】直前に撮った1コマ(その露出が shotExp)を手がかりに場面の明るさを測る。
	//  実装がどう測るかは自由(撮影画像のサムネイル/ライブビュー/外部センサー…)。呼び出し側は
	//  out.sceneRef と out.usable だけを見る。
	//  keepGoing: 中断判定(falseを返したら速やかに諦める)。
	virtual errCode meterScene(const hgc::exposure& shotExp, meterResult& out,
	                           const std::function<bool()>& keepGoing)
	{ (void)shotExp; (void)out; (void)keepGoing; return ERR_HGC_NOT_SUPPORTED; }
	// 【シャッターを切る前の測光】まだ1コマも撮っていない状態で場面の明るさを測る
	//  (撮影窓の手前で1枚目の露出を決める初期収束で使う)。
	//  撮影画像が存在しないので、実装はこの用途に限りカメラを自由に使ってよい。測るために露出を
	//  動かしたときは out.appliedExp へ申告すること(呼び出し側の露出キャッシュ整合のため)。
	//  返すのは meterScene と同じ「露出非依存の場面の明るさ」= out.sceneRef。
	virtual errCode meterHere(meterResult& out, const std::function<bool()>& keepGoing)
	{ (void)out; (void)keepGoing; return ERR_HGC_NOT_SUPPORTED; }
	// 「いつ測光(meterScene)を呼んでほしいか」の申告(2026-07-27)。captureRunner はこの指示に従って
	// スケジュールする。値は実装(カメラ/方式)ごとに変えてよい:
	//  ・撮影画像フィードバック系: ソースは直前の撮影画像=呼び出し時刻に依存しない
	//    → afterShutterClose=true(露光が閉じ次第すぐ呼んでよい。最速)
	//  ・ライブビュー系: シャッターの一定時間前の輝度を見たい
	//    → afterShutterClose=false + leadMs(シャッターの何ms前に呼ぶか)
	struct meterTiming
	{
		bool afterShutterClose = false;	// true=露光終了直後に測光してよい
		int  leadMs            = 5000;	// false時: シャッターの何ms前に測光を開始するか
	};
	virtual meterTiming meterTimingHint(void) const { return meterTiming{}; }
	// 【測光方式の指定】所持カメラの設定(hgc::camera::meterLv)を上位が渡す。
	//  false(既定) = 撮影済みサムネイルだけで測る。最も正確で、これが基本。
	//  true         = ライブビュー主体で測り、ライブビューでは追随できない明るさのときだけ
	//                 サムネイルへ落ちる。**サムネイルの取得回数に上限がある機種のための逃げ道**で、
	//                 精度は落ちる(EOS R10 は撮影と対にしたサムネイル取得が電源投入あたり
	//                 200回程度で応答しなくなり、一晩持たない)。
	//  実装がこの区別を持たないなら何もしなくてよい(その機種の唯一の方式で測る)。
	virtual void setMeterLv(bool) {}
	// 測光の内部状態(学習値・鮮度基準など)を捨てる(セッション確立/再接続時)。
	virtual void meterReset(void) {}
	// 【1枚目のシャッターを切る直前に呼ばれる】撮影の準備(撮影モード変更・設定取得・初期収束の
	//  露出適用)でカメラ側に溜まった「これから測る1コマとは無関係な状態」を捨てて構え直す。
	//  実装が何も溜め込まないなら何もしなくてよい。
	//  【なぜ要るか(2026-08-13 実測)】CCAPI 実装は登録通知(event/polling)で1コマを待つ。
	//   セッション開始時に一度空読みしていたが、その後の準備で設定変更イベントが積まれ、
	//   **1コマ目の通知だけが10秒待っても拾えなかった**(CoreS3/StickS3 とも R100 で100%再現。
	//   try=27〜34、2コマ目は1.4〜2.0秒遅れ、3コマ目からは平常)。空読みをここへ移す。
	virtual void meterArm(void) {}
	// ライブビューを止める(既定は何もしない)。撮影ループ中に掴み続けないため。
	virtual errCode stopLiveView(void) { return ERR_HGC_NOT_SUPPORTED; }
	// 撮影ループ中にライブビューが必要か(既定は必要=従来動作)。
	virtual bool liveViewNeededWhileCapturing(void) const { return true; }
};

#endif // _API_BASE_H_
