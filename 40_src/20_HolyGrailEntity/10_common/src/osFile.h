#ifndef _OS_FILE_H_
#define _OS_FILE_H_
// プラットフォーム非依存のファイル保存抽象(データ構造仕様書43 §8.1)。
// ログや将来の永続化は dataManager 経由でこの抽象を使い、保存メディアの差
// (M5Stack=SD/LittleFS, Android=外部ファイル領域, Windows=通常ファイル)を吸収する。
// 実装は各プラットフォーム(20_platform/*/src/osFile*.cpp)に置く。

#include <string>

namespace osfile
{
	// 保存先のベースディレクトリを設定する(Android はアプリ外部ファイル領域を JNI 経由で渡す)。
	// M5Stack 等、設定不要なプラットフォームでは無視してよい。
	void setBaseDir(const std::string& dir);

	// 指定名のサブディレクトリ(例 "asset" "plan" "master" "log")のフルパスを返す。
	// 無ければ作成を試みる。取得・作成に失敗したら空文字を返す(データ構造仕様書43 §7.6)。
	std::string dir(const std::string& name);

	// ログ保存ディレクトリ(= dir("log"))。
	std::string logDir(void);

	// path へアトミックに書き込む(一時ファイルへ書いて rename。§7.5)。
	// ディレクトリが無ければ作成を試みる。return: 成功
	bool writeAll(const std::string& path, const char* data, size_t len);

	// path へ追記し、即 flush する。ディレクトリが無ければ作成を試みる。
	// path はフルパス。return: 成功
	bool append(const std::string& path, const char* data, size_t len);

	// path の内容をすべて out に読み込む(検証・ログ転送用)。return: 成功
	bool readAll(const std::string& path, std::string& out);
}

#endif // _OS_FILE_H_
