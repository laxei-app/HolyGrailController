#include "presenceMonitor.h"
#include "cameraController.h"
#include "dataManager.h"	// 挨拶を送ったことをログへ(飛んだかどうかを追えるように)
#include "httpAuth.h"		// 401 の内訳をログへ(資格情報が無いのか nc なのか)
#include "netThread.h"
#include "osSystemCall.h"
#include <json/nlohmann/json.hpp>
#include <vector>
#include <mutex>
#include <atomic>
#include <string>
#include <cstdio>
#include <ctime>
#include <thread>
#include <chrono>

// カメラ在否モニタ(共通実装)。旧 phonePresence.cpp のループをそのまま共通化し、canScan 述語を追加した。
//  注: detectTarget が確保する device.apiBase は shared_ptr 所有。フル探索結果 found は merge 後に破棄され
//      最後の参照が消えて解放されるため、常駐フル探索でもリークしない。
namespace {

	struct pcam { std::string serial; std::string model; std::string assignedName; std::string ip; bool online = false; long long lastSeen = 0;
	              bool verify = false;		// #1: 次tickで即疎通確認し、失敗ならTTLを待たずオフラインへ
	              std::string location;	// SSDP のデバイス記述URL(認証不要)。生死確認はこれを引く
	              std::string access;		// CCAPI の入口(挨拶を送る先)
	              int  greetLeft = 0;		// 挨拶の残り試行回数(0=済み/不要)。失敗しても捨てない
	              long long greetAt = 0;	// 直近に挨拶を試した時刻(間隔をあけるため)
	              bool missed = false; };	// 直近の生死確認で返事が無かった(短い離脱の検知)
	std::vector<pcam>       g_map;
	std::mutex              g_mapMutex;
	std::function<void()>   g_onChange;
	std::function<bool()>   g_canScan;		// この周期にネットワーク探索してよいか(エッジ=撮影中は false)
	std::function<bool(const std::string&)> g_inUse;	// 撮影に使っている個体か(触らない判定)
	std::atomic<bool>       g_running{false};
	std::atomic<bool>       g_scanned{false};	// 一度でもフル探索(M-SEARCH)を終えたか。未探索なら在否は「不明」
	std::atomic<bool>       g_wake{false};		// NOTIFY等で即フル探索を促す
	bool                    g_useNotify = true;	// 受動NOTIFY(SSDP watchStart)を使うか(エッジ=false)
	void*                   g_thread = nullptr;
	std::string             g_lastSig;			// 直近スナップショット署名(変化検知用)

	constexpr int  kFullEverySec = 60;			// フル M-SEARCH の間隔(オンライン検知/取りこぼし保険)
	constexpr int  kTickSec      = 6;			// ループ周期(この単位で疎通確認・wake確認)
	constexpr long kTtlSec       = 150;			// この秒数見えないオンライン個体はオフライン扱い
	constexpr int  kGreetTries   = 8;			// 挨拶をあきらめるまでの回数(理由は greetLocked を参照)
	constexpr long kGreetGapSec  = 10;			// 挨拶を試す間隔[秒]

	std::string hostOf(const std::string& url)
	{
		auto p = url.find("://");
		if (p == std::string::npos) { return std::string(); }
		size_t s = p + 3, e = s;
		while (e < url.size() && url[e] != ':' && url[e] != '/') { ++e; }
		return url.substr(s, e - s);
	}

	// httpAuth が host を覚えるときの鍵と同じ形("http://ip:port")。
	//  net.cpp の endpointOf と揃えること。ここがずれると診断が always known=0 になる。
	std::string endpointOf(const std::string& url)
	{
		const size_t p = url.find("://");
		if (p == std::string::npos) { return url; }
		const size_t s = url.find('/', p + 3);
		return url.substr(0, (s == std::string::npos) ? url.size() : s);
	}

	// 挨拶を仕掛ける。1回で終わりにしない理由は greetLocked の説明を参照。
	void armGreet(struct pcam& c) { c.greetLeft = kGreetTries; c.greetAt = 0; }

	std::string signatureLocked()
	{
		std::vector<std::string> items;
		for (const auto& c : g_map) { if (c.online) { items.push_back(c.serial + ":" + c.ip); } }
		std::sort(items.begin(), items.end());
		std::string s; for (auto& i : items) { s += i; s += ","; }
		return s;
	}

