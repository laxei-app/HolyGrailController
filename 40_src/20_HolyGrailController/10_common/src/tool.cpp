#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cmath>
#include "tool.h"

// XML文字列から指定したタグに囲まれた値を抽出する
// xml XMLのフルテキスト
// tagName 取得したいタグ名 (例: "friendlyName")
// return タグの中身の文字列。見つからない場合は空文字。
std::string tool::getXmlTagValue(const std::string& xml, const std::string& tagName) 
{
    std::string startTag = "<" + tagName; // 名前空間や属性がある場合を考慮し '>' は含めない
    std::string endTag = "</" + tagName + ">";

    // 開始タグを検索
    size_t startPos = xml.find(startTag);
    if (startPos == std::string::npos) return "";

    // 開始タグの閉じ括弧 '>' を探す (属性などがある場合を飛ばすため)
    size_t contentStart = xml.find('>', startPos);
    if (contentStart == std::string::npos) return "";
    contentStart += 1; // '>' の次からが中身

    // 終了タグを検索
    size_t endPos = xml.find(endTag, contentStart);
    if (endPos == std::string::npos) return "";

    // 中身を切り出す
    return xml.substr(contentStart, endPos - contentStart);
}

// DeviceDiscovery の usn 用にコロンで区切られた値を取得する
// キーワードの大文字/小文字は区別せずに検索する。
// キーの後ろの値だけを取り出す。
// usn 内の書式は以下のようになっている
// この場合、key が "device"であれば "ICPO-CameraControlAPIService" を返す。
// 
// uuid:00000000-0000-0000-0001-F8A26DB2EE0D::urn:schemas-canon-com:device:ICPO-CameraControlAPIService:1
// 
// kvp    :入力の usn の値。終端は改行コードではない。
// key    :取得したいキー
// return :key に対応する値。見つからなかった時は長さ無し。
std::string tool::getKvpValueColon(const std::string& kvp, const std::string& key)
{
    std::string kvpLo = tool::toLower(kvp);
    std::string keyLo = tool::toLower(key);
    int keyIdx = (int)kvpLo.find(keyLo);
    if (keyIdx == -1) { return ""; }        // 見つからない。
    int keyColIdx = keyIdx + (int)keyLo.length();
    if (kvpLo[keyColIdx] != ':') { return ""; }                 // keyの後ろの':'が無い

    int end_c = (int)kvpLo.find(":", keyColIdx+1);              // val の終端の ':' を探す
    int endIdx = end_c;
    if (end_c == -1) { endIdx = (int)kvpLo.length(); }          // ':'がない時全体の終端
    int offset = keyColIdx + 1;
    return kvp.substr(offset, endIdx - offset);
}

// DeviceDiscovery で取得した Key Value Pair 書式のパケットから指定キーの値を取得する。
// キーワードの大文字/小文字は区別せずに検索する。
// キーの後ろの値だけを取り出す。
// key value pair の書式は以下のようになっている。
// この場合、key が "LOCATION"であれば "abcdefg" を返す。
// 
// LOCATION:abcdefg
// 
// kvp    :取得した全体のパケット
// key    :取得したいキー
// return :key に対応する値。見つからなかった時は長さ無し。
std::string tool::getKvpValue(const std::string& kvp, const std::string& key)
{
    std::string kvpLo = tool::toLower(kvp);
    std::string keyLo = tool::toLower(key);
    int locIdx = (int)kvpLo.find(keyLo);
    if (locIdx == -1) { return ""; }        // 見つからない。
    if (kvpLo[locIdx + (int)keyLo.length()] != ':') { return ""; }    // keyの後ろの':'が無い

    int end_r = (int)kvpLo.find("\r", locIdx);
    int end_n = (int)kvpLo.find("\n", locIdx);
    int endIdx = end_r;
    if ((end_r != -1) && (end_n == -1)) { endIdx = end_r; }         // \r だけがある
    if ((end_r == -1) && (end_n != -1)) { endIdx = end_n; }         // \n だけがある
    if ((end_r != -1) && (end_n != -1)) { endIdx = (end_n < end_r) ? end_n : end_r; }         // \r \n 両方ある。小さい方
    if ((end_r == -1) && (end_n == -1)) { endIdx = (int)kvpLo.length(); }  // 改行が無い時
    int offset = locIdx + (int)keyLo.length() + 1; 
    offset += spaceLen(kvpLo, offset);                  // スペースを取り除く
    return kvp.substr(offset, endIdx - offset);
}

