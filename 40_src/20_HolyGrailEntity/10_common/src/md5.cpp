#include "md5.h"
#include <cstdint>
#include <cstring>

// RFC 1321 のとおりの実装。意図と限界はヘッダの説明を参照。
namespace
{
	struct ctx
	{
		uint32_t h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
		uint64_t len  = 0;			// 入力の総バイト数
		uint8_t  buf[64];
		size_t   fill = 0;			// buf に溜まっている量
	};

	const uint32_t kT[64] =
	{
		0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
		0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
		0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
		0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
		0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
		0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
		0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
		0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u,
	};

	const int kS[64] =
	{
		7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
		5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
		4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
		6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
	};

	inline uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

	void block(ctx& c, const uint8_t p[64])
	{
		uint32_t m[16];
		for (int i = 0; i < 16; ++i)
		{
			m[i] = static_cast<uint32_t>(p[i * 4])
			     | (static_cast<uint32_t>(p[i * 4 + 1]) <<  8)
			     | (static_cast<uint32_t>(p[i * 4 + 2]) << 16)
			     | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
		}
		uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
		for (int i = 0; i < 64; ++i)
		{
			uint32_t f; int g;
			if      (i < 16) { f = (b & cc) | (~b & d);        g = i; }
			else if (i < 32) { f = (d & b)  | (~d & cc);       g = (5 * i + 1) & 15; }
			else if (i < 48) { f = b ^ cc ^ d;                 g = (3 * i + 5) & 15; }
			else             { f = cc ^ (b | ~d);              g = (7 * i)     & 15; }
			const uint32_t tmp = d;
			d = cc; cc = b;
			b = b + rotl(a + f + kT[i] + m[g], kS[i]);
			a = tmp;
		}
		c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
	}

	void update(ctx& c, const uint8_t* p, size_t n)
	{
		c.len += n;
		while (n > 0)
		{
			const size_t take = ((64 - c.fill) < n) ? (64 - c.fill) : n;
			std::memcpy(c.buf + c.fill, p, take);
			c.fill += take; p += take; n -= take;
			if (c.fill == 64) { block(c, c.buf); c.fill = 0; }
		}
	}

	void finish(ctx& c, uint8_t out[16])
	{
		const uint64_t bits = c.len * 8;
		const uint8_t pad = 0x80;
		update(c, &pad, 1);
		const uint8_t zero = 0;
		while (c.fill != 56) { update(c, &zero, 1); }
		uint8_t lenLe[8];
		for (int i = 0; i < 8; ++i) { lenLe[i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff); }
		// update だと len が動くのでここは直接書く
		std::memcpy(c.buf + 56, lenLe, 8);
		block(c, c.buf);
		for (int i = 0; i < 4; ++i)
		{
			out[i * 4]     = static_cast<uint8_t>( c.h[i]        & 0xff);
			out[i * 4 + 1] = static_cast<uint8_t>((c.h[i] >>  8) & 0xff);
			out[i * 4 + 2] = static_cast<uint8_t>((c.h[i] >> 16) & 0xff);
			out[i * 4 + 3] = static_cast<uint8_t>((c.h[i] >> 24) & 0xff);
		}
	}
}

namespace md5
{
	std::string hex(const std::string& data)
	{
		ctx c;
		update(c, reinterpret_cast<const uint8_t*>(data.data()), data.size());
		uint8_t d[16];
		finish(c, d);
		static const char* kHex = "0123456789abcdef";
		std::string out;
		out.reserve(32);
		for (int i = 0; i < 16; ++i) { out += kHex[(d[i] >> 4) & 0xf]; out += kHex[d[i] & 0xf]; }
		return out;
	}
}
