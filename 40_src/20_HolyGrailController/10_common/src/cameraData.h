#ifndef _CAMERA_DATA_H_
#define _CAMERA_DATA_H_

namespace cmdt
{
	// 撮影の設定可能な値
	class shotRange
	{
	public:
		class range
		{
		public:
			float max;
			float min;
			float step;
		};
		std::vector<float> ss;
		std::vector<float> iso;
		std::vector<float> fNum;
//		range ct;		// color temperature
//		range cs;		// color shift
	};

	// ヒストグラム
	inline constexpr int hist_elem = 4;
	inline constexpr int hist_bin = 256;
	typedef union hist_gram_t
	{
		uint16_t raw[hist_elem][hist_bin];

		struct
		{
			uint16_t	y[hist_bin];		// 輝度
			uint16_t	r[hist_bin];		// red
			uint16_t	g[hist_bin];		// green
			uint16_t	b[hist_bin];		// blue
		};
	} HISTOGRAM;

	// 撮影時の設定
	class shotSet
	{
	public:
		float ss;
		float fNum;
		float iso;
	public:
		shotSet(float ss, float fNum, float iso)
		{
			this->ss = ss;
			this->fNum = fNum;
			this->iso = iso;
		}
	};

	// スケジュール内の位置
	enum pos
	{
		start,
		middle,
		end
	};

	// 撮影制御
	enum captureControl
	{
		fix,							// 固定
		free							// 自動
	};

	// 日時
	struct date_time
	{
		uint16_t	year;				// 年
		uint16_t	month;				// 月
		uint16_t	day;				// 日
		uint16_t	hour;				// 時
		uint16_t	min;				// 分
		uint16_t	sec;				// 秒
	};

	// 制御の優先度
	enum pri
	{
		ss,								// シャッター速度
		f_num,							// f値
		iso,							// iso 感度
		nd,								// nd フィルタ
		NUM								// 優先度の数
	};

	// スケジュールの支点
	struct fullcrum
	{
		shotSet			limitDark;			// 暗限界
		shotSet			limitBright;		// 明限界
		pri				priority[pri::NUM];	// 撮影設定の優先度
		captureControl	control;			// 制御方法
		pos     		position;			// 位置付け
		date_time		dayTime;			// 時刻
	};


};

#endif // _CAMERA_DATA_H_
