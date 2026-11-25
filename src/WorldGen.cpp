#include "simlab/WorldGen.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <utility>

namespace simlab {

namespace { constexpr int kRiverMinArea = 8; }

// ---- deterministic integer noise ---------------------------------------------
// All coordinates are integers; no floating point anywhere in generation.

std::uint32_t WorldGenerator::hash2(std::int32_t x, std::int32_t y, std::uint64_t seed) {
    std::uint64_t h = seed;
    h ^= (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0xBF58476D1CE4E5B9ull);
    h ^= (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0x94D049BB133111EBull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return static_cast<std::uint32_t>(h);
}

namespace {
inline std::int32_t floor_div(std::int32_t a, std::int32_t b) {
    std::int32_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
}

// Value noise in [0,1023] at lattice scale `scale` cells. Floor division keeps
// the lattice continuous across the axes (negative global coordinates).
std::int32_t WorldGenerator::vnoise(std::int32_t x, std::int32_t y, int scale,
                                    std::uint64_t seed) {
    const std::int32_t gx = floor_div(x, scale), gy = floor_div(y, scale);
    const std::int32_t fx = x - gx * scale, fy = y - gy * scale;
    const std::int64_t tx = fx * 1024 / scale, ty = fy * 1024 / scale;
    const std::int64_t sx = tx * tx * (3072 - 2 * tx) / 1048576;
    const std::int64_t sy = ty * ty * (3072 - 2 * ty) / 1048576;
    const std::int32_t v00 = hash2(gx, gy, seed) & 1023;
    const std::int32_t v10 = hash2(gx + 1, gy, seed) & 1023;
    const std::int32_t v01 = hash2(gx, gy + 1, seed) & 1023;
    const std::int32_t v11 = hash2(gx + 1, gy + 1, seed) & 1023;
    const std::int64_t top = v00 + (v10 - v00) * sx / 1024;
    const std::int64_t bot = v01 + (v11 - v01) * sx / 1024;
    return static_cast<std::int32_t>(top + (bot - top) * sy / 1024);
}

// 4-octave fbm in [0,1023].
std::int32_t WorldGenerator::fbm(std::int32_t x, std::int32_t y, std::uint64_t seed) {
    std::int64_t sum = 0, amp = 0;
    for (int o = 0; o < 4; ++o) {
        const int scale = 8 << o;             // 8,16,32,64 cells
        const std::int64_t a = 8 >> o;        // 8,4,2,1
        sum += vnoise(x, y, scale, seed + o * 0x9E3779B9ull) * a;
        amp += a;
    }
    return static_cast<std::int32_t>(sum / amp);
}

// ---- Global Map tier: analytic fields (TODO §2.8.C) ---------------------------
// Every function below is a PURE function of (seed, sea_level, global cell
// coords). No grid, no iteration order, no chunk boundaries — sample any
// coordinate and get the same answer forever.

std::int32_t WorldGenerator::elevation_at(std::uint64_t seed, int gx, int gy) {
    // elevation is the hottest pure function (rivers, moisture, slope, rain
    // shadow all sample it). A direct-mapped memo is invisible to results:
    // each entry is keyed by the full (seed,gx,gy) input.
    struct Slot { std::uint64_t seed; std::int32_t gx, gy, val; bool ok; };
    static thread_local Slot cache[1 << 16];
    const std::uint32_t h = hash2(gx, gy, seed * 0x9E3779B97F4A7C15ull) & 0xFFFF;
    Slot& c = cache[h];
    if (c.ok && c.seed == seed && c.gx == gx && c.gy == gy) return c.val;
    const std::uint64_t ts = worldgen_seed(seed, "terrain");
    const std::int32_t local = fbm(gx, gy, ts);                       // hills (8..64 cells)
    std::int32_t v;
    int PW = 0, PH = 0;
    if (planet_dims(seed, PW, PH)) {
        // several continents (scale ~ width/3), ridged ranges, ocean rim
        // continental mask at planetary scales (3 octaves); local hills
        // only roughen it, so oceans are real basins (~55% of the surface)
        const std::int32_t c1 = vnoise(gx, gy, std::max(12, PW / 3), ts ^ 0xC0171u);
        const std::int32_t c2 = vnoise(gx + 977, gy + 311, std::max(8, PW / 7), ts ^ 0xC0172u);
        const std::int32_t c3 = vnoise(gx + 131, gy + 719, std::max(6, PW / 16), ts ^ 0xC0173u);
        const std::int32_t cont = (c1 * 4 + c2 * 2 + c3) / 7;
        v = 200 + (cont - 540) * 9 / 10 + (local - 512) / 5;
        const std::int32_t rid = std::abs(vnoise(gx + 311, gy + 97, std::max(10, PW / 4),
                                                 ts ^ 0x41D6Eu) - 512);
        if (rid < 36 && v > 200) v += (36 - rid) * 7;                 // mountain spine (on land)
        const int edge = std::min(gx, PW - 1 - gx);
        const int margin = std::max(3, PW / 10);
        if (edge < margin) v -= (margin - edge) * 320 / margin;       // east/west seas
        if (edge < margin / 3) v = 40;          // uniform deep sea at the date line: wrap is seamless
        if (gx < 0 || gx >= PW || gy < 0 || gy >= PH) v = 0;          // beyond the planet: sea
    } else {
        const std::int32_t cont  = vnoise(gx, gy, 192, ts ^ 0xC0171u);    // continents / basins
        v = 80 + (local * 2 + cont) / 3 * 420 / 1023;
    }
    c = {seed, gx, gy, v, true};
    return v;
}

bool WorldGenerator::is_sea(std::uint64_t seed, int gx, int gy, std::int32_t sea_level) {
    return elevation_at(seed, gx, gy) < sea_level;
}

// A lake is a basin: a land cell strictly lower than all four neighbours
// (water pools where drainage has no outlet).
bool WorldGenerator::is_lake(std::uint64_t seed, int gx, int gy, std::int32_t sea_level) {
    if (is_sea(seed, gx, gy, sea_level)) return false;
    const std::int32_t e = elevation_at(seed, gx, gy);
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k)
        if (elevation_at(seed, gx + dx[k], gy + dy[k]) <= e) return false;
    return true;
}

// Steepest-descent trace: from (gx,gy) follow the lowest 4-neighbour while it
// is strictly lower; stops at sea or a lake basin (flag set), a plateau
// minimum, or 64 steps. `out` receives the LAND cells visited (standing-water
// terminus not included).
void WorldGenerator::trace_descent(std::uint64_t seed, int gx, int gy,
                                   std::int32_t sea_level,
                                   std::vector<std::pair<int,int>>& out,
                                   bool* reached_water) {
    out.clear();
    if (reached_water) *reached_water = false;
    int cx = gx, cy = gy;
    for (int step = 0; step < 64; ++step) {
        const std::int32_t e0 = elevation_at(seed, cx, cy);
        std::int32_t best = e0;
        int bx = cx, by = cy;
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            const int nx = cx + dx[k], ny = cy + dy[k];
            if (is_sea(seed, nx, ny, sea_level)) { best = -1; bx = nx; by = ny; break; }
            const std::int32_t e = elevation_at(seed, nx, ny);
            if (e < best) { best = e; bx = nx; by = ny; }
        }
        if (bx == cx && by == cy) {                   // no lower neighbour
            if (is_lake(seed, cx, cy, sea_level)) {   // basin: drainage ends in a lake
                if (!out.empty() && out.back() == std::make_pair(cx, cy)) out.pop_back();
                if (reached_water) *reached_water = true;
            }
            break;
        }
        cx = bx; cy = by;
        if (is_sea(seed, cx, cy, sea_level)) {        // reached standing water
            if (reached_water) *reached_water = true;
            return;
        }
        out.push_back({cx, cy});
    }
}

// Steepest-descent step: the lowest strictly-lower 4-neighbour (a sea
// neighbour always wins). False for sea, lakes and plateau minima.
static bool flow_target(std::uint64_t seed, int cx, int cy, std::int32_t sl,
                        int& tx, int& ty) {
    using G = WorldGenerator;
    if (G::is_sea(seed, cx, cy, sl) || G::is_lake(seed, cx, cy, sl)) return false;
    std::int32_t best = G::elevation_at(seed, cx, cy);
    bool found = false;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
        const int nx = cx + dx[k], ny = cy + dy[k];
        if (G::is_sea(seed, nx, ny, sl)) { tx = nx; ty = ny; return true; }
        const std::int32_t e = G::elevation_at(seed, nx, ny);
        if (e < best) { best = e; tx = nx; ty = ny; found = true; }
    }
    return found;
}

