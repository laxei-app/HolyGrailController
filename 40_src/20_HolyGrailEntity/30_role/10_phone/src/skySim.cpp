// 撮影シミュレーション(スマホ専用・§7.3 画面360)。
//  指定時刻・撮影方向(方位/仰角)・レンズ(平面/魚眼)・センサー比から、恒星・惑星・太陽・月を
//  カメラの画角平面へ投影し、画角内に入る天体の正規化座標を返す。UI(SkySimView)が描画する。
//
//  ・恒星は fixed_star.json(ra[時],dec[度],mag,color)を hge_simLoadStars で一度読み込む。
//  ・投影は平面(直線)レンズ=ノモニック(端ほど間延び=tan)、魚眼=等距離(r=fθ)で切り替える。
//  ・センサー比(縦横)に合わせた正規化座標 x,y∈[-1,1](x=右+ / y=上+)を返し、|x|,|y|<=1 が画角内。
//
//  ネイティブ天文計算(Astronomy Engine)はスマホ側にしか無い機能なので 30_role/10_phone に置く
//  (エッジ=M5Stack/StickS3 のビルドには含めない=RAM/サイズに影響させない)。
#include "holyGrailEntity.h"
#include "errorCode.h"
#include <astronomy/astronomy.h>
#include <json/nlohmann/json.hpp>
#include <vector>
#include <string>
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>

using json = nlohmann::json;

namespace {
	constexpr double PI  = 3.14159265358979323846;
	constexpr double RAD = PI / 180.0;

	struct SimStar { std::string name; double ra; double dec; double mag; std::string color; };
	std::vector<SimStar> g_stars;
	std::mutex           g_starMtx;

	struct Vec3 { double x, y, z; };
	inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline Vec3   cross(const Vec3& a, const Vec3& b) {
		return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	}

	// 方位[°](0=北,90=東)・高度[°] → 単位ベクトル(東,北,上)
	inline Vec3 dirFromAzAlt(double azDeg, double altDeg) {
		double a = azDeg * RAD, h = altDeg * RAD;
		return { std::cos(h) * std::sin(a), std::cos(h) * std::cos(a), std::sin(h) };
	}

	// 天体(方位/高度)をカメラのセンサー平面へ投影。visible なら nx,ny∈[-1,1] を返す。
	//  カメラ光軸 b、右方向 r(方位+方向で水平)、カメラ上 u=r×b。
	//  平面レンズ: nx = (xc/zc)/tan(fovW/2)  端ほど間延び(tan)。
	//  魚眼(等距離): nx = (θ・xc/√(xc²+yc²))/(fovW/2)  θ=光軸からの角。
	bool project(double objAz, double objAlt, double camAz, double camEl,
	             double fovW, double fovH, bool fisheye, double& nx, double& ny) {
		Vec3 d = dirFromAzAlt(objAz, objAlt);
		Vec3 b = dirFromAzAlt(camAz, camEl);
		Vec3 r = { std::cos(camAz * RAD), -std::sin(camAz * RAD), 0.0 };
		Vec3 u = cross(r, b);
		double xc = dot(d, r), yc = dot(d, u), zc = dot(d, b);
		if (!fisheye) {
			if (zc <= 0.02) { return false; }			// 背後/真横は平面レンズでは写らない
			double X = xc / zc, Y = yc / zc;
			nx = X / std::tan(fovW / 2.0);
			ny = Y / std::tan(fovH / 2.0);
		} else {
			double cz = zc; if (cz > 1.0) { cz = 1.0; } if (cz < -1.0) { cz = -1.0; }
			double theta = std::acos(cz);
			double s = std::sqrt(xc * xc + yc * yc);
			if (s < 1e-9) { nx = 0.0; ny = 0.0; }
			else { nx = (theta * (xc / s)) / (fovW / 2.0); ny = (theta * (yc / s)) / (fovH / 2.0); }
		}
		return (std::fabs(nx) <= 1.0 && std::fabs(ny) <= 1.0);
	}

	// ローカル日時 → Astronomy Engine の時刻(真のUTC瞬時)
	astro_time_t makeUtc(int y, int mo, int d, int h, int mi, int s, int offMin) {
		astro_time_t t = Astronomy_MakeTime(y, mo, d, h, mi, static_cast<double>(s));
		return Astronomy_AddDays(t, -static_cast<double>(offMin) / 1440.0);
	}
} // namespace

