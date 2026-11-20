#include "simlab/render/Globe.hpp"
#include "simlab/WorldGen.hpp"
#include <algorithm>
#include <cmath>

namespace simlab::render {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t rgb(int r, int g, int b) {
    return 0xFF000000u | (std::uint32_t(std::clamp(r, 0, 255)) << 16) |
           (std::uint32_t(std::clamp(g, 0, 255)) << 8) | std::uint32_t(std::clamp(b, 0, 255));
}
std::uint32_t mix(std::uint32_t a, std::uint32_t b, int t /*0..256*/) {
    auto ch = [&](int s) {
        const int x = int((a >> s) & 255), y = int((b >> s) & 255);
        return std::uint32_t(x + (y - x) * t / 256) << s;
    };
    return 0xFF000000u | ch(16) | ch(8) | ch(0);
}

// biome colour from analytic fields (elevation m, temperature deci-C, moisture permille)
std::uint32_t biome(std::int32_t e, std::int32_t t, std::int32_t m, std::int32_t sea) {
    if (e < sea) {
        const int depth = sea - e;
        if (t < -60) return rgb(206, 222, 234);                        // pack ice
        const int k = std::clamp(depth * 256 / 160, 0, 256);
        return mix(rgb(64, 138, 168), rgb(18, 44, 104), k);            // shallow -> deep
    }
    if (t < -30 || e > 500) return rgb(236, 240, 246);                 // ice cap / summit snow
    if (e > 440) return rgb(128, 116, 104);                            // bare mountains
    if (t < 50) return mix(rgb(148, 150, 126), rgb(200, 205, 200), std::clamp((50 - t) * 3, 0, 256));   // tundra
    if (m < 230) return rgb(206, 182, 122);                            // desert / dry steppe
    if (t > 270 && m > 640) return rgb(30, 98, 44);                    // rainforest
    if (m > 520) return rgb(58, 122, 52);                              // forest
    return rgb(122, 160, 78);                                          // grassland
}

}  // namespace

std::uint32_t planet_cell_color(std::uint64_t seed, int gx, int gy, std::int32_t sea) {
    const std::int32_t e = WorldGenerator::elevation_at(seed, gx, gy);
    const std::int32_t t = WorldGenerator::temperature_at(seed, gx, gy, sea);
    const std::int32_t m = e < sea ? 0 : WorldGenerator::moisture_at(seed, gx, gy, sea);
    std::uint32_t c = biome(e, t, m, sea);
    // relief: a cheap east-west slope shade
    if (e >= sea) {
        const std::int32_t ex = WorldGenerator::elevation_at(seed, gx + 1, gy);
        c = mix(c, (e - ex) > 0 ? rgb(255, 250, 230) : rgb(20, 20, 30), std::clamp(std::abs(e - ex) * 6, 0, 70));
    }
    return c;
}

void PlanetMap::build(std::uint64_t seed, int w, int h, int width) {
    gen_seed = seed; pw = w; ph = h;
    aw = std::clamp(std::min(width, w), 64, 2048);
    ah = aw / 2;
    albedo.assign(std::size_t(aw) * ah, 0);
    // moisture is the costly field (sea proximity): sample it 4x coarser
    const int mw = aw / 4, mh = ah / 4;
    std::vector<std::int32_t> moist(std::size_t(mw) * mh);
    for (int y = 0; y < mh; ++y)
        for (int x = 0; x < mw; ++x)
            moist[std::size_t(y) * mw + x] = WorldGenerator::moisture_at(
                seed, int((long long)(x * 4 + 2) * pw / aw), int((long long)(y * 4 + 2) * ph / ah), 200);
    for (int y = 0; y < ah; ++y) {
        const int gy = int((long long)y * ph / ah + ph / (2 * ah));
        for (int x = 0; x < aw; ++x) {
            const int gx = int((long long)x * pw / aw + pw / (2 * aw));
            const std::int32_t e = WorldGenerator::elevation_at(seed, gx, gy);
            const std::int32_t t = WorldGenerator::temperature_at(seed, gx, gy, 200);
            const std::int32_t m = moist[std::size_t(std::min(y / 4, mh - 1)) * mw + std::min(x / 4, mw - 1)];
            std::uint32_t c = biome(e, t, m, 200);
            if (e >= 200) {
                const std::int32_t en = WorldGenerator::elevation_at(seed, gx + pw / aw + 1, gy);
                c = mix(c, (e - en) > 0 ? rgb(255, 250, 230) : rgb(20, 20, 30), std::clamp(std::abs(e - en) * 4, 0, 60));
            }
            albedo[std::size_t(y) * aw + x] = c;
        }
    }
}

