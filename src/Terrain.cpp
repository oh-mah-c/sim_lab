#include "simlab/Terrain.hpp"

namespace simlab {

bool TerrainGrid::near_water(Vec2 p, int radius_cells) const {
    if (empty() || !contains(p)) return false;
    const int cx = cell_of(p.x, cell) - ox, cy = cell_of(p.y, cell) - oy;
    for (int dy = -radius_cells; dy <= radius_cells; ++dy)
        for (int dx = -radius_cells; dx <= radius_cells; ++dx) {
            if (dx == 0 && dy == 0) continue;   // "near" excludes the cell itself
            const int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (water[static_cast<std::size_t>(ny) * w + nx]) return true;
        }
    return false;
}

}  // namespace simlab