extern "C" {

// fixed_star.json 配列 [{name,ra,dec,mag,color_code,...}] を読み込む。戻り=星数、負=エラー。
int32_t hge_simLoadStars(const char* starsJson) {
	if (starsJson == nullptr) { return ERR_HGC_INVALID_ARG; }
	try {
		json j = json::parse(starsJson);
		std::lock_guard<std::mutex> lk(g_starMtx);
		g_stars.clear();
		if (j.is_array()) {
			g_stars.reserve(j.size());
			for (auto& e : j) {
				SimStar s;
				s.name  = e.value("name", std::string());
				s.ra    = e.value("ra", 0.0);
				s.dec   = e.value("dec", 0.0);
				s.mag   = e.value("mag", 99.0);
				s.color = e.value("color_code", std::string("#FFFFFF"));
				g_stars.push_back(std::move(s));
			}
		}
		return static_cast<int32_t>(g_stars.size());
	} catch (...) { return ERR_HGC_JSON_PARSE; }
}

// params(JSON) から画角内の天体を投影して返す(バッファ規約)。
//  params = { "datetime":"YYYY/MM/DD HH:MM:SS", "offMin":int, "lat","lon","alt":double,
//             "az","el":double(撮影方向/仰角), "landscape":0/1, "fisheye":0/1,
//             "focal":mm, "sensorW","sensorH":mm, "magLimit":double(既定6.5) }
//  返り = { "objects":[{ "name","x","y","mag","color","kind"(star/sun/moon/planet) }...],
//           "aspect":横/縦, "landscape":0/1, "fisheye":0/1 }  x,y∈[-1,1](x右+ y上+)
int32_t hge_simulateSky(const char* paramsJson, char* buf, int32_t* inoutLen) {
	if (inoutLen == nullptr) { return ERR_HGC_INVALID_ARG; }
	std::string outStr = "{\"objects\":[]}";
	try {
		json p = json::parse(paramsJson ? paramsJson : "{}");
		int y = 2025, mo = 1, d = 1, h = 0, mi = 0, s = 0;
		std::string dt = p.value("datetime", std::string());
		if (!dt.empty()) { std::sscanf(dt.c_str(), "%d/%d/%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s); }
		int    offMin  = p.value("offMin", 0);
		double lat = p.value("lat", 0.0), lon = p.value("lon", 0.0), alt = p.value("alt", 0.0);
		double camAz   = p.value("az", 0.0), camEl = p.value("el", 0.0);
		bool   landscape = p.value("landscape", 1) != 0;
		bool   fisheye   = p.value("fisheye", 0) != 0;
		double focal   = p.value("focal", 50.0);
		double sensorW = p.value("sensorW", 36.0);
		double sensorH = p.value("sensorH", 24.0);
		double magLimit = p.value("magLimit", 6.5);
		if (focal <= 0.0) { focal = 50.0; }
		if (sensorW <= 0.0) { sensorW = 36.0; }
		if (sensorH <= 0.0) { sensorH = 24.0; }

		// 縦向き(portrait)は横辺・縦辺を入れ替える(画像の横=fw / 縦=fh)。
		double fw = landscape ? sensorW : sensorH;
		double fh = landscape ? sensorH : sensorW;
		double fovW, fovH;
		if (!fisheye) {
			fovW = 2.0 * std::atan2(fw / 2.0, focal);		// ノモニック(直線)
			fovH = 2.0 * std::atan2(fh / 2.0, focal);
		} else {
			fovW = fw / focal; if (fovW > PI) { fovW = PI; }	// 等距離魚眼(最大180°)
			fovH = fh / focal; if (fovH > PI) { fovH = PI; }
		}

		astro_time_t     t   = makeUtc(y, mo, d, h, mi, s, offMin);
		astro_observer_t obs = Astronomy_MakeObserver(lat, lon, alt);

		json objs = json::array();

		// 恒星(赤経[時]/赤緯[度]をそのまま地平座標へ)
		{
			std::lock_guard<std::mutex> lk(g_starMtx);
			for (auto& st : g_stars) {
				if (st.mag > magLimit) { continue; }
				astro_horizon_t hz = Astronomy_Horizon(&t, obs, st.ra, st.dec, REFRACTION_NORMAL);
				double nx, ny;
				if (project(hz.azimuth, hz.altitude, camAz, camEl, fovW, fovH, fisheye, nx, ny)) {
					objs.push_back({ {"name", st.name}, {"x", nx}, {"y", ny},
					                 {"mag", st.mag}, {"color", st.color}, {"kind", "star"} });
				}
			}
		}

		// 太陽・月・惑星
		struct B { astro_body_t body; const char* kind; const char* color; const char* name; };
		const B bodies[] = {
			{ BODY_SUN,     "sun",    "#FFF3B0", "Sun"     },
			{ BODY_MOON,    "moon",   "#ECECE0", "Moon"    },
			{ BODY_MERCURY, "planet", "#B8B0A0", "Mercury" },
			{ BODY_VENUS,   "planet", "#FFF3D0", "Venus"   },
			{ BODY_MARS,    "planet", "#FF6B4A", "Mars"    },
			{ BODY_JUPITER, "planet", "#E6D6AC", "Jupiter" },
			{ BODY_SATURN,  "planet", "#E9DCA6", "Saturn"  },
		};
		for (const auto& bd : bodies) {
			astro_equatorial_t eq = Astronomy_Equator(bd.body, &t, obs, EQUATOR_OF_DATE, ABERRATION);
			if (eq.status != ASTRO_SUCCESS) { continue; }
			astro_horizon_t hz = Astronomy_Horizon(&t, obs, eq.ra, eq.dec, REFRACTION_NORMAL);
			double nx, ny;
			if (project(hz.azimuth, hz.altitude, camAz, camEl, fovW, fovH, fisheye, nx, ny)) {
				double mag = 99.0;
				astro_illum_t il = Astronomy_Illumination(bd.body, t);
				if (il.status == ASTRO_SUCCESS) { mag = il.mag; }
				objs.push_back({ {"name", bd.name}, {"x", nx}, {"y", ny},
				                 {"mag", mag}, {"color", bd.color}, {"kind", bd.kind} });
			}
		}

		json out;
		out["objects"]   = objs;
		out["aspect"]    = fw / (fh > 0.0 ? fh : 1.0);
		out["landscape"] = landscape ? 1 : 0;
		out["fisheye"]   = fisheye ? 1 : 0;
		outStr = out.dump();
	} catch (...) { outStr = "{\"objects\":[]}"; }

	int32_t need = static_cast<int32_t>(outStr.size()) + 1;
	if (buf == nullptr || *inoutLen < need) { *inoutLen = need; return ERR_HGC_BUF_SHORT; }
	std::memcpy(buf, outStr.c_str(), need);
	*inoutLen = need;
	return ERR_HGC_OK;
}

} // extern "C"