// Flow accumulation: number of cells (self included) draining through
// (gx,gy) within `depth` upstream steps. Bounded recursion, pure function.
static int upstream_area(std::uint64_t seed, int gx, int gy, std::int32_t sl, int depth) {
    int area = 1;
    if (depth == 0) return area;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
        const int nx = gx + dx[k], ny = gy + dy[k];
        int tx = 0, ty = 0;
        if (flow_target(seed, nx, ny, sl, tx, ty) && tx == gx && ty == gy)
            area += upstream_area(seed, nx, ny, sl, depth - 1);
    }
    return area;
}

// A cell is a river cell iff (a) its own descent reaches standing water and
// (b) enough drainage area converges on it (flow accumulation >= threshold) —
// headwaters and divides stay dry; channels form where flow concentrates.
// Pure function of global coords, so chunk order cannot matter.
bool WorldGenerator::is_river(std::uint64_t seed, int gx, int gy,
                              std::int32_t sea_level) {
    if (is_sea(seed, gx, gy, sea_level) || is_lake(seed, gx, gy, sea_level)) return false;
    if (upstream_area(seed, gx, gy, sea_level, 3) < kRiverMinArea) return false;
    std::vector<std::pair<int,int>> own;
    bool reached = false;
    trace_descent(seed, gx, gy, sea_level, own, &reached);
    return reached;
}

// ---- process fields -------------------------------------------------------------

std::int32_t WorldGenerator::latitude_term(int gy) {
    // triangle wave over a 1024-cell "meridian": warm at gy=0 (equator band),
    // cooling toward gy=±512 (poles). Pure function of the global row.
    const int P = 1024;
    const int r = ((gy % P) + P) % P;
    const int tri = r < P - r ? r : P - r;            // 0..512
    return 60 - tri * 240 / 512;                      // +6.0°C .. -18.0°C
}

std::int32_t WorldGenerator::slope_at(std::uint64_t seed, int gx, int gy) {
    const std::int32_t e0 = elevation_at(seed, gx, gy);
    std::int32_t s = 0;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
        const std::int32_t d = e0 - elevation_at(seed, gx + dx[k], gy + dy[k]);
        s = std::max(s, d < 0 ? -d : d);
    }
    return s;
}

