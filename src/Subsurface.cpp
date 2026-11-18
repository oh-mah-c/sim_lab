#include "simlab/Subsurface.hpp"
#include "simlab/Terrain.hpp"   // TerrainGrid::cell_of

namespace simlab {

const Stratum* SubsurfaceGrid::stratum_at(Vec2 p, int cell_size,
                                          std::int32_t depth) const {
    if (empty()) return nullptr;
    const int gcx = TerrainGrid::cell_of(p.x, cell_size);
    const int gcy = TerrainGrid::cell_of(p.y, cell_size);
    const int cx = gcx - ox, cy = gcy - oy;
    if (cx < 0 || cy < 0 || cx >= w || cy >= h) return nullptr;
    const auto& col = at(cx, cy);
    for (const Stratum& s : col)
        if (depth >= s.top && depth < s.bottom) return &s;
    return col.empty() ? nullptr : &col.back();
}

MaterialId SubsurfaceGrid::material_at(Vec2 p, int cell_size,
                                       std::int32_t depth) const {
    const Stratum* s = stratum_at(p, cell_size, depth);
    return s ? s->material : kNoMaterial;
}

}  // namespace simlab
