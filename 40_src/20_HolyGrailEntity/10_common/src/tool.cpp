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

