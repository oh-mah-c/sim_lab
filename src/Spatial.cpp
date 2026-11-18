#include "simlab/Spatial.hpp"
#include <algorithm>

namespace simlab {

std::vector<EntityId> SpatialIndex::within(Vec2 p, std::int64_t radius) const {
    std::vector<EntityId> out;
    const std::int64_t r2 = radius * radius;
    // Visit the cells overlapped by the query square, then distance-check.
    const std::int64_t x0 = p.x - static_cast<std::int32_t>(radius);
    const std::int64_t x1 = p.x + static_cast<std::int32_t>(radius);
    const std::int64_t y0 = p.y - static_cast<std::int32_t>(radius);
    const std::int64_t y1 = p.y + static_cast<std::int32_t>(radius);
    auto cell_of = [&](std::int64_t v) {
        return v >= 0 ? v / cell_ : (v - cell_ + 1) / cell_;
    };
    for (std::int64_t cx = cell_of(x0); cx <= cell_of(x1); ++cx)
        for (std::int64_t cy = cell_of(y0); cy <= cell_of(y1); ++cy) {
            const auto it = cells_.find((cx << 32) ^ (cy & 0xFFFFFFFFll));
            if (it == cells_.end()) continue;
            for (const Entry& en : it->second)
                if (dist2(p, en.p) <= r2) out.push_back(en.id);
        }
    std::sort(out.begin(), out.end());   // deterministic result order
    return out;
}

}  // namespace simlab