// Fault traces: thin contours of a low-frequency noise field — linear
// zones along which hydrothermal fluids ascend (TODO §2.4 step 2).
bool WorldGenerator::is_fault(std::uint64_t seed, int gx, int gy) {
    const std::int32_t n = vnoise(gx, gy, 40, worldgen_seed(seed, "geology") ^ 0xFA017u);
    const std::int32_t d = n > 512 ? n - 512 : 512 - n;
    return d < 7;
}

// Uplift + erosion strip cover fastest on steep ground (TODO §2.4 step 5).
std::int32_t WorldGenerator::erosion_depth_at(std::uint64_t seed, int gx, int gy) {
    return slope_at(seed, gx, gy) / 2;
}

// Orographic rain shadow: wind-borne moisture is lost crossing high ground, so
// cells downwind of a ridge are drier. Samples the elevation upwind.
std::int32_t WorldGenerator::rain_shadow_at(std::uint64_t seed, int gx, int gy) {
    std::int32_t wx = 0, wy = 0;
    wind_at(seed, gx, gy, wx, wy);
    const int sx = wx > 256 ? 1 : (wx < -256 ? -1 : 0);
    const int sy = wy > 256 ? 1 : (wy < -256 ? -1 : 0);
    if (!sx && !sy) return 0;
    const std::int32_t e0 = elevation_at(seed, gx, gy);
    std::int32_t best = 0;
    for (int k : {3, 6}) {
        const std::int32_t up = elevation_at(seed, gx - sx * k, gy - sy * k) - e0;
        if (up > best) best = up;
    }
    return std::min<std::int32_t>(300, std::max<std::int32_t>(0, (best - 60) * 3));
}

std::int32_t WorldGenerator::bedrock_class_at(std::uint64_t seed, int gx, int gy) {
    const std::int32_t geo = fbm(gx + 7331, gy + 9176, worldgen_seed(seed, "geology"));
    return geo < 330 ? 1 : (geo < 700 ? 2 : 3);       // igneous / sedimentary / metamorphic
}

std::int32_t WorldGenerator::moisture_at(std::uint64_t seed, int gx, int gy,
                                         std::int32_t sea_level) {
    const std::uint64_t cseed = worldgen_seed(seed, "climate");
    std::int32_t m = fbm(gx + 977, gy + 331, cseed ^ 0x51EDu) * 750 / 1023;
    m = std::max<std::int32_t>(0, m - rain_shadow_at(seed, gx, gy));
    // bounded sea-proximity bonus: count sea cells in Chebyshev radius 3.
    // Pure function of global coords — replaces grid-wide diffusion so the
    // result is identical no matter which region is generated first.
    int sea_n = 0;
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (is_sea(seed, gx + dx, gy + dy, sea_level)) ++sea_n;
        }
    m = std::min<std::int32_t>(1000, m + sea_n * 60);
    int PW = 0, PH = 0;
    if (planet_dims(seed, PW, PH)) {
        // wet equator (rainforest), dry subtropics, cold-dry poles
        const std::int32_t lat = planet_latitude(seed, gy);
        const std::int32_t belt = lat < 250 ? 260 - lat : (lat < 500 ? -120 : (lat < 800 ? 60 : -200));
        m = std::clamp<std::int32_t>(m + belt, 0, 1000);
    }
    return m;
}

std::int32_t WorldGenerator::temperature_at(std::uint64_t seed, int gx, int gy,
                                            std::int32_t /*sea_level*/) {
    int PW = 0, PH = 0;
    if (planet_dims(seed, PW, PH)) {
        // belt: +34.5 C at the equator -> -20.0 C at the poles; lapse above
        // the lowlands (deci-C per elevation unit, integer)
        const std::int32_t lat = planet_latitude(seed, gy);
        const std::int32_t e = elevation_at(seed, gx, gy);
        return 345 - lat * 545 / 1000 - std::max<std::int32_t>(0, e - 220) * 7 / 10;
    }
    return 400 - elevation_at(seed, gx, gy) + latitude_term(gy);
}

std::uint64_t WorldGenerator::planet_seed(std::uint32_t user_seed, int w, int h) {
    const std::uint64_t W = static_cast<std::uint64_t>(std::clamp(w, 16, 32767));
    const std::uint64_t H = static_cast<std::uint64_t>(std::clamp(h, 16, 32767));
    return (1ull << 63) | (H << 47) | (W << 32) | user_seed;
}
bool WorldGenerator::planet_dims(std::uint64_t seed, int& w, int& h) {
    if (!(seed >> 63)) return false;
    w = static_cast<int>((seed >> 32) & 0x7FFF);
    h = static_cast<int>((seed >> 47) & 0x7FFF);
    return w > 0 && h > 0;
}
std::int32_t WorldGenerator::planet_latitude(std::uint64_t seed, int gy) {
    int PW = 0, PH = 0;
    if (!planet_dims(seed, PW, PH)) return 500;
    const int half = std::max(1, PH / 2);
    return std::clamp<std::int32_t>(std::abs(gy - half) * 1000 / half, 0, 1000);
}

std::uint8_t WorldGenerator::water_at(std::uint64_t seed, int gx, int gy,
                                      std::int32_t sea_level) {
    if (is_sea(seed, gx, gy, sea_level) || is_lake(seed, gx, gy, sea_level)) return 1;
    if (is_river(seed, gx, gy, sea_level)) return 2;
    return 0;
}