	void mergeFullLocked(long long now)
	{
		std::vector<class device> found;
		// 【CCAPI を叩かない】在否に必要なのは機種名/シリアル/愛称/IP だけで、いずれも SSDP の
		//  デバイス記述(認証不要)から取れる。以前は detectTarget で apiBase まで作っていたため、
		//  撮影主体でない側が認証付きで CCAPI を叩き、ダイジェスト認証の nc がぶつかって
		//  カメラを 403 で締め出していた(EOS R50 V 実測 2026-08-16)。
		//  カメラに触るのは「そのカメラで撮る主体」だけ、という規則に合わせる。
		cameraController::identifyTargets(found);
		for (auto& d : found)
		{
			std::string ip = hostOf(d.urlAccess);
			if (ip.empty()) { ip = hostOf(d.location); }	// urlAccess が無い機種は記述URLのホストで代用
			if (d.serialno.empty() || ip.empty()) { continue; }
			bool merged = false;
			for (auto& c : g_map)
			{
				if (c.serial != d.serialno) { continue; }
				c.model = d.model; c.assignedName = d.assignedName; c.ip = ip; c.location = d.location;
				c.access = d.urlAccess;
				// 居なかったものが見えた = ネットワークに入り直した。挨拶を送る(greetLocked を参照)。
				//  【TTL を待たない】オフライン確定には kTtlSec(150秒)かかる。カメラ側で手動で
				//   切って繋ぎ直す操作はそれより短いことが多く、online のままなので以前は
				//   挨拶が立たなかった(2026-08-28 実機で発生)。生死確認を一度でも落として
				//   いれば「入り直した」とみなす。
				if (!c.online || c.missed) { armGreet(c); }
				c.missed = false;
				c.online = true; c.lastSeen = now; merged = true; break;
			}
			if (!merged)
			{
				pcam n{ d.serialno, d.model, d.assignedName, ip, true, now };
				n.location = d.location;
				n.access = d.urlAccess;
				armGreet(n);		// 初めて見えた
				g_map.push_back(n);
			}
		}
	}

