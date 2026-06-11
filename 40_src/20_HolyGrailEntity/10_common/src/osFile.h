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

	// ログ保存ディレクトリ(末尾セパレータ無し)を返す。無ければ作成を試みる。
	// 取得・作成に失敗したら空文字を返す。
	std::string logDir(void);

	// path へ追記し、即 flush する。ディレクトリが無ければ作成を試みる。
	// path はフルパス。return: 成功
	bool append(const std::string& path, const char* data, size_t len);
}

#endif // _OS_FILE_H_
