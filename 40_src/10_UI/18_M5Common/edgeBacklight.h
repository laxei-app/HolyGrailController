// エッジ端末のバックライト自動消灯(2026-08-17)。
//
// 【なぜ要るか】屋外に放置して一晩撮るのに、LCD が点きっぱなしだと
//  ・星景撮影の現場で光害になる(本人にも周囲にも迷惑)
//  ・電池運用のとき無駄に減る
// 消し忘れが必ず起きるので、手動だけでは目的を果たせない。時間で自動的に消す。
//
// 【方針(ユーザー指示 2026-08-17)】
//  ・無操作 kIdleOffMs(1分)で消灯。**撮影中かどうかに関係なく常に**
//  ・スマホから撮影計画が送られてきたときに点灯(人が操作している合図)
//  ・時刻になって撮影が始まったときに点灯
//  ・消灯中の入力は「点けるだけ」。その入力で撮影を開始/停止させない(誤爆防止)
//  ・手動消灯もできる(CoreS3=最下段の時計帯タップ / StickS3=どちらかのキー長押し)
//  ・消灯中は**何も光らせない**。電源LEDも消す(機種側の apply が面倒を見る)
//
// 【この層の役割】時間と状態だけを持つ。実際に何を消すか(LCDのバックライト・電源LED)は
//  機種ごとに違うので、apply コールバックで機種側(main.cpp)へ委ねる。
//  18_M5Common に共通、機種固有は各フォルダ、という既存の層構成に合わせている。
#pragma once

#include <cstdint>

namespace edgeBL
{
	// 消灯中に「生きている」ことだけを知らせる心拍[ms](2026-08-19 ユーザー指示)。
	//
	// 【なぜ要るか】完全に消すと、電源が切れているのか消しただけなのか見分けがつかない。
	//  屋外に置いてくるので「動いている」ことだけは知りたい。
	// 【なぜ点けっぱなしにしないか】星景撮影の現場なので光を出したくない。
	//  10秒に一度 kBeatMs だけ光らせれば、平均の光量は点灯の 1/150 以下に収まる。
	//  伝える情報は「生きている」だけでよく、表示内容は見えなくてよい(ユーザー指示)。
	// 【何を光らせるかは機種ごと】CoreS3 は電源LEDを持たない(M5Unified の setLed も
	//  ESP32-S3 では M5PaperS3 しか扱わない)のでバックライトを最低輝度で一瞬だけ。
	//  StickS3 は電源LEDがあるのでそちらを一瞬だけ。判断は beatFn を渡す機種側に置く。
	constexpr uint32_t kBeatEveryMs = 10000;	// 心拍の間隔(2026-08-20 に5秒から10秒へ)
	constexpr uint32_t kBeatMs      = 60;	// 1回の点灯時間(loop の周期より十分長くとる)

	// 無操作でこの時間が過ぎたら消す[ms]。
	//  短すぎると設置作業のたびに消えて煩わしく、長すぎると放置時に光り続ける。
	//  復帰は画面に触る/ボタンを押すだけなので、短めでも実害が小さい。
	constexpr uint32_t kIdleOffMs = 60000;

	// 機種側が用意する「実際に点ける/消す」処理。バックライトと電源LEDをまとめて扱う。
	using applyFn = void (*)(bool on);
	// 機種側が用意する「心拍を光らせる/消す」処理。何を光らせるかは機種が決める。
	using beatFn  = void (*)(bool on);

	class state
	{
	public:
		// 起動時に1回呼ぶ。点灯状態から始める(設置作業のため)。
		void begin(applyFn fn, uint32_t now, beatFn bf = nullptr)
		{
			apply_ = fn;
			beat_  = bf;
			on_    = true;
			last_  = now;
			beatAt_  = now;
			beatOn_  = false;
			if (apply_) { apply_(true); }
		}

		bool isOn(void) const { return on_; }

		// 操作やイベントがあった。点いていれば計時をやり直すだけ、消えていれば点ける。
		//  return: この呼び出しで**消灯から復帰した**か。true なら呼び出し側は
		//          その入力を「点灯のためだけ」に使い、本来の操作は行わないこと。
		bool poke(uint32_t now)
		{
			last_ = now;
			if (on_) { return false; }
			on_ = true;
			if (apply_) { apply_(true); }
			return true;
		}

		// 手動消灯。
		void off(uint32_t now)
		{
			last_ = now;
			if (!on_) { return; }
			on_ = false;
			if (apply_) { apply_(false); }
			beatAt_ = now; beatOn_ = false;	// 消した直後は光らせない(間隔を空けてから)
		}

		// 毎ループ呼ぶ。無操作が続いたら消す。消灯中は心拍を打つ。
		void update(uint32_t now)
		{
			if (on_)
			{
				if (now - last_ < kIdleOffMs) { return; }
				on_ = false;
				if (apply_) { apply_(false); }
				beatAt_ = now; beatOn_ = false;
				return;
			}
			// 消灯中: 一定間隔で短く光らせる(生きている合図)。
			if (!beat_) { return; }
			if (beatOn_)
			{
				if (now - beatAt_ < kBeatMs) { return; }
				beatOn_ = false; beatAt_ = now; beat_(false);
			}
			else
			{
				if (now - beatAt_ < kBeatEveryMs) { return; }
				beatOn_ = true; beatAt_ = now; beat_(true);
			}
		}

	private:
		applyFn  apply_ = nullptr;
		beatFn   beat_   = nullptr;
		bool     on_     = true;
		uint32_t last_   = 0;
		uint32_t beatAt_ = 0;	// 心拍の最後の切り替え時刻
		bool     beatOn_ = false;	// 今そのひと呼吸を光らせているか
	};
}