// DeviceDiscovery で取得した Key Value Pair 書式のパケットから指定の値の位置を取得する
// kvp   :検索対象の文字列
// val   :検索する文字列
// return:検索した値のインデックス。見つからないときは -1。
bool tool::findKvp(const std::string& kvp, const std::vector<std::string>& values)
{
    for (const auto& val : values)
    {   // すべてのキーワードがあることを確認する
        std::string kvpLo = tool::toLower(kvp);
        std::string valLo = tool::toLower(val);
        if(kvpLo.find(valLo) == -1) return false;
    }
    return true;
}

// 文字列を小文字に変換する
// mix    :変換前の文字列
// return :小文字に変換した文字列
std::string tool::toLower(std::string mix) 
{
    std::string lower = mix;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    return lower;
}

// data の　offset からのスペースの数を調べる
int tool::spaceLen(std::string & data, int offset)
{
    
    size_t end = data.length();
    int point;
    for (point = offset; point < end; point++)
    {
        if (data[point] != ' ') { break; }
    }
    return point - offset;
}

// シャッター速度の文字列を実数に変換する
// ss : シャッター速度の文字列。
//     「15"」 「0"8」「1/15"」「1/1600"」こんな形式。
float tool::ssToReal(std::string ss) 
{
    if (ss.empty()) return 0.0;

    // 1. 文字列内の " を処理しやすくするために置換
    // 0"8 -> 0.8 に、1/30" -> 1/30 に変換する
    size_t pos;
    while ((pos = ss.find('\"')) != std::string::npos) 
    {
        // 末尾にある場合は削除、途中にあれば小数点に置換
        if (pos == ss.length() - 1) { ss.erase(pos); }
        else                        { ss[pos] = '.'; }
    }

    // 2. 分数形式 (1/1600 など) かどうかを判定
    if (ss.find('/') != std::string::npos) 
    {
        size_t slashPos = ss.find('/');
        try 
        {
            float numerator = std::stof(ss.substr(0, slashPos));
            float denominator = std::stof(ss.substr(slashPos + 1));
            return numerator / denominator;
        }
        catch (...) 
        {
            return 0.0;
        }
    }

    // 3. 単純な数値（15 や 0.8）として変換
    try {
        return std::stof(ss);
    }
    catch (...) {
        return 0.0;
    }
}

// 実数のシャッター速度を"を小数点位置にした文字列に変換する
// ss     : 実数のシャッター速度
// return : 文字列のシャッター速度
//          小数点を"で表す。
//          0.8 = 0.8
//          1/1000 = 1/1000     // " は不要
//          30 = 30
std::string tool::ssToStr(float ss)
{
    if (ss <= 0) return "0\"";

    // 1. 1秒以上の場合 (例: 15.0 -> 15")
    if (ss >= 1.0) {
        // 整数で割り切れる場合は整数のみ、そうでない場合は1.3"などの形式
        if (std::fmod(ss, 1.0) == 0) {
            return std::to_string(static_cast<int>(ss))+"\"";
        }
        else {
            // 1.3" などのケース（あまりCCAPIでは見かけませんが念のため）
            std::stringstream ssStr;
            ssStr << std::fixed << std::setprecision(1) << ss ;
            return ssStr.str();
        }
    }

    // 2. 1秒未満で "0.x" 形式にする境界値 (0.3秒〜0.8秒付近)
    // Canonの慣習に従い、0.3秒以上1秒未満を 0"X 形式とする
    if (ss >= 0.25) {
        int tenth = static_cast<int>(std::round(ss * 10));
        return "0\"" + std::to_string(tenth);
    }

    // 3. それより速い場合 (例: 0.0333 -> 1/30")
    // 逆数を取って分母を計算する
    int denominator = static_cast<int>(std::round(1.0 / ss));
    return "1/" + std::to_string(denominator);
}