std::int32_t WorldGenerator::veg_value(std::int32_t temp, std::int32_t moist) {
    std::int32_t v = (temp >= -50 && temp <= 350) ? moist * 9 / 10 : moist / 5;
    return std::min<std::int32_t>(1000, v);
}

std::int32_t WorldGenerator::vegetation_at(std::uint64_t seed, int gx, int gy,
                                           std::int32_t sea_level) {
    if (water_at(seed, gx, gy, sea_level)) return 0;
    return veg_value(temperature_at(seed, gx, gy, sea_level),
                     moisture_at(seed, gx, gy, sea_level));
}

std::uint8_t WorldGenerator::water_ctx(EvalCtx& ctx, int gx, int gy) {
    const auto key = std::make_pair(gx, gy);
    const auto it = ctx.water.find(key);
    if (it != ctx.water.end()) return it->second;
    const std::uint8_t v = water_at(ctx.seed, gx, gy, ctx.sea_level);
    ctx.water[key] = v;
    return v;
}

void WorldGenerator::wind_at(std::uint64_t seed, int gx, int gy,
                             std::int32_t& wx, std::int32_t& wy) {
    const std::uint64_t aseed = worldgen_seed(seed, "atmosphere");
    wx = (fbm(gx + 53, gy + 91, aseed ^ 0xA11u) - 512) * 2;
    wy = (fbm(gx + 37, gy + 17, aseed ^ 0xB22u) - 512) * 2;
}

std::int32_t WorldGenerator::water_table_at(std::uint64_t seed, int gx, int gy,
                                            std::int32_t sea_level) {
    const std::uint64_t hseed = worldgen_seed(seed, "hydrology");
    const std::int32_t m = moisture_at(seed, gx, gy, sea_level);
    const std::int32_t n = hash2(gx, gy, hseed ^ 0xA0B0u) % 100;
    // permeable rock (sedimentary) drains downward -> deeper table; dense
    // igneous rock holds water near the surface (TODO §2.5 hydrology<-geology)
    const std::int32_t cls = bedrock_class_at(seed, gx, gy);
    const std::int32_t perm = cls == 2 ? 6 : (cls == 1 ? -2 : 0);
    return std::max<std::int32_t>(1, 4 + (1000 - m) * 20 / 1000 + n / 4 + perm);
}

// Placer deposition site (TODO §2.4): placer minerals are not sprinkled by
// condition coincidence — they are the END of a causal chain. A fault
// precipitates gold-bearing ore; uplift+erosion frees it; the stream
// transports it; where the channel loses energy (low slope) the load drops.
// This field evaluates the last two links analytically:
//   1. the cell is a river cell (flowing water exists to carry sediment)
//   2. slope is low enough that transport stalls (deposition, not erosion)
//   3. walking UPSTREAM through the flow field reaches a fault — gold-bearing
//      terrain drains through this reach. Bounded DFS, deterministic order.
bool WorldGenerator::placer_site_at(std::uint64_t seed, int gx, int gy,
                                    std::int32_t sea_level) {
    if (!is_river(seed, gx, gy, sea_level)) return false;
    if (slope_at(seed, gx, gy) > 32) return false;      // flow still too fast
    // Upstream catchment search: neighbours whose flow_target is (cx,cy).
    // seen-set keeps it finite; traversal order is fixed -> deterministic.
    constexpr int kMaxUpstreamCells = 1024;
    std::set<std::pair<int, int>> seen;
    std::vector<std::pair<int, int>> stack;
    stack.push_back({gx, gy});
    seen.insert({gx, gy});
    int visited = 0;
    while (!stack.empty() && visited < kMaxUpstreamCells) {
        const auto [cx, cy] = stack.back();
        stack.pop_back();
        ++visited;
        if (is_fault(seed, cx, cy)) return true;        // gold source upstream
        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
            const int nx = cx + dx[k], ny = cy + dy[k];
            if (!seen.insert({nx, ny}).second) continue;
            int tx = 0, ty = 0;
            if (flow_target(seed, nx, ny, sea_level, tx, ty) &&
                tx == cx && ty == cy)
                stack.push_back({nx, ny});
        }
    }
    return false;
}

