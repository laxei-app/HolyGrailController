#pragma once

// SoftAP が配る IP を「前に配った先」を避けて決める(2026-08-26)。
//
// 【直したい事】ユーザー報告: カメラの電源を入れ直したり、エッジを再起動したりすると
//  **2台が同じ IP になる**。実機で再現済み(エッジを瞬時リセット → エッジは .4 を PC へ
//  配り直したが、EOS R100 は .4 を掴んだまま → PC が join/leave(reason=8) を繰り返して
//  APIPA へ落ちた)。
//
// 【なぜ起きるか】ESP32 の DHCP サーバ(dhcps)は**貸出表をRAMにしか持たない**。電源が
//  落ちれば表は消え、次の起動では範囲の先頭(既定 192.168.4.2)からまた配り始める。
//  ところがカメラ側は「まだ自分のリース期間だ」と思って前の IP を使い続けるので、
//  電源の落ちていなかったカメラと、再起動後に配られた端末とが衝突する。
//  **短い停電ほど危ない**(長く止まればカメラも諦めて DHCP を引き直すので綺麗に収まる)。
//
// 【直し方】配った先を自分で覚えて、次に配る範囲をそこから外す。
//  ・DHCP が配ったとき(AP_STAIPASSIGNED は ip と mac をくれる)に、その **MAC の行**を
//    書き換える。同じ端末が別の IP をもらったのだから、**古い IP は空いた**と分かる。
//    ここが MAC を持つ理由。IP だけの記録では「増える一方」になり、範囲がどんどん上へ
//    追いやられて /24 を使い切ってしまう。
//  ・定期的に、AP に**今つながっている端末の一覧**(esp_wifi_ap_get_sta_list)を見て、
//    居る MAC の時刻を更新する。**問い合わせはしない**: 撮影中のカメラは在否監視の
//    対象外なので、叩いて確かめる方式だと撮影中の行を消してしまう。
//  ・同じく定期的に **ARP 表**も見る。DHCP を引き直さずに前の IP を使い続ける機器が
//    実在する(実機の EOS R100 と EOS R50 V で確認。AP を再起動しても DHCP を出さず
//    .4 / .7 を握ったままだった)。この手合いは貸出のイベントが出ないので、ARP から
//    ip↔mac を拾わないと記録に載らず、その IP を新参へ配って結局ぶつかる。
//  ・数時間(kExpireSec)更新の無い行は消す。ずっと残ると範囲が枯れる。
//    時刻は **UTC のエポック秒**で持つ(海外へ持ち出して時差が変わっても壊れない)。
//    **時計が入っていない間は消さない**(未設定の 1970 年と、C_TIME 後の 2026 年を
//    引き算すると全部が期限切れに見えるため)。
//  ・起動時にこの記録を読み、埋まっている IP を1つも含まない**連続した空き**の
//    一番下を配布開始位置にして SoftAP を立てる。
//
// 【残る穴(承知の上)】記録そのものが無い**初回だけ**は既定位置(.2)から配るので、
//  そこに居座る端末が居れば衝突しうる。2回目以降は ARP から拾った分も載っているので
//  避けられる。塞ぐには AP を dhcps 停止のまま上げて10秒ほど様子を見る必要があり、
//  起動が遅くなるので採らない。
//
// 【検証】配布開始位置の決め方(chooseStartOctet)は 50_tools/edge/lease_range_test.py に
//  同じ手順を写した試験がある。ここを直したらあちらも直して走らせること。
//
// 置き場所は 18_M5Common。CoreS3 も StickS3 も同じものを使う(機種で分岐しない)。

#include <WiFi.h>
#include <esp_wifi.h>
#include "lwip/etharp.h"	// ARP表(DHCPを引き直さない機器のIPを拾う)
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "dataManager.h"
#include "osFile.h"
#include "linkDown.h"	// 抜けた端末を撮影ループへ知らせる
#include <array>

namespace edgeApLeases
{
	// ── 決め事 ──────────────────────────────────────────────
	// 覚えておく端末数。AP の同時接続上限(10)+入れ替わりの余裕。
	constexpr size_t    kMaxRows    = 24;
	// これだけ更新が無ければ「もう居ない」とみなして消す。ユーザー指示は「数時間」。
	//  短すぎると一晩の撮影の合間に消え、長すぎると範囲が枯れる。6時間を採る。
	constexpr long long kExpireSec  = 6LL * 3600LL;
	// 2020-01-01T00:00:00Z。これより前なら時計が入っていない(既存の判定と同じ基準)。
	constexpr long long kClockValid = 1577836800LL;
	// Arduino の softAPConfig が作る貸出範囲の幅。start から start+10 までの **11個**
	//  (NetworkInterface::config: lease.end_ip = lease.start_ip + 10)。幅は変えられない。
	constexpr int       kPoolWidth  = 11;
	// 配布開始位置に選べる最終オクテット。.0=ネットワーク / .1=AP自身 は使えない。
	//  上限はフレームワークの制限 ((start & ~netmask) < 245) に合わせる。
	constexpr int       kOctetLo    = 2;
	constexpr int       kOctetHi    = 244;
	// 純粋な時刻更新だけのときに書き込む間隔。毎分書くと内蔵フラッシュ(StickS3)が傷む。
	//  取りこぼしても期限(6時間)に対しては誤差なので粗くてよい。
	constexpr uint32_t  kSaveEveryMs = 30UL * 60UL * 1000UL;
	// 在席の見直し間隔。
	constexpr uint32_t  kScanEveryMs = 60UL * 1000UL;

