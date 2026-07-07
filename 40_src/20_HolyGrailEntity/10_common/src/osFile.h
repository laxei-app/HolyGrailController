#ifndef _OS_FILE_H_
#define _OS_FILE_H_
// プラットフォーム非依存のファイル保存抽象(データ構造仕様書43 §8.1)。
// ログや将来の永続化は dataManager 経由でこの抽象を使い、保存メディアの差
// (M5Stack=SD/LittleFS, Android=外部ファイル領域, Windows=通常ファイル)を吸収する。
// 実装は各プラットフォーム(20_platform/*/src/osFile*.cpp)に置く。

#include <string>
#include <vector>

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

	// path の offset バイト目から最大 maxLen バイトを out に読み込む(大きなログの分割転送用。RAM節約)。
	// out には実際に読めたバイト数分だけ入る(< maxLen なら EOF)。return: 成功(offsetがEOF以降でも空で成功)。
	// M5Stack 実装のみ(エッジのログ転送で使用)。他プラットフォームはスタブ(false)。
	bool readRange(const std::string& path, size_t offset, size_t maxLen, std::string& out);

	// 現在採用しているファイルシステム名("SD"/"LittleFS"/"none")を返す(検証用)。
	const char* backendName(void);

	// 指定サブディレクトリ(例 "plan")内のファイル名のうち、prefix で始まり suffix で終わる
	// ものをベース名で返す(撮影計画 plan_<id>.json の列挙など)。prefix/suffix が空なら無条件。
	// フルパスは dir(subdir) + "/" + 返り値 で構成する。
	std::vector<std::string> listFiles(const std::string& subdir,
	                                   const std::string& prefix,
	                                   const std::string& suffix);

	// 指定サブディレクトリ内のファイルを削除する(plan_<id>.json の削除など)。return: 成功。
	bool removeFile(const std::string& subdir, const std::string& name);

	// ログディレクトリ(logDir)内のログファイル名一覧(例 "hg_2026-06-21.log")。
	std::vector<std::string> logFileNames(void);
	// ログディレクトリ内の指定名のログファイルを削除する。return: 成功。
	bool removeLog(const std::string& name);

	// 内蔵フラッシュ(LittleFS)の /log 配下のログファイルをすべて削除する。
	// SD 採用中でも内蔵ログのみを対象にする。return: 削除数(マウント失敗時 -1)。
	// 注: 実装はメディア固有のため M5Stack のみ。他プラットフォームは未参照(未定義)。
	int removeInternalLogs(void);
}

#endif // _OS_FILE_H_