// Strata column: soil / subsoil / bedrock keyed on global coords. Rock and soil
// types are DATA: bedrock = a material whose `lithology` class matches the
// parent-rock class at the cell; soil = a material whose `soil_texture`
// matches climate + parent rock. Adding a rock/soil = editing materials.txt.
std::vector<Stratum> WorldGenerator::column_at(const World& w, std::uint64_t seed,
                                               int gx, int gy,
                                               std::int32_t sea_level) {
    const std::uint64_t gseed = worldgen_seed(seed, "geology");
    const std::uint64_t sseed = worldgen_seed(seed, "subsurface");
    const PropId lith_p = SIMLAB_PID(w.props, "lithology");
    const PropId tex_p  = SIMLAB_PID(w.props, "soil_texture");
    auto with_prop = [&](PropId pid, std::int64_t val) {
        std::vector<MaterialId> out;
        if (pid == kNoProp) return out;
        for (const MaterialDef& m : w.materials.defs()) {
            const auto it = m.props.find(pid);
            if (it != m.props.end() && it->second.i == val) out.push_back(m.id);
        }
        return out;                                  // ascending id = deterministic
    };

    const std::int32_t cls = bedrock_class_at(seed, gx, gy);
    MaterialId bedrock = w.materials.id_of("stone");   // generic fallback
    const std::vector<MaterialId> rocks = with_prop(lith_p, cls);
    if (!rocks.empty())
        bedrock = rocks[hash2(gx, gy, gseed ^ 0x1A1u) % rocks.size()];

    // soil texture: climate (moisture/temperature) modulated by parent rock
    const std::int32_t moist = moisture_at(seed, gx, gy, sea_level);
    const std::int32_t temp  = temperature_at(seed, gx, gy, sea_level);
    std::int64_t tex = 2;                              // 1 sand 2 loam 3 clay
    if (cls == 2)      tex = (moist < 400) ? 1 : 2;
    else if (cls == 3) tex = (moist > 550) ? 3 : 2;
    else               tex = (temp < 100) ? 2 : (moist > 500 ? 2 : 1);
    std::vector<MaterialId> soils = with_prop(tex_p, tex);
    if (soils.empty()) soils = with_prop(tex_p, 2);
    const MaterialId soil = soils.empty() ? bedrock : soils[0];

    const int max_depth = 300;
    const std::int32_t soil_t = 1 + hash2(gx, gy, sseed ^ 0x501u) % 2;
    const std::int32_t sub_t  = 3 + hash2(gx, gy, sseed ^ 0x502u) % 4;
    return {
        {soil,      0, soil_t},
        {bedrock,   soil_t, soil_t + sub_t},
        {bedrock,   soil_t + sub_t, max_depth},
    };
}

// ---- pipeline -----------------------------------------------------------------

bool WorldGenerator::generate(World& w, const WorldGenParams& p, std::string* err) {
    if (p.grid_w <= 0 || p.grid_h <= 0 || p.cell_size <= 0) {
        if (err) *err = "worldgen: invalid grid params";
        return false;
    }
    TerrainGrid& t = w.terrain;
    t.w = p.grid_w; t.h = p.grid_h; t.cell = p.cell_size;
    t.ox = p.origin_x; t.oy = p.origin_y;
    const std::size_t n = static_cast<std::size_t>(t.w) * t.h;
    t.elevation.assign(n, 0);
    t.moisture.assign(n, 0);
    t.temperature.assign(n, 0);
    t.water.assign(n, 0);
    t.vegetation.assign(n, 0);
    t.slope.assign(n, 0);
    t.wind_x.assign(n, 0);
    t.wind_y.assign(n, 0);
    t.water_table.assign(n, 0);
    t.traffic_wear.assign(n, 0);
    t.pollution.assign(n, 0);

    SubsurfaceGrid& s = w.subsurface;
    s.w = t.w; s.h = t.h; s.ox = t.ox; s.oy = t.oy; s.max_depth = 300;
    s.cells.assign(n, {});

    GenCtx ctx(w, p);
    for (int gy = p.origin_y; gy < p.origin_y + p.grid_h; ++gy)
        for (int gx = p.origin_x; gx < p.origin_x + p.grid_w; ++gx) {
            fill_terrain_cell(ctx, gx, gy);
            fill_subsurface_cell(ctx, gx, gy);
        }
    if (!p.defer_entities) {
        stage_resources(ctx, p.origin_x, p.origin_y, p.grid_w, p.grid_h);
        stage_ecology(ctx, p.origin_x, p.origin_y, p.grid_w, p.grid_h);
        w.mark_gen_cells(p.origin_x, p.origin_y, p.grid_w, p.grid_h);
    }
    w.world_seed = p.seed;
    w.gen_sea_level = p.sea_level;
    w.generator_version = kGeneratorVersion;
    w.gen_origin_x = p.origin_x;
    w.gen_origin_y = p.origin_y;
    return true;
}

bool WorldGenerator::generate_region(World& w, const WorldGenParams& p,
                                     int gx0, int gy0, int gw, int gh,
                                     std::string* err) {
    if (gw <= 0 || gh <= 0) { if (err) *err = "worldgen: empty region"; return false; }
    if (w.terrain.empty()) { if (err) *err = "worldgen: terrain not initialized"; return false; }
    // A world is ONE configuration: regenerating with a different seed/sea
    // level would silently mix incompatible fields.
    if (w.world_seed != 0 && (w.world_seed != p.seed || w.gen_sea_level != p.sea_level)) {
        if (err) *err = "worldgen: params differ from the world's recorded seed/sea_level";
        return false;
    }
    GenCtx ctx(w, p);
    for (int gy = gy0; gy < gy0 + gh; ++gy)
        for (int gx = gx0; gx < gx0 + gw; ++gx) {
            fill_terrain_cell(ctx, gx, gy);
            fill_subsurface_cell(ctx, gx, gy);
        }
    if (!p.defer_entities) {
        stage_resources(ctx, gx0, gy0, gw, gh);
        stage_ecology(ctx, gx0, gy0, gw, gh);
        w.mark_gen_cells(gx0, gy0, gw, gh);
    }
    w.world_seed = p.seed;
    w.gen_sea_level = p.sea_level;
    w.generator_version = kGeneratorVersion;
    w.gen_origin_x = w.terrain.ox;
    w.gen_origin_y = w.terrain.oy;
    return true;
}

