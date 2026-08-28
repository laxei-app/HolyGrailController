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
// 【nc(ノンスカウンタ)は順番が命】RFC 2617 の nc は「この nonce で何回目か」で、サーバは
//  リプレイ攻撃検知に使う。EOS R50 V は**到着順が入れ替わっただけで弾く**(実測)。ワーカー2本が
//  同じカメラへ並行に投げると必ず起きるので、認証が要る相手へは hostGuard で1本ずつ投げる。
//  また、同じ nonce のまま nc を1に戻すと「使用済みの nc」= リプレイと見なされ、以後何をしても
//  401 になる。nc を巻き戻してよいのは nonce が変わったときだけ。
//
// 【nc は時刻から始める】EOS R50 V は nonce を使い回し、nc を**プロセスをまたいで**覚えている。
//  実測した受理条件は「その nonce で見た最大値より大きければよい。飛びは自由」
//  (nc=2 の直後に nc=100000 が通り、その後 nc=50 は弾かれた)。つまりアプリを再起動して 1 から
//  数え直すと、前回の続きより小さいので**必ず弾かれる**。カメラの控えは知りようがないので、
//  時刻(2020-01-01 からの秒×8)を種にして「次に起動したときのほうが必ず大きい」ようにする。
//
// 【資格情報は候補で持つ】発見の段階では「その IP がどの所持カメラか」がまだ確定しない
//  (機種やシリアルを取るのに CCAPI を叩く必要があり、それ自体が 401 になる)。そこで所持カメラの
//  ユーザーID/パスワードを候補として全部渡しておき、通ったものを host ごとに覚える。
//  台数は数台なので、初回に何度か試すだけで済む。
#include <string>
#include <utility>
#include <vector>

// 【カメラの nc の扱い(2026-08-28 EOS R50 V で実測)】
//  カメラは端末を区別していない。見ているのは nc が **前回受け入れた値より大きいか** だけ。
//   ・同じ nc の再送 → 401 / 小さい nc へ戻る → 401 / 大きい nc へ飛ぶ → 200
//   ・nonce を貰い直しても nc の記憶は消えない(nonce ごとではなく一本で覚えている)
//   ・弾かれても、前回受け入れた値より大きい nc を出せば回復する(締め出しではない)
//  だから nc の種を時刻から作る。後から接続した端末のほうが必ず大きい種から始まるので、
//  スマホとエッジのどちらからでも**順番に**使える(同時使用は成立しない)。
//  **時計が未設定の端末は認証を送らない**。種が 0 になり、他が一度触ると追いつけないため。

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

	// 認証が通らないときに、何が起きているかをログへ出すための覗き窓(診断専用)。
	//  「資格情報が無い」のか「打ち止めになった」のか「nonce を毎回作り直されている」のかは
	//  外から見るとどれも 401 で、区別がつかない。パスワードそのものは絶対に出さない。
	//  戻り: 例 "creds=2 idx=0 known=1 exhausted=0 bumped=0 nc=64285d0e nonce=7a1f..."
	std::string diagnose(const std::string& host);

	// 401 の WWW-Authenticate を覚える。true=この host へ再送する価値がある。
	//  同じ nonce で再び 401 が来たときの扱いが要点:
	//   ・stale=true、または**一度でも通ったことがある資格情報**なら nc/順序の問題とみなし、
	//     資格情報も nc も維持したまま投げ直す(ここで nc を戻すとリプレイ扱いで永久に通らない)。
	//   ・まだ一度も通っていないなら本当に資格情報が違うので次の候補へ進む。
	//  候補を使い切ったら false を返し、以後その host は**打ち止め**にする。誤った資格情報での
	//  認証失敗を投げ続けると、カメラによっては完全に締め出される(EOS R50 V は 403 "Not access"
	//  になり本体の設定を入れ直すまで戻らない)。打ち止めは setCandidates/addCandidate で解ける。
	bool learn(const std::string& host, const std::string& wwwAuthenticate);

	// 認証付きの要求が通った(401 以外が返った)ことを記録する。次に同じ nonce で 401 が来ても
	//  「パスワードが違う」と誤判定しないために要る。
	void noteSuccess(const std::string& host);

	// 認証が要る相手へは1本ずつ投げるための錠。候補が1つも無ければ何もしない(=既存の動作のまま)。
	//  カメラごとに別の錠なので、2台同時撮影の並行性は落ちない。
	class hostGuard
	{
	public:
		explicit hostGuard(const std::string& host);
		~hostGuard();
		hostGuard(const hostGuard&) = delete;
		hostGuard& operator=(const hostGuard&) = delete;
	private:
		std::string host_;
		bool        held_ = false;
	};

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
