#pragma once
// Planetary globe view (SDL-free, viewer side, read-only).
//
// The planet is analytic: WorldGenerator's pure field functions of the
// planet-coded worldgen seed define every cell of a 2:1 equirectangular
// planet without materializing it. PlanetMap samples those fields ONCE into a
// small albedo texture (<= 2048 x 1024 ARGB, 8 MB worst case) plus per-chunk
// colour tiles on demand; GlobeRenderer turns it into a lit sphere on the CPU
// with a per-pixel inverse-projection table (rebuilt only when the radius or
// the tilt changes — spinning east/west is a column offset).
// Floats live here only (display); nothing feeds back into the World.
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace simlab::render {

// Terrain colour of one analytic planet cell (ARGB, opaque).
std::uint32_t planet_cell_color(std::uint64_t gen_seed, int gx, int gy, std::int32_t sea_level = 200);

struct PlanetMap {
    std::uint64_t gen_seed = 0;
    int pw = 0, ph = 0;                   // planet size in cells (2:1)
    int aw = 0, ah = 0;                   // albedo map size
    std::vector<std::uint32_t> albedo;    // aw x ah, row 0 = north pole
    bool valid() const { return aw > 0 && !albedo.empty(); }
    // sample the planet (aw <= 2048, ah = aw / 2)
    void build(std::uint64_t gen_seed, int pw, int ph, int aw = 1024);
    // planet cell -> albedo texel
    int col_of(int gx) const { return int(((long long)gx % pw + pw) % pw * aw / pw); }
    int row_of(int gy) const { return gy <= 0 ? 0 : gy >= ph ? ah - 1 : int((long long)gy * ah / ph); }
};

// 32x32-cell colour tiles of the far planet for the 2.5D surface view,
// generated on demand from the analytic fields, LRU-bounded.
class PlanetTiles {
public:
    static constexpr int kChunk = 32;
    void reset(std::uint64_t gen_seed, int pw, int ph, std::size_t max_tiles = 384);
    // tile containing chunk (cx, cy); cx wraps east-west. nullptr off-planet.
    const std::vector<std::uint32_t>* tile(int cx, int cy);
    std::size_t size() const { return map_.size(); }
    std::uint64_t key(int cx, int cy) const { return (std::uint64_t(std::uint32_t(cy)) << 32) | std::uint32_t(cx); }
private:
    std::uint64_t seed_ = 0;
    int pw_ = 0, ph_ = 0;
    std::size_t max_ = 384;
    std::list<std::uint64_t> lru_;
    struct Slot { std::vector<std::uint32_t> px; std::list<std::uint64_t>::iterator it; };
    std::unordered_map<std::uint64_t, Slot> map_;
};

struct GlobeView {
    double lon = 0, lat = 0;   // radians: the point under the screen centre
    double radius = 200;       // pixels
};

class GlobeRenderer {
public:
    // Render the globe centred at (cx, cy) into `out` (ow x oh ARGB). Pixels
    // outside the sphere + rim are left untouched (caller clears/stars).
    // sun_lon: sub-solar longitude (radians). fog: aw x ah bytes (0 unexplored,
    // 1 shrouded, 2 visible) or nullptr (God Eye).
    void render(const PlanetMap& m, const GlobeView& v, double sun_lon,
                const std::uint8_t* fog, std::uint32_t* out, int ow, int oh,
                int cx, int cy);
    // screen offset from the globe centre -> lon/lat (false off the sphere)
    static bool unproject(const GlobeView& v, double dx, double dy, double& lon, double& lat);
    // lon/lat -> screen offset from the centre (false on the far side)
    static bool project(const GlobeView& v, double lon, double lat, double& dx, double& dy);
    std::size_t table_pixels() const { return px_.size(); }
private:
    struct Px { std::int32_t x, y; std::int32_t u0; std::int32_t row; std::uint16_t coslat; };
    struct Rim { std::int32_t x, y; std::uint8_t a; };
    std::vector<Px> px_;
    std::vector<Rim> rim_;
    std::vector<std::int16_t> cos_;      // aw entries, cos(2*pi*i/aw) * 1024
    int key_r_ = -1, key_lat_ = 1 << 30, key_aw_ = 0, key_ah_ = 0;
    void rebuild(const PlanetMap& m, const GlobeView& v);
};

}  // namespace simlab::render