void WorldGenerator::fill_terrain_cell(GenCtx& ctx, int gx, int gy) {
    TerrainGrid& t = ctx.wm.terrain;
    int lx, ly;
    if (!t.local_cell(gx, gy, lx, ly)) return;
    const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
    const std::uint64_t seed = ctx.p.seed;
    const std::int32_t sl = ctx.p.sea_level;
    const std::uint8_t wat = water_ctx(ctx, gx, gy);
    t.elevation[i]    = elevation_at(seed, gx, gy);
    t.moisture[i]     = moisture_at(seed, gx, gy, sl);
    t.temperature[i]  = temperature_at(seed, gx, gy, sl);
    t.water[i]        = wat;
    t.vegetation[i]   = wat ? 0 : veg_value(t.temperature[i], t.moisture[i]);
    t.wind_x[i] = 0; t.wind_y[i] = 0;
    wind_at(seed, gx, gy, t.wind_x[i], t.wind_y[i]);
    t.water_table[i]  = water_table_at(seed, gx, gy, sl);
    t.slope[i] = slope_at(seed, gx, gy);
}

void WorldGenerator::fill_subsurface_cell(GenCtx& ctx, int gx, int gy) {
    SubsurfaceGrid& s = ctx.wm.subsurface;
    int lx, ly;
    // subsurface shares terrain dims/origin
    lx = gx - s.ox; ly = gy - s.oy;
    if (lx < 0 || ly < 0 || lx >= s.w || ly >= s.h) return;
    s.at(lx, ly) = column_at(ctx.wm, ctx.p.seed, gx, gy, ctx.p.sea_level);
}

bool WorldGenerator::field_value(EvalCtx& ctx, const std::string& field, int gx, int gy,
                                 std::int64_t depth, std::int64_t& v) {
    const std::uint64_t seed = ctx.seed;
    const std::int32_t sl = ctx.sea_level;
    if (field == "elevation") v = elevation_at(seed, gx, gy);
    else if (field == "moisture") v = moisture_at(seed, gx, gy, sl);
    else if (field == "temperature") v = temperature_at(seed, gx, gy, sl);
    else if (field == "slope") v = slope_at(seed, gx, gy);
    else if (field == "vegetation") v = vegetation_at(seed, gx, gy, sl);
    else if (field == "water_table") v = water_table_at(seed, gx, gy, sl);
    else if (field == "fault") v = is_fault(seed, gx, gy) ? 1 : 0;
    else if (field == "placer_site") v = placer_site_at(seed, gx, gy, sl) ? 1 : 0;
    else if (field == "near_water") {
        // any water (sea, lake or river) in the 8 surrounding cells, self excluded
        v = 0;
        for (int dy = -1; dy <= 1 && !v; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                if (water_ctx(ctx, gx + dx, gy + dy)) { v = 1; break; }
            }
    }
    else if (field == "lithology") {
        const PropertyDef* pd = ctx.w.props.find_name("lithology");
        v = 0;
        if (!pd) return true;
        const auto col = column_at(ctx.w, seed, gx, gy, sl);
        for (const Stratum& st : col)
            if (depth >= st.top && depth < st.bottom) {
                if (const MaterialDef* md = ctx.w.materials.find(st.material)) {
                    const auto it = md->props.find(pd->id);
                    if (it != md->props.end()) v = it->second.i;
                }
                break;
            }
    }
    else return false;
    return true;
}

bool WorldGenerator::conditions_hold(EvalCtx& ctx, const std::vector<FormationCondition>& cs,
                                     int gx, int gy, std::int64_t depth, std::string* failed) {
    for (const FormationCondition& c : cs) {
        std::int64_t v = 0;
        if (!field_value(ctx, c.field, gx, gy, depth, v) || v < c.lo || v > c.hi) {
            if (failed) *failed = c.field;
            return false;
        }
    }
    return true;
}

void WorldGenerator::stage_resources(GenCtx& ctx, int gx0, int gy0, int gw, int gh) {
    World& w = ctx.wm;
    PropId chance_pid = kNoProp;
    if (const PropertyDef* pd = w.props.find_name("formation_chance")) chance_pid = pd->id;
    const std::uint64_t rseed = worldgen_seed(ctx.p.seed, "resources");
    TerrainGrid& t = w.terrain;
    for (const DepositDef& d : w.deposits.defs()) {
        std::int64_t chance = 25;
        if (chance_pid != kNoProp)
            if (const Value* v = d.props.get(chance_pid)) chance = v->i;
        for (int gy = gy0; gy < gy0 + gh; ++gy)
            for (int gx = gx0; gx < gx0 + gw; ++gx) {
                if (w.gen_cells_.count({gx, gy})) continue;   // entity stage already ran
                // surface deposits need exposed ground: open water (sea/lake)
                // forbids them, but a river bed IS a depositional surface —
                // placer/clay/bar deposits legitimately form in the channel.
                if (d.surface && water_ctx(ctx, gx, gy) == 1) continue;
                if (!conditions_hold(ctx, d.conditions, gx, gy, d.depth)) continue;
                const std::uint32_t h = hash2(gx, gy, rseed ^ (d.id * 0x9E3779B9u));
                if (static_cast<std::int64_t>(h % 1000) >= chance) continue;
                if (!w.claim_spawn(d.id, gx, gy)) continue;
                // uplift + erosion: cover shallower than the eroded depth is gone
                const bool outcrop = !d.surface && erosion_depth_at(ctx.p.seed, gx, gy) >= d.depth;
                w.create_deposit(d.name, t.cell_center(gx, gy), outcrop);
            }
    }
}