void PlanetTiles::reset(std::uint64_t seed, int pw, int ph, std::size_t max_tiles) {
    seed_ = seed; pw_ = pw; ph_ = ph; max_ = max_tiles;
    lru_.clear(); map_.clear();
}

const std::vector<std::uint32_t>* PlanetTiles::tile(int cx, int cy) {
    if (pw_ <= 0) return nullptr;
    const int ncx = (pw_ + kChunk - 1) / kChunk;
    cx = ((cx % ncx) + ncx) % ncx;
    if (cy < 0 || cy * kChunk >= ph_) return nullptr;
    const std::uint64_t k = key(cx, cy);
    if (auto it = map_.find(k); it != map_.end()) {
        lru_.splice(lru_.begin(), lru_, it->second.it);
        return &it->second.px;
    }
    if (map_.size() >= max_) {
        map_.erase(lru_.back());
        lru_.pop_back();
    }
    Slot s;
    s.px.resize(kChunk * kChunk);
    for (int y = 0; y < kChunk; ++y)
        for (int x = 0; x < kChunk; ++x) {
            const int gy = cy * kChunk + y;
            s.px[std::size_t(y) * kChunk + x] = gy < ph_ ? planet_cell_color(seed_, cx * kChunk + x, gy) : 0xFF000000u;
        }
    lru_.push_front(k);
    s.it = lru_.begin();
    return &map_.emplace(k, std::move(s)).first->second.px;
}

void GlobeRenderer::rebuild(const PlanetMap& m, const GlobeView& v) {
    const int R = std::max(4, int(v.radius));
    px_.clear(); rim_.clear();
    const double cl = std::cos(v.lat), sl = std::sin(v.lat);
    const int Rr = int(R * 1.09) + 1;
    for (int y = -Rr; y <= Rr; ++y)
        for (int x = -Rr; x <= Rr; ++x) {
            const double d2 = double(x) * x + double(y) * y;
            if (d2 >= double(R) * R) {
                const double d = std::sqrt(d2) / R;
                if (d < 1.09) rim_.push_back({x, y, std::uint8_t(std::clamp(int(200 * (1.09 - d) / 0.09), 0, 200))});
                continue;
            }
            const double nx = x / double(R), ny = -y / double(R);
            const double nz = std::sqrt(std::max(0.0, 1 - nx * nx - ny * ny));
            // undo the view tilt: world = Rx(lat) * n
            const double wy = ny * cl + nz * sl;
            const double wz = -ny * sl + nz * cl;
            const double lat = std::asin(std::clamp(wy, -1.0, 1.0));
            const double lonr = std::atan2(nx, wz);
            Px p;
            p.x = x; p.y = y;
            p.u0 = int(std::floor(lonr / (2 * kPi) * m.aw));
            p.row = std::clamp(int((0.5 - lat / kPi) * m.ah), 0, m.ah - 1);
            p.coslat = std::uint16_t(std::cos(lat) * 1024);
            px_.push_back(p);
        }
    cos_.resize(std::size_t(m.aw));
    for (int i = 0; i < m.aw; ++i) cos_[std::size_t(i)] = std::int16_t(std::cos(2 * kPi * i / m.aw) * 1024);
    key_r_ = R; key_lat_ = int(std::lround(v.lat * 4096)); key_aw_ = m.aw; key_ah_ = m.ah;
}