	struct row
	{
		uint8_t   mac[6] = {0, 0, 0, 0, 0, 0};
		uint8_t   oct    = 0;		// 192.168.4.<oct> の最終オクテット
		long long utc    = 0;		// 最後に AP で見たエポック秒(0=時計が入る前に記録した)
	};

	// ── 状態(ヘッダのみで完結させる。C++17 の inline 変数) ──────────
	inline std::vector<row>& rows(void) { static std::vector<row> v; return v; }
	inline bool&     dirty(void)   { static bool b = false;      return b; }
	// 「行の増減や IP の変化」があった印。時刻更新だけの dirty と分けて、こちらは即書く。
	//  ※ 書き込み自体は必ず loop(pump)から行う。DHCP のイベントは Arduino のイベント
	//     タスク上で走るので、そこで SD/LittleFS へ書くとスタックが足りない恐れがある。
	inline bool&     urgent(void)  { static bool b = false;      return b; }
	inline uint32_t& lastSave(void){ static uint32_t t = 0;      return t; }
	inline uint32_t& lastScan(void){ static uint32_t t = 0;      return t; }

	inline std::string filePath(void)
	{
		const std::string d = osfile::dir("asset");
		return d.empty() ? std::string() : (d + "/apLeases.txt");
	}

	inline std::string macText(const uint8_t* m)
	{
		char b[20];
		std::snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X",
		              m[0], m[1], m[2], m[3], m[4], m[5]);
		return std::string(b);
	}

	inline bool sameMac(const uint8_t* a, const uint8_t* b)
	{
		return std::memcmp(a, b, 6) == 0;
	}

	// ── 保存・読み出し ───────────────────────────────────────
	// 1行 = "192.168.4.5 AA:BB:CC:DD:EE:FF 1787000000"。人が読めるようにテキストで持つ。
	inline void save(void)
	{
		const std::string p = filePath();
		if (p.empty()) { return; }
		std::string s;
		s.reserve(rows().size() * 40 + 64);
		for (const row& r : rows())
		{
			char b[80];
			std::snprintf(b, sizeof(b), "192.168.4.%u %s %lld\n",
			              (unsigned)r.oct, macText(r.mac).c_str(), r.utc);
			s += b;
		}
		if (osfile::writeAll(p, s.c_str(), s.size()))
		{
			dirty() = false;
			urgent() = false;
			lastSave() = millis();
		}
	}

	inline void load(void)
	{
		rows().clear();
		const std::string p = filePath();
		if (p.empty()) { return; }
		std::string s;
		if (!osfile::readAll(p, s)) { return; }
		size_t i = 0;
		while (i < s.size() && rows().size() < kMaxRows)
		{
			size_t e = s.find('\n', i);
			if (e == std::string::npos) { e = s.size(); }
			const std::string line = s.substr(i, e - i);
			i = e + 1;
			unsigned a = 0, b = 0, c = 0, d = 0;
			unsigned m[6] = {0, 0, 0, 0, 0, 0};
			long long t = 0;
			if (std::sscanf(line.c_str(), "%u.%u.%u.%u %02X:%02X:%02X:%02X:%02X:%02X %lld",
			                &a, &b, &c, &d, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &t) != 11)
			{
				continue;
			}
			if (d < 1 || d > 254) { continue; }
			row r;
			for (int k = 0; k < 6; ++k) { r.mac[k] = (uint8_t)m[k]; }
			r.oct = (uint8_t)d;
			r.utc = t;
			rows().push_back(r);
		}
		dirty() = false;
	}

	// ── 記録の更新 ──────────────────────────────────────────
	// DHCP が配った瞬間に呼ぶ。**同じ MAC の行を書き換える**のが肝で、これで古い IP が空く。
	//  ここは Arduino のイベントタスク上なので RAM だけ触り、書き込みは pump に任せる。
	// 抜けていった MAC を溜める置き場。
	//
	// 【イベントタスクからネットワークAPIを呼ばないこと(2026-08-28 実機で端末が固まった)】
	//  最初は離脱イベントの中で MAC から IP を組み立てていた。前半3オクテットを得るのに
	//  WiFi.softAPIP() を呼んだのが誤り。これは esp_netif の API で、イベントループの
	//  タスクから呼ぶと自分の応答を自分で待つ形になり、そこで止まる。実機では画面も時計も
	//  止まり、UDP も ETP も応答しなくなった(撮影スレッドは既に張った接続で動き続けるので
	//  「撮影はしているのに端末が死んでいる」という分かりにくい姿になる)。
	//  このファイルの頭にも「イベントタスクでは RAM だけ触り、あとは pump に任せる」と
	//  書いてある。ここもその決まりに従い、MAC を控えるだけにして解決は pump でやる。
	inline std::vector<std::array<uint8_t, 6>>& leftMacs(void)
	{
		static std::vector<std::array<uint8_t, 6>> v;
		return v;
	}

	// 離脱イベントから呼ぶ。RAM に積むだけ。
	inline void onLeft(const uint8_t* mac)
	{
		if (leftMacs().size() >= kMaxRows) { return; }	// 溜まりすぎたら捨てる(次の掃除で拾う)
		std::array<uint8_t, 6> m{};
		std::memcpy(m.data(), mac, 6);
		leftMacs().push_back(m);
	}

	inline void onAssigned(const uint8_t* mac, uint32_t ipAddr)
	{
		const uint8_t oct = (uint8_t)((ipAddr >> 24) & 0xFF);	// IPAddress と同じ並び(先頭が下位)
		if (oct < 1 || oct > 254) { return; }
		const long long now = (long long)std::time(nullptr);
		for (row& r : rows())
		{
			if (!sameMac(r.mac, mac)) { continue; }
			if (r.oct != oct) { urgent() = true; }
			r.oct = oct;
			r.utc = now;
			dirty() = true;
			return;
		}
		if (rows().size() >= kMaxRows)
		{
			// 一番古い行を捨てる(時計未設定の 0 は最古として扱われるので先に消える)。
			size_t oldest = 0;
			for (size_t k = 1; k < rows().size(); ++k)
			{
				if (rows()[k].utc < rows()[oldest].utc) { oldest = k; }
			}
			rows().erase(rows().begin() + (long)oldest);
		}
		row r;
		std::memcpy(r.mac, mac, 6);
		r.oct = oct;
		r.utc = now;
		rows().push_back(r);
		dirty() = true;
		urgent() = true;
	}

	// 定期の見直し。loop から毎秒呼ぶ(中で間引く)。
	// 抜けた MAC を IP へ直して撮影ループへ渡す(pump から呼ぶ=通常のタスク文脈)。
	inline void drainLeft(void)
	{
		if (leftMacs().empty()) { return; }
		const IPAddress ap = WiFi.softAPIP();	// ここは通常のタスクなので呼んでよい
		for (const auto& m : leftMacs())
		{
			for (const row& r : rows())
			{
				if (!sameMac(r.mac, m.data())) { continue; }
				char b[16];
				std::snprintf(b, sizeof(b), "%u.%u.%u.%u",
				              (unsigned)ap[0], (unsigned)ap[1], (unsigned)ap[2], (unsigned)r.oct);
				linkDown::note(std::string(b));
				break;
			}
		}
		leftMacs().clear();
	}

	inline void pump(void)
	{
		drainLeft();		// 抜けた端末を撮影ループへ知らせる(タイムアウトを待たせない)
		const uint32_t ms = millis();
		// 行が増えた/IPが変わった なら、次の電源断に間に合うようすぐ書く。
		if (urgent()) { save(); }
		if (ms - lastScan() < kScanEveryMs) { return; }
		lastScan() = ms;

		const long long now = (long long)std::time(nullptr);

		// ① AP に今つながっている端末の時刻を更新する(通信は起こさない)。
		wifi_sta_list_t sl{};
		if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK)
		{
			for (int i = 0; i < sl.num; ++i)
			{
				for (row& r : rows())
				{
					if (sameMac(r.mac, sl.sta[i].mac)) { r.utc = now; dirty() = true; break; }
				}
			}
		}

		// ①-b ARP表から「実際にそのIPを使っている端末」を拾う(2026-08-26 実測で追加)。
		//  DHCP を引き直さずに前の IP を使い続ける機器がある。実機の EOS R100 と
		//  EOS R50 V がまさにそれで、AP を再起動しても DHCP を出さずに .4 / .7 を
		//  使い続けていた。貸出のイベントが出ないので、①だけでは記録に載らない。
		//  → 通信さえしていれば ARP 表には ip↔mac が出るので、そこから拾って記録に足す。
		//  これが無いと「記録に無い IP」を新参へ配ってしまい、結局ぶつかる。
		//  ※ 去った機器の残骸も数分は残るが、その IP を避けるのは安全側なので構わない
		//    (居なければ期限切れで消える)。
		if (WiFi.getMode() & WIFI_MODE_AP)
		{
			for (size_t i = 0; i < 16; ++i)		// 範囲外は etharp_get_entry が 0 を返す
			{
				ip4_addr_t*      ip  = nullptr;
				struct netif*    nif = nullptr;
				struct eth_addr* ea  = nullptr;
				if (etharp_get_entry(i, &ip, &nif, &ea) == 0) { continue; }
				if (ip == nullptr || ea == nullptr) { continue; }
				const uint32_t a = ip4_addr_get_u32(ip);
				if ((a & 0x00FFFFFFu) != 0x0004A8C0u) { continue; }	// 192.168.4.0/24 だけ
				if (((a >> 24) & 0xFF) == 1) { continue; }			// AP自身
				onAssigned(ea->addr, a);
			}
		}

		// ② 時計が入ったのに 0 のままの行へ今の時刻を入れる(時計未設定中に記録した行)。
		if (now >= kClockValid)
		{
			for (row& r : rows())
			{
				if (r.utc < kClockValid) { r.utc = now; dirty() = true; }
			}
		}

		// ③ 期限切れを消す。**時計が入っているときだけ**行う。
		if (now >= kClockValid)
		{
			for (size_t k = rows().size(); k-- > 0; )
			{
				if (now - rows()[k].utc <= kExpireSec) { continue; }
				char d[96];
				std::snprintf(d, sizeof(d), "lease forget ip=192.168.4.%u mac=%s",
				              (unsigned)rows()[k].oct, macText(rows()[k].mac).c_str());
				dataManager::logEvent("APSTA", d);
				rows().erase(rows().begin() + (long)k);
				dirty() = true;
				urgent() = true;		// 減った事実はすぐ残す(次の起動で範囲を下げられる)
			}
			if (urgent()) { save(); }
		}

		// ④ 時刻更新だけの変化は、間隔を空けて書く(内蔵フラッシュを傷めない)。
		if (dirty() && (ms - lastSave() >= kSaveEveryMs)) { save(); }
	}

	// ── 配布開始位置を決める ────────────────────────────────
	// 記録に載っている IP を1つも含まない、幅 kPoolWidth の連続した空きの**一番下**。
	inline int chooseStartOctet(void)
	{
		bool used[256];
		std::memset(used, 0, sizeof(used));
		used[0] = used[1] = used[255] = true;		// ネットワーク / AP自身 / ブロードキャスト
		for (const row& r : rows()) { used[r.oct] = true; }

		for (int s = kOctetLo; s <= kOctetHi; ++s)
		{
			int hit = -1;
			for (int k = 0; k < kPoolWidth; ++k)
			{
				if (used[s + k]) { hit = k; break; }
			}
			if (hit < 0) { return s; }
			s += hit;		// 次に試すのは詰まっていた場所の1つ先(ループの ++s と合わせて)
		}
		// ここへ来るのは /24 が埋まったとき。既定位置へ戻す(期限切れが片付けば直る)。
		dataManager::logEvent("APSTA", "lease pool: no free run, fallback to .2", true);
		return kOctetLo;
	}

	// SoftAP を立てる直前に呼ぶ。記録を読み、配布開始 IP を返す。
	inline IPAddress startIp(void)
	{
		load();
		const int s = chooseStartOctet();
		Serial.printf("[AP] lease record: %u row(s), pool start=192.168.4.%d (width %d)\n",
		              (unsigned)rows().size(), s, kPoolWidth);
		for (const row& r : rows())
		{
			Serial.printf("[AP]   held 192.168.4.%-3u %s utc=%lld\n",
			              (unsigned)r.oct, macText(r.mac).c_str(), r.utc);
		}
		return IPAddress(192, 168, 4, (uint8_t)s);
	}

	// 診断用: 記録の中身をシリアルへ出す。
	inline void dump(void)
	{
		const long long now = (long long)std::time(nullptr);
		Serial.printf("[LEASE] %u row(s) file=%s now=%lld\n",
		              (unsigned)rows().size(), filePath().c_str(), now);
		for (const row& r : rows())
		{
			Serial.printf("[LEASE]   192.168.4.%-3u %s utc=%lld age=%llds\n",
			              (unsigned)r.oct, macText(r.mac).c_str(), r.utc,
			              (now >= kClockValid && r.utc > 0) ? (now - r.utc) : -1LL);
		}
		Serial.printf("[LEASE] next pool start would be 192.168.4.%d\n", chooseStartOctet());
	}
}
