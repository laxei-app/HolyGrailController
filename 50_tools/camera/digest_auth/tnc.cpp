// nc(ノンスカウンタ)の扱いと、同じ nonce の 401 を誤判定しないことの検証。
//  実機(EOS R50 V)で「nc を巻き戻すと以後どれだけ正しく作っても 401」になったことへの回帰テスト。
#include "httpAuth.h"
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
static int fails = 0;
static void chk(bool ok, const char* w){ if(!ok){++fails;std::printf("FAIL %s\n",w);} else std::printf("ok   %s\n",w); }
static std::string f(const std::string& h, const std::string& k)
{
	const size_t p = h.find(k + "=");
	if (p == std::string::npos) { return ""; }
	size_t q = p + k.size() + 1;
	if (q < h.size() && h[q] == '"') { const size_t e = h.find('"', q + 1); return h.substr(q + 1, e - q - 1); }
	const size_t e = h.find_first_of(", ", q);
	return h.substr(q, (e == std::string::npos ? h.size() : e) - q);
}
static std::string chal(const char* nonce, const char* extra = "")
{
	return std::string("Digest realm=\"CameraControlApi\",nonce=\"") + nonce +
	       "\",domain=\"http://192.168.1.16:8080/ccapi\",opaque=\"op\",algorithm=MD5,qop=\"auth\"" + extra;
}
int main()
{
	const std::string H = "192.168.1.16:8080";

	// ① 通ったあとに同じ nonce で 401 が来ても、nc を巻き戻さない
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"eto", "pw"} });
	httpAuth::learn(H, chal("N1"));
	unsigned last = 0;
	for (int i = 0; i < 5; ++i) { last = std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16); }
	chk(last > 0, "nc は時刻から始まる(0 ではない)");
	httpAuth::noteSuccess(H);							// 実際に通った
	chk(httpAuth::learn(H, chal("N1")), "通った後の同一nonce 401 は再送する");
	chk(std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16) == last + 1, "nc を巻き戻さない");
	chk(f(httpAuth::authorization(H, "GET", "/a"), "username") == "eto", "資格情報を替えない");

	// ② stale=true なら未実績でも nc を維持する
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"eto", "pw"} });
	httpAuth::learn(H, chal("N2"));
	httpAuth::authorization(H, "GET", "/a");
	const unsigned n2 = std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16);
	chk(httpAuth::learn(H, chal("N2", ",stale=true")), "stale は再送する");
	chk(std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16) == n2 + 1, "stale で nc を維持");

	// ③ 一度も通っていない同一nonceの401は、本当に資格情報が違う → 次の候補へ、nc は数え直し
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"bad", "bad"}, {"good", "good"} });
	httpAuth::learn(H, chal("N3"));
	httpAuth::authorization(H, "GET", "/a"); httpAuth::authorization(H, "GET", "/a");
	chk(httpAuth::learn(H, chal("N3")), "未実績の同一nonce 401 はまず nc を疑う");
	chk(f(httpAuth::authorization(H, "GET", "/a"), "username") == "bad", "1回目は資格情報を替えない");
	chk(httpAuth::learn(H, chal("N3")), "それでも駄目なら次の候補へ");
	{
		const std::string h = httpAuth::authorization(H, "GET", "/a");
		chk(f(h, "username") == "good", "2つ目の候補になる");
		chk(std::stoul(f(h, "nc"), nullptr, 16) > 0, "候補が変われば nc は数え直し(時刻の種から)");
	}

	// ④ nonce が変われば nc は数え直し
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"eto", "pw"} });
	httpAuth::learn(H, chal("N4"));
	httpAuth::authorization(H, "GET", "/a"); httpAuth::noteSuccess(H);
	httpAuth::learn(H, chal("N5"));
	chk(std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16) > 0, "nonce が変われば数え直し");

	// ⑤ 実績は nonce が変わったら引き継がない。候補を使い切ったら打ち止めにする
	//    (誤った資格情報を投げ続けるとカメラが締め出されるため)
	chk(httpAuth::learn(H, chal("N5")), "まず nc を疑う(飛ばして再送)");
	httpAuth::authorization(H, "GET", "/a");
	chk(!httpAuth::learn(H, chal("N5")), "候補を使い切ったら false");
	chk(!httpAuth::learn(H, chal("N5")), "打ち止めは続く(何度来ても再送しない)");
	chk(!httpAuth::learn(H, chal("N6")), "nonce が変わっても打ち止めのまま");
	chk(httpAuth::authorization(H, "GET", "/a").empty(), "打ち止め中は認証を付けない");
	httpAuth::addCandidate("eto2", "pw2");			// 資格情報を足したら再挑戦できる
	chk(httpAuth::learn(H, chal("N7")), "候補を足せば打ち止めが解ける");

	// ⑥ hostGuard: 同じ host は直列、別 host は並行(死錠しないこと)
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"eto", "pw"} });
	{
		std::vector<std::thread> ts;
		std::atomic<int> inside{0}, maxInside{0};
		for (int i = 0; i < 4; ++i)
		{
			ts.emplace_back([&]{
				for (int k = 0; k < 50; ++k)
				{
					httpAuth::hostGuard g(H);
					int n = ++inside;
					int prev = maxInside.load();
					while (n > prev && !maxInside.compare_exchange_weak(prev, n)) {}
					std::this_thread::yield();
					--inside;
				}
			});
		}
		for (auto& t : ts) { t.join(); }
		chk(maxInside.load() == 1, "同じ host は同時に1本だけ");
	}
	{	// 別 host 同士は待たない(片方を握ったまま他方を取れる)
		httpAuth::hostGuard a("192.168.1.7:8080");
		httpAuth::hostGuard b("192.168.1.9:8080");
		chk(true, "別 host は互いに待たない");
	}
	{	// 候補が無ければ錠は取らない(既存構成に影響しない)
		httpAuth::setCandidates({});
		httpAuth::hostGuard a(H);
		httpAuth::hostGuard b(H);		// 同じ host を二重に取れる = 何もしていない
		chk(true, "資格情報なしなら直列化しない");
	}

	// ⑦ 再起動直後の安全網。カメラは nonce を使い回し nc を覚えているので、起動し直した直後は
	//    前回の続きより小さい nc を送ってしまうことがある。そのとき「パスワードが違う」と
	//    決めつけず、nc を大きく飛ばして1回試すこと。
	httpAuth::resetAll();
	httpAuth::setCandidates({ {"eto", "pw"} });
	httpAuth::learn(H, chal("NR"));
	unsigned firstRun = 0;
	for (int i = 0; i < 20; ++i) { firstRun = std::stoul(f(httpAuth::authorization(H, "GET", "/a"), "nc"), nullptr, 16); }

	httpAuth::resetAll();					// アプリを再起動したのと同じ状態
	httpAuth::setCandidates({ {"eto", "pw"} });
	httpAuth::learn(H, chal("NR"));			// カメラは同じ nonce を返してくる
	httpAuth::authorization(H, "GET", "/a");	// 前回の続きより小さいかもしれない nc を送った
	chk(httpAuth::learn(H, chal("NR")), "弾かれても資格情報のせいにせず再送する");
	{
		const std::string h = httpAuth::authorization(H, "GET", "/a");
		chk(f(h, "username") == "eto", "資格情報は替えない");
		chk(std::stoul(f(h, "nc"), nullptr, 16) > firstRun, "nc を大きく飛ばして前回の続きを越える");
	}
	//  それでも駄目なら本当に資格情報が違う → 次の候補(無ければ打ち止め)
	chk(!httpAuth::learn(H, chal("NR")), "飛ばしても駄目なら諦める");

	std::printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
	return fails;
}