void GlobeRenderer::render(const PlanetMap& m, const GlobeView& v, double sun_lon,
                           const std::uint8_t* fog, std::uint32_t* out, int ow, int oh,
                           int cx, int cy) {
    if (!m.valid()) return;
    if (int(v.radius) != key_r_ || int(std::lround(v.lat * 4096)) != key_lat_ || m.aw != key_aw_ || m.ah != key_ah_)
        rebuild(m, v);
    const int aw = m.aw;
    const int uc = int(std::lround(v.lon / (2 * kPi) * aw));
    const int us = int(std::lround(sun_lon / (2 * kPi) * aw));
    static const std::uint32_t kParch = rgb(188, 162, 112);
    for (const Px& p : px_) {
        const int sx = cx + p.x, sy = cy + p.y;
        if (sx < 0 || sy < 0 || sx >= ow || sy >= oh) continue;
        const int u = (((p.u0 + uc) % aw) + aw) % aw;
        std::uint32_t c = m.albedo[std::size_t(p.row) * aw + u];
        if (fog) {
            const std::uint8_t f = fog[std::size_t(p.row) * aw + u];
            if (f == 0) c = mix(mix(c, rgb(128, 128, 128), 120), kParch, 150);   // old-map haze: relief still readable
            else if (f == 1) c = mix(c, rgb(10, 10, 16), 110);
        }
        // N.L with the sun over the equator at sun_lon (integer from here)
        const int dl = (((u - us) % aw) + aw) % aw;
        const int ndl = int(p.coslat) * cos_[std::size_t(dl)] / 1024;             // -1024..1024
        const int lit = std::clamp(ndl * 2 + 300, 0, 1024);                          // soft terminator
        const int k = 70 + lit * 186 / 1024;                                          // night floor
        const int r = int((c >> 16) & 255) * k / 256, g = int((c >> 8) & 255) * k / 256,
                  b = std::min(255, int(c & 255) * k / 256 + (lit < 300 ? (300 - lit) / 20 : 0));
        out[std::size_t(sy) * ow + sx] = rgb(r, g, b);
    }
    // atmospheric rim glow (additive blue)
    for (const Rim& q : rim_) {
        const int sx = cx + q.x, sy = cy + q.y;
        if (sx < 0 || sy < 0 || sx >= ow || sy >= oh) continue;
        std::uint32_t& d = out[std::size_t(sy) * ow + sx];
        d = mix(d, rgb(120, 180, 255), q.a);
    }
}

bool GlobeRenderer::unproject(const GlobeView& v, double dx, double dy, double& lon, double& lat) {
    const double nx = dx / v.radius, ny = -dy / v.radius;
    const double r2 = nx * nx + ny * ny;
    if (r2 >= 1) return false;
    const double nz = std::sqrt(1 - r2);
    const double cl = std::cos(v.lat), sl = std::sin(v.lat);
    const double wy = ny * cl + nz * sl, wz = -ny * sl + nz * cl;
    lat = std::asin(std::clamp(wy, -1.0, 1.0));
    lon = v.lon + std::atan2(nx, wz);
    lon = std::remainder(lon, 2 * kPi);
    return true;
}

bool GlobeRenderer::project(const GlobeView& v, double lon, double lat, double& dx, double& dy) {
    const double dl = lon - v.lon;
    const double wx = std::cos(lat) * std::sin(dl), wy = std::sin(lat), wz = std::cos(lat) * std::cos(dl);
    const double cl = std::cos(v.lat), sl = std::sin(v.lat);
    // n = Rx(-lat) * w
    const double ny = wy * cl - wz * sl, nz = wy * sl + wz * cl;
    if (nz <= 0) return false;
    dx = wx * v.radius; dy = -ny * v.radius;
    return true;
}

}  // namespace simlab::render
