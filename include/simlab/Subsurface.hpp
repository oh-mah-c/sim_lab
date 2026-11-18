#pragma once
#include <cstdint>
#include <vector>
#include "Materials.hpp"
#include "Types.hpp"

namespace simlab {

// ---- Subsurface (TODO §2.3 layer B) -------------------------------------------
// Per-cell column of strata below the surface. Depths are metres below the
// local surface (top=0). This is World Truth structure — agents must dig or
// prospect to learn it.
struct Stratum {
    MaterialId material = kNoMaterial;
    std::int32_t top = 0;      // metres below surface (inclusive)
    std::int32_t bottom = 0;   // metres below surface (exclusive)
    bool operator==(const Stratum& o) const {
        return material == o.material && top == o.top && bottom == o.bottom;
    }
};

struct SubsurfaceGrid {
    int w = 0, h = 0;
    int ox = 0, oy = 0;                  // GLOBAL cell coords of local (0,0)
    int max_depth = 300;                 // metres modelled below surface
    std::vector<std::vector<Stratum>> cells;

    bool empty() const { return w == 0 || h == 0; }
    void resize(int gw, int gh) {
        w = gw; h = gh;
        cells.assign(static_cast<std::size_t>(w) * h, {});
    }
    std::vector<Stratum>& at(int cx, int cy) {
        return cells[static_cast<std::size_t>(cy) * w + cx];
    }
    const std::vector<Stratum>& at(int cx, int cy) const {
        return cells[static_cast<std::size_t>(cy) * w + cx];
    }
    // Stratum containing `depth` metres below surface at world pos `p`.
    const Stratum* stratum_at(Vec2 p, int cell_size, std::int32_t depth) const;
    MaterialId material_at(Vec2 p, int cell_size, std::int32_t depth) const;
};

}  // namespace simlab