	void refreshLivenessLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (!c.online) { c.verify = false; continue; }
			// 【ここも CCAPI を叩かない】生死を知るだけなら SSDP のデバイス記述(認証不要)で足りる。
			//  記述URLは機種で異なる(ポートもパスも)ので、探索時に受け取ったものをそのまま引く。
			//  未取得のうちは在否を落とさない(TTL に委ねる)。
			if (c.location.empty()) { c.verify = false; continue; }	// 記述URL未取得=判定しない(TTLに委ねる)
			std::string resp;
			const bool alive = netThread::httpGet(c.location, resp) && !resp.empty();
			if (alive)
			{
				// 返事が無かった後に戻ってきた = 入り直した。TTL に達していなくても挨拶する。
				if (c.missed) { armGreet(c); c.missed = false; }
				c.lastSeen = now; c.verify = false;
			}
			else if (c.verify)  { c.missed = true; c.online = false; c.verify = false; }	// #1: 即確認の要求 → TTLを待たずオフライン
			else
			{
				c.missed = true;
				if (now - c.lastSeen > kTtlSec) { c.online = false; }
			}
		}
	}

	// 一度オフラインにした個体を、記憶している記述URLへ当てて復帰させる。
	//
	// 【なぜ要るか(2026-08-19)】オフラインになった個体は refreshLiveness が見ない(online だけを見る)ので、
	//  復帰の判定はフル探索(M-SEARCH)だけが握っている。M-SEARCH が届かない環境では**一度落ちたら
	//  二度と戻らない**。実機で発生: エッジのAPモードでは M-SEARCH が飛んでおらず、カメラの電源を
	//  入れ直したあと待機中の×が戻らなかった。
	//  記述URL(認証不要)へ GET するだけなので、カメラに触る側の規則(CCAPIを叩かない)は保つ。
	//  IPが変わっていると当たらないが、その場合はフル探索が拾う。ここは「戻れる道をもう1本」用。
	void recoverOfflineLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (c.online || c.location.empty()) { continue; }
			std::string resp;
			if (netThread::httpGet(c.location, resp) && !resp.empty())
			{ c.online = true; c.lastSeen = now; c.missed = false; armGreet(c); }	// 参加し直した
		}
	}

	// #1: verify 要求のある個体だけを先に疎通確認する(フル探索の周期を待たない)。
	bool anyVerifyPendingLocked()
	{
		for (const auto& c : g_map) { if (c.online && c.verify) { return true; } }
		return false;
	}

	bool canScanNow() { return !g_canScan || g_canScan(); }
	bool inUseNow(const std::string& serial) { return g_inUse && g_inUse(serial); }

	// カメラに「繋がったよ」と知らせる(2026-08-28 ユーザー報告 + 実機で確定)。
	//
	// 【なぜ要るか】カメラを Wi-Fi に参加させるとき、カメラは接続先のIPを表示して待ち続ける。
	//  この状態で電源を切ると Wi-Fi の設定がやり直しになる。**一度きちんと繋がれば覚える。**
	//
	// 【何が必要か(実機 EOS R50 V で確定)】
	//  ・SSDP や UPnP の記述子(:49152)では**足りない**。記述子を出しているのは別のサービス
	//  ・CCAPI(:8080)へ**届くだけでも足りない**。認証なしで叩いて 401 を貰っても変わらなかった
	//  ・**200 が返る CCAPI アクセス**で「接続が完了しました」に変わった
	//  つまりカメラが見ているのは「成功したやり取り」であって、到達ではない。
	//
	// 【いつ送るか】所持カメラに載っているかは関係ない。ネットワークに入り直すたびに要る。
	//  電源の入り切りだけでなく、カメラ側で手動で切断して繋ぎ直したときも同じ。
	//  なので「見えていなかったものが見えた」瞬間を合図にする(初回・復帰の両方)。
	//
	// 【1回で終わりにしない(2026-08-28 実機のログで判明)】
	//  以前は合図が立った1回だけ投げ、失敗したらそれきりだった。実機ではこれが効かず、
	//  カメラが2分以上「接続してください」のまま止まった(Edge00 のログに greet failed が
	//  1行だけ残り、以後は何も起きない)。うまくいかない理由が2つあり、どちらも
	//  「もう一度投げれば直る」種類のものだった。
	//   (a) 見つけた直後はカメラ側の CCAPI がまだ応じないことがある
	//   (b) ダイジェスト認証の nc が前回の続きに追いつくのに2往復要る。net の GET は
	//       401 を受けて認証を付け直すのを**1度しかやらない**ので、1回の呼び出しでは
	//       nc を大きく飛ばす手当て(httpAuth::learn の bump)まで届かない
	//  そこで成功するまで間隔をあけて投げ直す。ただし回数は必ず区切る。資格情報が違う
	//  相手へ延々と失敗を投げると、カメラが 403 で締め出して本体設定を入れ直すまで
	//  戻らなくなる(EOS R50 V 実測)。
	//
	// 【まず素で投げる(2026-09-05 ユーザー指示)】CCAPI の認証を使わない設定のカメラでは、
	//  認証なしの GET がそのまま 200 になり、それだけで挨拶が成立する。以前は資格情報の
	//  候補が1つも無いと投げない作りだったため、**認証を使わないカメラは永久に挨拶できず**、
	//  「このカメラに接続してください」の画面から進めなかった(実機で発生)。
	//  net の GET は素で投げ、401 を受けたときだけ認証を付けて1度だけ投げ直す。つまり
	//  「認証なし → 駄目なら認証」の順は 通信の層で既にできている。
	//  締め出しの歯止めは残す: 401 が返ったのに候補が1つも無ければ、そのカメラは認証が要ると
	//  分かるので**1回でやめる**(下の greetLeft=0)。素の401を繰り返さない。
	//
	// 【撮影中の個体には送らない】成功させるには認証が要り、認証は nc(ノンスカウンタ)を進める。
	//  カメラは nc を一本で覚えているので、撮る側は次の要求で 401 を受けて認証をやり直す羽目に
	//  なる(実測では時刻から種を作り直して回復するが、撮影の最中に往復を増やす意味は無い)。
	//  撮影中の個体は「ずっと見えている」ので、そもそもこの合図は立たない。念のため明示で弾く。
	void greetLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (c.greetLeft <= 0) { continue; }
			if (!c.online)        { continue; }	// 見えていないなら投げない(見えたときに再開する)
			if (inUseNow(c.serial)) { c.greetLeft = 0; continue; }	// 撮っている個体には触らない
			if (c.access.empty())
			{	// 入口が分からない。**ここが空だと挨拶は永久に飛ばない**ので残す。
				c.greetLeft = 0;
				dataManager::logEvent("CAMERA", ("greet skipped (no access url): " + c.serial).c_str());
				continue;
			}
			if (c.greetAt != 0 && now - c.greetAt < kGreetGapSec) { continue; }	// 間をあける
			c.greetAt = now;
			--c.greetLeft;
			// 機器情報を1回読むだけ。軽く、どの機種にもあり、こちらにも有用な内容が返る。
			std::string resp;
			const bool ok = netThread::httpGet(c.access + "/ver100/deviceinformation", resp);
			if (ok) { c.greetLeft = 0; dataManager::logEvent("CAMERA", ("greet ok " + c.access).c_str()); }
			else
			{	// 何で断られたかを残す。401 なら資格情報か nc、0 なら届いていない。
				//  401 は外から見ると全部同じ顔をしているので、認証側の内訳も添える
				//  (資格情報が無い/打ち止め/nonce を毎回作り直されている、の区別)。
				int st = 0; std::string body;
				netThread::lastHttpFailure(st, body);
				// 403 = カメラが締め出した。投げ直すほど悪くなるので即やめる。
				//  戻すにはカメラ本体の接続設定を入れ直すしかない(EOS R50 V 実測)。
				if (st == 403) { c.greetLeft = 0; }
				// 401 なのに手持ちの資格情報が1つも無い = このカメラは認証が要るのに、
				//  こちらは何も持っていない。投げ直しても素の 401 が増えるだけで、
				//  続けると 403 で締め出される。**1回でやめる**。
				//  所持カメラへ ID/パスワードを入れれば候補ができ、次に見つけたときに改めて挨拶する。
				else if (st == 401 && !httpAuth::hasCandidates()) { c.greetLeft = 0; }
				char b[256];
				std::snprintf(b, sizeof(b), "greet failed http=%d left=%d %s [%s]",
				              st, c.greetLeft, c.access.c_str(),
				              httpAuth::diagnose(endpointOf(c.access)).c_str());
				dataManager::logEvent("CAMERA", b, c.greetLeft == 0);	// 打ち止めのときだけ ERR
			}
		}
	}


	// 撮影中の軽い接触。**撮影に使っていない個体だけ**を記述URLで突く。
	//  目的は在否の更新ではなく、**AP の無通信タイマで追い出されないこと**。
	//  撮影中の個体には一切触れない(シャッターI/O と競合させない。現行の設計を維持)。
	void keepAliveLocked(long long now)
	{
		for (auto& c : g_map)
		{
			if (!c.online || c.location.empty()) { continue; }
			if (inUseNow(c.serial))              { continue; }	// 撮影中の個体は触らない
			std::string resp;
			// 応答が無いときは平時と同じ TTL 規則でオフラインへ落とす(新しい規則は作らない)。
			//  触るだけで判定しないと、カメラが死んでいてもアイコンは「居る」のままになる。
			//  「カメラ側は繋がって見えるのにエッジからは見えない」状態を表示で見分けられないのは困る(2026-08-24 指摘)。
			if (netThread::httpGet(c.location, resp) && !resp.empty()) { c.lastSeen = now; }
			else if (now - c.lastSeen > kTtlSec)                       { c.online = false; }
		}
	}

	void presenceLoop()
	{
		long long lastFull = 0;
		while (g_running.load())
		{
			const long long now = static_cast<long long>(std::time(nullptr));
			// wake は「この周回で必ず消費する」。条件式の中で exchange すると、短絡評価で
			// 評価されない経路ができ、wake が立ちっぱなしになって下の待ちが 0 秒になる
			// (=CPUを明け渡さない空回り → IDLEタスクが走れず task_wdt でリセット)。
			// 実際 2026-07-23 に両エッジが約2秒周期でリセットを繰り返した(WDT/ossNet)。
			const bool woke = g_wake.exchange(false);
			bool changed = false;
			if (canScanNow())	// エッジ: 撮影中はネットワークを触らずマップを保持(前回値のまま)
			{
				std::lock_guard<std::mutex> lk(g_mapMutex);
				// #1: 即確認の要求がある間は、まず疎通確認を優先する(オフラインを最短で確定させる)。
				//  その後のフル探索で、IPが変わっただけの個体は拾い直される。
				const bool verifyFirst = anyVerifyPendingLocked();
				const bool doFull = !verifyFirst &&
				                    (woke || (now - lastFull >= kFullEverySec) || lastFull == 0);
				if (doFull)
				{
					mergeFullLocked(now); lastFull = now; g_scanned.store(true);
					recoverOfflineLocked(now);	// M-SEARCHで拾えなかった復帰を記述URLで拾う
				}
				else        { refreshLivenessLocked(now); }
				greetLocked(now);	// 入り直したカメラへ挨拶(成功するまで数回投げる)
				std::string sig = signatureLocked();
				if (sig != g_lastSig) { g_lastSig = sig; changed = true; }
			}
			else
			{	// 撮影中。探索はしないが、使っていないカメラだけ軽く突いて追い出されないようにする。
				std::lock_guard<std::mutex> lk(g_mapMutex);
				keepAliveLocked(now);
			}
			if (changed && g_onChange) { g_onChange(); }
			// 待ち。wake で早く起きるのは良いが、**1秒は必ず眠る**。そうしないと wake が
			// 立て続けに来たときや、上のブロックを丸ごと飛ばす経路(撮影中=canScanNow false)で
			// 1周が一瞬で終わり、このスレッドがCPUを占有してWDTリセットに至る。
			std::this_thread::sleep_for(std::chrono::seconds(1));
			for (int i = 1; i < kTickSec && g_running.load() && !g_wake.load(); ++i)
			{ std::this_thread::sleep_for(std::chrono::seconds(1)); }
		}
	}

}	// anonymous namespace

