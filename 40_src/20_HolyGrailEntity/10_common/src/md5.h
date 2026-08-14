#ifndef _MD5_H_
#define _MD5_H_
// MD5(RFC 1321)。ダイジェスト認証のためだけに置いている。
//
// 【なぜ自前実装か】エッジは mbedTLS を持っているがスマホのネイティブには無い。JNI で Java の
//  MessageDigest を呼ぶと**エッジとスマホで別実装**になり、片方だけで壊れる不具合を作りやすい。
//  MD5 は短いので1つに保つ。
//
// 【念のため】MD5 はハッシュとしてはもう安全ではない。ここで使うのは CCAPI のダイジェスト認証が
//  algorithm="MD5" を要求するからであって(Reference 3.3.3)、選んだわけではない。
//  他の用途に流用しないこと。
#include <string>

namespace md5
{
	// 入力の MD5 を小文字16進32文字で返す(ダイジェスト認証はこの形式で連結する)。
	std::string hex(const std::string& data);
}

#endif // _MD5_H_
