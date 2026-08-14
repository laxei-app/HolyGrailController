#ifndef _HTTP_AUTH_H_
#define _HTTP_AUTH_H_
// HTTP ダイジェスト認証(RFC 2617)。CCAPI がカメラ側の設定で要求してくる。
//
// 【事前に知る必要は無い】認証が要るかどうかはカメラに聞かなくても分かる。要求してくるのは
//  サーバ側で、401 の WWW-Authenticate に realm/nonce/opaque/algorithm/qop が全部入っている
//  (CCAPI Reference 3.3.3: realm="CameraControlApi", algorithm="MD5", qop="auth")。
//  こちらが用意しておく必要があるのはユーザーIDとパスワードだけ。
//
// 【流れ】
//   ① Authorization 無しで投げる
//   ② 401 が返ったら learn() で覚える
//   ③ authorization() でヘッダを作って同じ要求を投げ直す
//   ④ 以降その host には最初から付ける(毎回2往復にしない)
//   ⑤ nonce が切れて再び 401 が来たら ② に戻る
//
// 【資格情報は候補で持つ】発見の段階では「その IP がどの所持カメラか」がまだ確定しない
//  (機種やシリアルを取るのに CCAPI を叩く必要があり、それ自体が 401 になる)。そこで所持カメラの
//  ユーザーID/パスワードを候補として全部渡しておき、通ったものを host ごとに覚える。
//  台数は数台なので、初回に何度か試すだけで済む。
#include <string>
#include <utility>
#include <vector>

namespace httpAuth
{
	// 使う資格情報の候補(所持カメラから集めたもの。パスワードは**復号済みの平文**を渡す)。
	void setCandidates(const std::vector<std::pair<std::string, std::string>>& userPass);

	// 候補を1つ足す(既にあれば何もしない)。設定済みの学習内容は捨てない。
	//  カメラ情報を JSON から読むたびに呼ばれる。所持カメラの編集・撮影計画の受信・起動時の
	//  読み込みのどれを通っても、同じ入口(csjson::cameraFromJson)で登録される。
	void addCandidate(const std::string& user, const std::string& pass);

	// 候補が1つでも登録されているか(0なら 401 が来ても打つ手が無い)。
	bool hasCandidates(void);

	// 401 の WWW-Authenticate を覚える。true=この host へ再送する価値がある。
	//  同じ nonce で再び 401 が来たら「その資格情報では通らない」と判断し、次の候補へ進む。
	//  候補を使い切ったら false を返す(呼び側は諦めて 401 をそのまま上へ返す)。
	bool learn(const std::string& host, const std::string& wwwAuthenticate);

	// Authorization ヘッダの値を作る。まだ何も覚えていなければ空文字。
	//  uri は絶対URLではなくパス("/ccapi/ver100/..." の部分)を渡すこと。
	std::string authorization(const std::string& host, const std::string& method, const std::string& uri);

	// その host について覚えていることを捨てる。
	void reset(const std::string& host);
	void resetAll(void);

	// --- 単体テスト用に計算だけ切り出したもの ---
	// RFC 2617 の response 値。qop が空なら RFC 2069 形式(nc/cnonce を使わない)。
	std::string responseHash(const std::string& user, const std::string& pass,
	                         const std::string& realm, const std::string& nonce,
	                         const std::string& method, const std::string& uri,
	                         const std::string& qop, const std::string& nc,
	                         const std::string& cnonce);
}

#endif // _HTTP_AUTH_H_
