#pragma once
#include <cstdint>
#include <vector>
#include "Types.hpp"

namespace simlab {

// ---- Terrain grid -----------------------------------------------------------
// The world's continuous fields, stored per-cell as integers (deterministic).
// Units:
//   elevation   metres, 0..~500
//   moisture    permille 0..1000
//   temperature deci-°C (300 = 30.0 °C)
//   water       0=land 1=sea/lake 2=river
//   vegetation  permille 0..1000 — plant growth potential
//   slope       metres of max elevation difference to 4-neighbours
struct TerrainGrid {
    int w = 0, h = 0;          // cells
    int cell = 16;             // map units per cell
    int ox = 0, oy = 0;        // GLOBAL cell coords of local cell (0,0)

    std::vector<std::int32_t> elevation;
    std::vector<std::int32_t> moisture;
    std::vector<std::int32_t> temperature;
    std::vector<std::uint8_t> water;        // 0=land, 1=sea/lake, 2=river
    std::vector<std::int32_t> vegetation;
    std::vector<std::int32_t> slope;
    std::vector<std::int32_t> wind_x;       // permille-ish wind vector (-1024..1024)
    std::vector<std::int32_t> wind_y;
    std::vector<std::int32_t> water_table;  // metres below surface to groundwater
    std::vector<std::int32_t> traffic_wear; // 0..1000 — footfall compaction (trails)
    std::vector<std::int32_t> pollution;    // 0..1000 — soot/filth; suppresses flora regrowth
    // Structural/conduction fields (save v28), derived from entities resting
    // on the cell — refreshed lazily over dirty cells only:
    std::vector<std::int32_t> support_strength;  // 0..1000 load-bearing capacity
    std::vector<std::int32_t> overburden_load;   // 0..1000 roof stress (240 = covered)
    std::vector<std::int32_t> conductivity;      // 0..1000 max material conductivity >200

    bool empty() const { return w == 0 || h == 0; }
    // Position -> global cell (floor division, safe for negative coords).
    static int cell_of(std::int32_t v, int cell) {
        return v >= 0 ? v / cell : (v - cell + 1) / cell;
    }
    // Global cell coords covered by this grid.
    bool contains(Vec2 p) const {
        const int gx = cell_of(p.x, cell) - ox, gy = cell_of(p.y, cell) - oy;
        return gx >= 0 && gy >= 0 && gx < w && gy < h;
    }
    std::size_t index(Vec2 p) const {
        const int gx = cell_of(p.x, cell) - ox, gy = cell_of(p.y, cell) - oy;
        return static_cast<std::size_t>(gy) * w + gx;
    }
    // Map-unit position of a GLOBAL cell's centre.
    Vec2 cell_center(int gx, int gy) const {
        return {gx * cell + cell / 2, gy * cell + cell / 2};
    }
    // Local cell indices for a global cell coord (false if outside grid).
    bool local_cell(int gx, int gy, int& lx, int& ly) const {
        lx = gx - ox; ly = gy - oy;
        return lx >= 0 && ly >= 0 && lx < w && ly < h;
    }

    std::int32_t elev_at(Vec2 p) const { return elevation[index(p)]; }
    std::int32_t moist_at(Vec2 p) const { return moisture[index(p)]; }
    std::int32_t temp_at(Vec2 p) const { return temperature[index(p)]; }
    bool is_water(Vec2 p) const { return water[index(p)] != 0; }
    std::int32_t veg_at(Vec2 p) const { return vegetation[index(p)]; }
    std::int32_t slope_at(Vec2 p) const { return slope[index(p)]; }
    std::int32_t water_table_at(Vec2 p) const { return water_table[index(p)]; }
    std::int32_t wear_at(Vec2 p) const {
        return traffic_wear.empty() ? 0 : traffic_wear[index(p)];
    }
    std::int32_t pollution_at(Vec2 p) const {
        return pollution.empty() ? 0 : pollution[index(p)];
    }
    void add_pollution(Vec2 p, std::int32_t delta) {
        if (pollution.empty() || !contains(p)) return;
        std::int32_t& v = pollution[index(p)];
        v = static_cast<std::int32_t>(
            std::min<std::int64_t>(1000, std::max<std::int64_t>(0,
                static_cast<std::int64_t>(v) + delta)));
    }
    void add_wear(Vec2 p, std::int32_t delta) {
        if (traffic_wear.empty() || !contains(p)) return;
        std::int32_t& w = traffic_wear[index(p)];
        w = static_cast<std::int32_t>(
            std::min<std::int64_t>(1000, std::max<std::int64_t>(0,
                static_cast<std::int64_t>(w) + delta)));
    }
    // Local moisture pulse (rain/succulence): same direct-field convention
    // as add_wear — deterministic, already covered by state_hash/save.
    void add_moisture(Vec2 p, std::int32_t delta) {
        if (moisture.empty() || !contains(p)) return;
        std::int32_t& m = moisture[index(p)];
        m = static_cast<std::int32_t>(
            std::min<std::int64_t>(1000, std::max<std::int64_t>(0,
                static_cast<std::int64_t>(m) + delta)));
    }
    bool near_water(Vec2 p, int radius_cells = 1) const;
};

}  // namespace simlab