namespace presenceMon {

void start(std::function<void()> onChange, std::function<bool()> canScan, bool useNotify,
           std::function<bool(const std::string&)> inUse)
{
	if (g_running.exchange(true)) { return; }	// 二重起動防止
	g_onChange = std::move(onChange);
	g_canScan  = std::move(canScan);
	g_useNotify = useNotify;
	g_inUse    = std::move(inUse);
	g_lastSig.clear();
	// 受動NOTIFY: カメラ出現の広告を拾ったら即フル探索へ前倒し(useNotify のときのみ。
	//  エッジは共有SSDPリスナを entity 側=runner poke 用が使うため、ここでは二重登録しない)。
	if (g_useNotify) { cameraController::watchStart([]() { g_wake.store(true); }); }
	ossc::THREAD_FUNC fn = [](void*) -> errCode { presenceLoop(); return ERR_HGC_OK; };
	// スタックは 6144。既定(12288)は過大だった。実測で used=3616(2026-08-23)で、
	// 同じ HTTP/UDP の I/O をする netThread ワーカー(6144 確保/2704 使用)と同水準。
	// このスレッドは常駐なので、削った差分がそのまま内部RAMの余裕になる。
	g_thread = ossc::threadNet(fn, nullptr, 6144);
}

void stop(void)
{
	if (!g_running.exchange(false)) { return; }
	if (g_useNotify) { cameraController::watchStop(); }
	if (g_thread) { ossc::threadEnd(g_thread); g_thread = nullptr; }
	std::lock_guard<std::mutex> lk(g_mapMutex);
	g_map.clear(); g_lastSig.clear(); g_scanned.store(false);
}

std::string json(void)
{
	std::lock_guard<std::mutex> lk(g_mapMutex);
	nlohmann::json arr = nlohmann::json::array();
	for (const auto& c : g_map)
	{
		if (c.serial.empty() || c.ip.empty()) { continue; }
		arr.push_back({ {"serial", c.serial}, {"model", c.model}, {"assignedName", c.assignedName}, {"ip", c.ip}, {"online", c.online} });
	}
	return arr.dump();
}

void verifyNow(const hgc::camera& cam)
{
	{
		std::lock_guard<std::mutex> lk(g_mapMutex);
		const std::string& m = cam.model.empty() ? cam.name : cam.model;
		for (auto& c : g_map)
		{
			if (!c.online) { continue; }
			const bool hit = (!cam.serial.empty() && c.serial == cam.serial) ||
			                 (cam.serial.empty() && !m.empty() &&
			                  (c.model.find(m) != std::string::npos ||
			                   (!c.model.empty() && m.find(c.model) != std::string::npos)));
			if (hit) { c.verify = true; }
		}
	}
	// ループの待ちを打ち切って即座に処理させる(疎通確認→必要ならフル探索の順で回る)。
	g_wake.store(true);
}

int presence(const hgc::camera& cam)
{
	if (!g_scanned.load()) { return -1; }	// 未探索=不明
	std::lock_guard<std::mutex> lk(g_mapMutex);
	if (!cam.serial.empty())
	{
		for (const auto& c : g_map) { if (c.serial == cam.serial) { return c.online ? 1 : 0; } }
		// serial 指定だが未知(まだ見えたことがない) → 下のモデル照合へ。
	}
	const std::string& m = cam.model.empty() ? cam.name : cam.model;
	if (!m.empty())
	{
		for (const auto& c : g_map)
		{
			if (c.online && (c.model.find(m) != std::string::npos || (!c.model.empty() && m.find(c.model) != std::string::npos)))
			{ return 1; }
		}
	}
	return 0;	// 探索済みでオンライン一致が無い=オフライン(×)
}

}	// namespace presenceMon