// Ecology: species are habitat DATA (data/species.txt). An individual spawns
// where every habitat condition holds and a per-cell hash passes `density`.
// Flora: at most one per cell (first matching species in file order).
void WorldGenerator::stage_ecology(GenCtx& ctx, int gx0, int gy0, int gw, int gh) {
    World& w = ctx.wm;
    const std::uint64_t eseed = worldgen_seed(ctx.p.seed, "ecology");
    TerrainGrid& t = w.terrain;
    int spawned = 0;
    for (int gy = gy0; gy < gy0 + gh && spawned < ctx.p.max_trees; ++gy)
        for (int gx = gx0; gx < gx0 + gw && spawned < ctx.p.max_trees; ++gx) {
            if (w.gen_cells_.count({gx, gy})) continue;   // entity stage already ran
            if (water_ctx(ctx, gx, gy)) continue;
            bool flora_here = false;
            for (const SpeciesDef& sp : w.species.defs()) {
                if (spawned >= ctx.p.max_trees) break;
                if (!sp.fauna && flora_here) continue;
                if (!ctx.p.skip_species.empty() &&
                    std::find(ctx.p.skip_species.begin(), ctx.p.skip_species.end(),
                              sp.name) != ctx.p.skip_species.end()) continue;
                if (!conditions_hold(ctx, sp.conditions, gx, gy, 0)) continue;
                if (hash2(gx, gy, eseed ^ (sp.id * 0x7A77u)) % 1000 >=
                    static_cast<std::uint32_t>(sp.density)) continue;
                if (!w.claim_spawn(World::kSpawnTreeTag - sp.id, gx, gy)) {
                    if (!sp.fauna) flora_here = true;   // already occupied by this species
                    continue;
                }
                // founding genome (Phase 7): species `gene` loci get a
                // deterministic per-cell deviation so populations start with
                // heritable variation. Genome == expressed phenotype at birth;
                // selection acts on later offspring, never on the founder.
                EntityId nid = w.create_entity(sp.name, sp.material,
                                               t.cell_center(gx, gy), sp.props);
                if (nid != kNoEntity && !sp.genes.empty()) {
                    Entity* e = w.find(nid);
                    if (e) {
                        for (std::size_t li = 0; li < sp.genes.size(); ++li) {
                            const PropId pid = sp.genes[li];
                            const PropertyDef* pd = w.props.find(pid);
                            if (!pd) continue;
                            const std::int64_t base = e->prop(w, pid)
                                ? e->prop(w, pid)->i : 0;
                            std::int64_t lo = base - 25, hi = base + 25;
                            if (pd->has_range) { lo = pd->lo; hi = pd->hi; }
                            const std::int64_t span = std::max<std::int64_t>(1, hi - lo);
                            const std::int64_t step = std::max<std::int64_t>(1, span / 20);
                            const std::uint32_t hh =
                                hash2(gx, gy, eseed ^ (sp.id * 0x9E3779u) ^
                                      (static_cast<std::uint32_t>(li) * 0x85EBCA6Bu));
                            const std::int64_t delta =
                                static_cast<std::int64_t>(hh % (2 * step + 1)) - step;
                            const std::int64_t allele =
                                std::clamp<std::int64_t>(base + delta, lo, hi);
                            e->genome.set(pid, allele);
                            e->props.set(pid, Value::integer(allele));
                        }
                    }
                }
                // Phase 10: species with a body plan spawn as anatomical
                // composites — contained part_of children, integrity 1000.
                if (nid != kNoEntity) w.build_anatomy(nid);
                ++spawned;
                if (!sp.fauna) flora_here = true;
            }
        }
}

// ---- validation ---------------------------------------------------------------
// Physical/causal plausibility (TODO §2.6) — re-derives conditions from the
// analytic fields instead of trusting generation, so it also catches
// hand-edited or corrupted worlds. Rules are physical (flow direction, habitat,
// formation conditions), not name lists.

