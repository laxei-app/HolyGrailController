#ifndef  _TOOL_H_
#define  _TOOL_H_
#include "common.h"

class tool
{
public:
    // 文字列解析系
	static std::string getXmlTagValue(const std::string& xml, const std::string& tagName);
	static std::string getKvpValue(const std::string& kvp, const std::string& key);
	static std::string getKvpValueColon(const std::string& kvp, const std::string& key);
	static bool findKvp(const std::string& kvp, const std::vector<std::string>& values);
	static std::string toLower(std::string data);
	static int spaceLen(std::string& data, int offset);
	static float ssToReal(std::string ss);

    // プラットフォームで実装する
    // 時間管理系
	static void* startElapse(void);
	static uint32_t getElapse(void * handle);
	static void sleep(uint32_t ms);
	// 壁時計(UTCエポック)のミリ秒。シャッター投下時刻など「絶対時刻をms精度で」記録する用途。
	// startElapse(単調増加・経過計測用)と異なり、実時刻(RTC/システム時刻)に対応する。
	static uint64_t epochMs(void);
    static void memoryInfo(void);

};
#endif // _TOOL_H_