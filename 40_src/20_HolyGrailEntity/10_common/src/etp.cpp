// etp パケットの組立/解釈(データ構造仕様書43 §6)。
#include "etp.h"
#include <cstring>

namespace etp
{
	namespace
	{
		void putU16(std::vector<uint8_t>& v, uint16_t x)
		{
			v.push_back(static_cast<uint8_t>(x & 0xFF));
			v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
		}
		void putU32(std::vector<uint8_t>& v, uint32_t x)
		{
			v.push_back(static_cast<uint8_t>(x & 0xFF));
			v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
			v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
			v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
		}
		uint16_t getU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
		uint32_t getU32(const uint8_t* p)
		{
			return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
		}

		// 4 バイトごとの u32(リトルエンディアン)として総和する。端数は 0 詰め。
		uint32_t sumWords(const uint8_t* p, size_t n)
		{
			uint32_t s = 0;
			for (size_t i = 0; i < n; i += 4)
			{
				uint32_t w = 0;
				for (int b = 0; b < 4; ++b)
				{
					size_t idx = i + static_cast<size_t>(b);
					if (idx < n) { w |= static_cast<uint32_t>(p[idx]) << (8 * b); }
				}
				s += w;
			}
			return s;
		}
	}

	std::vector<uint8_t> encode(uint16_t cmd, uint16_t method, const std::string& data)
	{
		// data を 4 バイト境界へ空白で詰める(JSONは末尾空白を無視する)。
		std::string d = data;
		while (d.size() % 4 != 0) { d.push_back(' '); }
		const uint32_t length = static_cast<uint32_t>(d.size());

		std::vector<uint8_t> v;
		v.reserve(18 + d.size());
		putU16(v, HEADER);
		putU16(v, cmd);
		putU16(v, method);
		putU32(v, length);
		v.insert(v.end(), d.begin(), d.end());
		putU32(v, TERMINAL);

		// header から terminal までの総和の 2 の補数を sum とする。
		uint32_t sum = static_cast<uint32_t>(0u - sumWords(v.data(), v.size()));
		putU32(v, sum);
		return v;
	}

	int decode(const uint8_t* buf, size_t len, packet& out)
	{
		if (len < 10) { return 0; }					// header..length 未達
		if (getU16(buf) != HEADER) { return -1; }	// 同期外れ
		uint32_t length = getU32(buf + 6);
		if (length % 4 != 0) { return -1; }			// data は 4 の整数倍
		size_t total = 18 + static_cast<size_t>(length);	// header..sum
		if (len < total) { return 0; }				// パケット未達

		const uint8_t* term = buf + 10 + length;
		if (getU32(term) != TERMINAL) { return -1; }

		// sum 検証: header..terminal の総和の 2 の補数 == 格納された sum
		uint32_t expect = static_cast<uint32_t>(0u - sumWords(buf, 14 + static_cast<size_t>(length)));
		uint32_t got    = getU32(term + 4);
		if (expect != got) { return -1; }

		out.cmd    = getU16(buf + 2);
		out.method = getU16(buf + 4);
		out.data.assign(reinterpret_cast<const char*>(buf + 10), length);
		// 末尾の詰め空白を除去
		while (!out.data.empty() && (out.data.back() == ' ' || out.data.back() == '\0'))
		{
			out.data.pop_back();
		}
		return static_cast<int>(total);
	}
}
