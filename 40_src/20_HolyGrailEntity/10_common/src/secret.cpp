#include "secret.h"
#include <cstdint>
#include <cstring>
#include <ctime>

// ChaCha20(RFC 8439)。鍵はここに固定で持つ。意図と限界はヘッダの説明を参照。
namespace
{
	// 固定鍵(32byte)。変更すると**既に保存済みのパスワードは復号できなくなる**ので、
	// 変えるときはユーザーに入力し直してもらう必要がある。
	const uint8_t kKey[32] =
	{
		0x8f, 0x2a, 0x61, 0xd4, 0x0c, 0x93, 0x7e, 0x15,
		0xb8, 0x47, 0x2c, 0xe9, 0x5a, 0x30, 0xf1, 0x6d,
		0x24, 0xbb, 0x09, 0x7a, 0xd3, 0x58, 0x8e, 0x41,
		0x1f, 0xc6, 0x35, 0xa2, 0x77, 0xe0, 0x4b, 0x99,
	};

	inline uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

	inline void quarterRound(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d)
	{
		a += b; d ^= a; d = rotl32(d, 16);
		c += d; b ^= c; b = rotl32(b, 12);
		a += b; d ^= a; d = rotl32(d,  8);
		c += d; b ^= c; b = rotl32(b,  7);
	}

	// 鍵ストリームを1ブロック(64byte)作る。
	void chachaBlock(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64])
	{
		static const char kSigma[] = "expand 32-byte k";
		uint32_t st[16];
		std::memcpy(&st[0], kSigma, 16);
		std::memcpy(&st[4], key, 32);
		st[12] = counter;
		std::memcpy(&st[13], nonce, 12);

		uint32_t w[16];
		std::memcpy(w, st, sizeof(w));
		for (int i = 0; i < 10; ++i)		// 20ラウンド = (列4 + 対角4) × 10
		{
			quarterRound(w[0], w[4], w[ 8], w[12]);
			quarterRound(w[1], w[5], w[ 9], w[13]);
			quarterRound(w[2], w[6], w[10], w[14]);
			quarterRound(w[3], w[7], w[11], w[15]);
			quarterRound(w[0], w[5], w[10], w[15]);
			quarterRound(w[1], w[6], w[11], w[12]);
			quarterRound(w[2], w[7], w[ 8], w[13]);
			quarterRound(w[3], w[4], w[ 9], w[14]);
		}
		for (int i = 0; i < 16; ++i) { w[i] += st[i]; }
		std::memcpy(out, w, 64);
	}

	// data を鍵ストリームで XOR する(暗号化と復号は同じ処理)。
	void chachaXor(const uint8_t nonce8[8], uint8_t* data, size_t len)
	{
		uint8_t nonce[12] = {0};
		std::memcpy(nonce + 4, nonce8, 8);	// 先頭4byteは0固定(RFC8439 の 96bit nonce へ埋める)
		uint8_t ks[64];
		uint32_t counter = 1;
		for (size_t off = 0; off < len; off += 64, ++counter)
		{
			chachaBlock(kKey, counter, nonce, ks);
			const size_t n = ((len - off) < 64) ? (len - off) : 64;
			for (size_t i = 0; i < n; ++i) { data[off + i] ^= ks[i]; }
		}
	}

	const char* kPrefix = "v1:";

	char hexDigit(int v) { return static_cast<char>((v < 10) ? ('0' + v) : ('a' + v - 10)); }

	int hexVal(char c)
	{
		if (c >= '0' && c <= '9') { return c - '0'; }
		if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
		if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
		return -1;
	}

	// nonce を作る。暗号学的な乱数である必要はない(鍵が固定なので、そこは元々守れていない)。
	//  求めているのは「同じ平文が毎回同じ暗号文にならない」ことだけ。
	void makeNonce(uint8_t out[8])
	{
		static uint32_t seq = 0;
		const uint32_t t = static_cast<uint32_t>(std::time(nullptr));
		const uint32_t s = ++seq;
		for (int i = 0; i < 4; ++i) { out[i]     = static_cast<uint8_t>((t >> (8 * i)) & 0xff); }
		for (int i = 0; i < 4; ++i) { out[4 + i] = static_cast<uint8_t>((s >> (8 * i)) & 0xff); }
	}
}

namespace secret
{
	bool isEncrypted(const std::string& s)
	{
		return s.compare(0, std::strlen(kPrefix), kPrefix) == 0;
	}

	std::string encrypt(const std::string& plain)
	{
		if (plain.empty()) { return std::string(); }	// 未設定は未設定のまま

		uint8_t nonce[8];
		makeNonce(nonce);

		std::string buf = plain;
		chachaXor(nonce, reinterpret_cast<uint8_t*>(&buf[0]), buf.size());

		std::string out = kPrefix;
		out.reserve(out.size() + (8 + buf.size()) * 2);
		for (int i = 0; i < 8; ++i)
		{
			out += hexDigit((nonce[i] >> 4) & 0xf); out += hexDigit(nonce[i] & 0xf);
		}
		for (size_t i = 0; i < buf.size(); ++i)
		{
			const uint8_t b = static_cast<uint8_t>(buf[i]);
			out += hexDigit((b >> 4) & 0xf); out += hexDigit(b & 0xf);
		}
		return out;
	}

	std::string decrypt(const std::string& stored)
	{
		if (!isEncrypted(stored)) { return stored; }	// 平文はそのまま(ヘッダの説明を参照)

		const std::string hex = stored.substr(std::strlen(kPrefix));
		if ((hex.size() % 2) != 0 || hex.size() < 16) { return std::string(); }	// nonce にも満たない

		std::string raw;
		raw.reserve(hex.size() / 2);
		for (size_t i = 0; i < hex.size(); i += 2)
		{
			const int hi = hexVal(hex[i]), lo = hexVal(hex[i + 1]);
			if (hi < 0 || lo < 0) { return std::string(); }		// 壊れている
			raw += static_cast<char>((hi << 4) | lo);
		}

		uint8_t nonce[8];
		std::memcpy(nonce, raw.data(), 8);
		std::string body = raw.substr(8);
		if (!body.empty()) { chachaXor(nonce, reinterpret_cast<uint8_t*>(&body[0]), body.size()); }
		return body;
	}
}