std::vector<std::string> WorldGenerator::validate(const World& w) {
    std::vector<std::string> issues;
    const TerrainGrid& t = w.terrain;
    const bool generated = w.world_seed != 0;
    EvalCtx ctx(w, w.world_seed, w.gen_sea_level);
    auto at = [](Vec2 p) { return std::to_string(p.x) + "," + std::to_string(p.y); };

    // ---- terrain field consistency ----
    if (!t.empty()) {
        const std::size_t n = static_cast<std::size_t>(t.w) * t.h;
        if (t.elevation.size() != n || t.moisture.size() != n || t.water.size() != n ||
            t.vegetation.size() != n)
            issues.push_back("terrain arrays do not match grid size");
        else
            for (int ly = 0; ly < t.h; ++ly)
                for (int lx = 0; lx < t.w; ++lx) {
                    const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
                    const std::string cell = "cell " + std::to_string(lx + t.ox) + "," + std::to_string(ly + t.oy);
                    if (t.moisture[i] < 0 || t.moisture[i] > 1000 || t.vegetation[i] < 0 || t.vegetation[i] > 1000)
                        issues.push_back("moisture/vegetation out of 0..1000 at " + cell);
                    if (t.water[i] && t.vegetation[i] > 0)
                        issues.push_back("vegetation on water at " + cell);
                    if (!generated) continue;
                    if (t.water[i] == 2) {
                        // a river must flow DOWNHILL (or into standing water)
                        bool outlet = false;
                        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                        for (int k = 0; k < 4 && !outlet; ++k) {
                            const int nx = lx + dx[k], ny = ly + dy[k];
                            if (nx < 0 || ny < 0 || nx >= t.w || ny >= t.h) { outlet = true; break; }   // leaves grid
                            const std::size_t j = static_cast<std::size_t>(ny) * t.w + nx;
                            if (t.elevation[j] < t.elevation[i] || t.water[j] == 1) outlet = true;
                        }
                        if (!outlet) issues.push_back("river flows uphill (no lower neighbour) at " + cell);
                    }
                    if (t.water[i] == 1 && t.elevation[i] >= w.gen_sea_level &&
                        !is_lake(w.world_seed, lx + t.ox, ly + t.oy, w.gen_sea_level))
                        issues.push_back("standing water above sea level that is not a basin at " + cell);
                    if (t.water[i] == 0 && t.elevation[i] < w.gen_sea_level)
                        issues.push_back("dry land below sea level at " + cell);
                }
    }

    for (const auto& ep : w.entities) {
        const Entity& e = *ep;
        if (!e.alive) continue;
        const bool on_map = !t.empty() && t.contains(e.pos);
        const int cell = t.empty() ? 16 : t.cell;
        const int gx = TerrainGrid::cell_of(e.pos.x, cell), gy = TerrainGrid::cell_of(e.pos.y, cell);
        if (e.deposit != kNoDeposit) {
            const DepositDef* d = w.deposits.find(e.deposit);
            const char* dn = d ? d->name.c_str() : "?";
            // standing water (sea/lake) forbids surface deposits; a river bed
            // (water==2) is exposed depositional ground — placers live there.
            if (on_map && t.water[t.index(e.pos)] == 1 && d && d->surface)
                issues.push_back("surface deposit '" + std::string(dn) + "' on water at " + at(e.pos));
            if (d && !d->surface && d->depth <= 0)
                issues.push_back("buried deposit '" + std::string(dn) + "' has depth <= 0");
            if (d && !d->surface && on_map && d->depth > 0 && d->depth >= t.elev_at(e.pos))
                issues.push_back("buried deposit '" + std::string(dn) + "' deeper than local elevation");
            if (d && !d->surface && on_map && !w.subsurface.empty() &&
                w.subsurface.material_at(e.pos, t.cell, static_cast<std::int32_t>(d->depth)) == kNoMaterial)
                issues.push_back("buried deposit '" + std::string(dn) + "' below modelled strata");
            if (const PropertyDef* pdr = w.props.find_name("deposit_remaining"))
                if (const Value* rv = e.props.get(pdr->id))
                    if (rv->i <= 0) issues.push_back("depleted deposit '" + std::string(dn) + "'");
            // formation conditions must hold at the cell (geology -> resources)
            std::string failed;
            if (generated && d && !conditions_hold(ctx, d->conditions, gx, gy, d->depth, &failed))
                issues.push_back("deposit '" + std::string(dn) + "' violates formation condition '" +
                                 failed + "' at " + at(e.pos));
            continue;
        }
        const SpeciesDef* sp = w.species.find_name(e.kind_name);
        if (sp) {
            if (on_map && t.is_water(e.pos))
                issues.push_back("species '" + sp->name + "' on water at " + at(e.pos));
            std::string failed;
            if (generated && !conditions_hold(ctx, sp->conditions, gx, gy, 0, &failed))
                issues.push_back("species '" + sp->name + "' outside habitat (" + failed + ") at " + at(e.pos));
            // plants cannot live in extreme desert unless water is adjacent
            std::int64_t m = 0, nw = 0;
            if (generated && !sp->fauna && field_value(ctx, "moisture", gx, gy, 0, m) &&
                field_value(ctx, "near_water", gx, gy, 0, nw) && m < 100 && !nw)
                issues.push_back("flora '" + sp->name + "' in extreme desert at " + at(e.pos));
        } else if (e.kind_name.find("tree") != std::string::npos && on_map && t.is_water(e.pos)) {
            issues.push_back("tree entity on water at " + at(e.pos));
        }
    }
    return issues;
}

// ---- preview ------------------------------------------------------------------

std::string WorldGenerator::preview_ascii(const World& w, int max_cols) {
    const TerrainGrid& t = w.terrain;
    std::ostringstream o;
    o << "world seed=" << w.world_seed << " gen_v=" << w.generator_version
      << " grid=" << t.w << "x" << t.h << " origin=(" << t.ox << "," << t.oy << ")\n";
    if (t.empty()) { o << "(empty terrain)\n"; return o.str(); }
    const int sx = std::max(1, t.w / max_cols), sy = std::max(1, sx);
    for (int ly = 0; ly < t.h; ly += sy) {
        for (int lx = 0; lx < t.w; lx += sx) {
            const std::size_t i = static_cast<std::size_t>(ly) * t.w + lx;
            char c = '.';
            if (t.water[i] == 1) c = '~';
            else if (t.water[i] == 2) c = '=';
            else if (t.elevation[i] >= 400) c = '^';
            else if (t.elevation[i] >= 300) c = '+';
            else if (t.elevation[i] >= 200) c = '-';
            o << c;
        }
        o << '\n';
    }
    return o.str();
}

}  // namespace simlab