// 実数のシャッター速度を"を小数点位置にした文字列に変換する
// ss : 実数のシャッター速度
std::string tool::ssToStrY(float ss) 
{
    if (ss <= 0) return "0\"";

    // 1. 1秒以上の場合 (例: 15.0 -> 15")
    if (ss >= 1.0) {
        // 整数で割り切れる場合は整数のみ、そうでない場合は1.3"などの形式
        if (std::fmod(ss, 1.0) == 0) {
            return std::to_string(static_cast<int>(ss)) + "\"";
        }
        else {
            // 1.3" などのケース（あまりCCAPIでは見かけませんが念のため）
            std::stringstream ssStr;
            ssStr << std::fixed << std::setprecision(1) << ss << "\"";
            return ssStr.str();
        }
    }

    // 2. 1秒未満で "0.x" 形式にする境界値 (0.3秒〜0.8秒付近)
    // Canonの慣習に従い、0.3秒以上1秒未満を 0"X 形式とする
    if (ss >= 0.25) {
        int tenth = static_cast<int>(std::round(ss * 10));
        return "0\"" + std::to_string(tenth);
    }

    // 3. それより速い場合 (例: 0.0333 -> 1/30")
    // 逆数を取って分母を計算する
    int denominator = static_cast<int>(std::round(1.0 / ss));
    return "1/" + std::to_string(denominator) + "\"";
}


// 実数のシャッター速度を"を小数点位置にした文字列に変換する。
// テーブルを使用した方式。ssToStr()と比較評価する。
// ss : 実数のシャッター速度
std::string tool::ssToStrX(double ss)
{
    struct ShutterSpeedEntry {
        double value;
        std::string label;
    };

    // 主要なシャッター速度のテーブル
    const static std::vector<ShutterSpeedEntry> SHUTTER_SPEED_TABLE = 
    {
        {30.0, "30\""}, {15.0, "15\""}, {8.0, "8\""}, {4.0, "4\""}, {2.0, "2\""},
        {1.0, "1\""}, {0.8, "0\"8"}, {0.5, "0\"5"}, {0.4, "0\"4"}, {0.3, "0\"3"},
        {1.0 / 4.0, "1/4\""}, {1.0 / 8.0, "1/8\""}, {1.0 / 15.0, "1/15\""},
        {1.0 / 30.0, "1/30\""}, {1.0 / 60.0, "1/60\""}, {1.0 / 125.0, "1/125\""},
        {1.0 / 250.0, "1/250\""}, {1.0 / 500.0, "1/500\""}, {1.0 / 1000.0, "1/1000\""},
        {1.0 / 1600.0, "1/1600\""}, {1.0 / 2000.0, "1/2000\""}, {1.0 / 4000.0, "1/4000\""},
        {1.0 / 8000.0, "1/8000\""}
        // 必要に応じて CCAPI の仕様書にある値をすべて追加してください
    };

    if (ss <= 0) return "0\"";

    // 入力値に最も近いテーブル内の値を探す
    auto it = std::min_element(SHUTTER_SPEED_TABLE.begin(), SHUTTER_SPEED_TABLE.end(),
        [ss](const ShutterSpeedEntry& a, const ShutterSpeedEntry& b) 
    {
        return std::abs(a.value - ss) < std::abs(b.value - ss);
    });

    return it->label;
}