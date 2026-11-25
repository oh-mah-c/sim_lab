#pragma once
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>
#include "Types.hpp"

namespace simlab {

// Uniform-grid spatial index for neighbourhood queries. Stores positions
// itself (id -> pos) so range queries can distance-check; buckets accelerate
// lookup. Deterministic iteration order is NOT required for correctness —
// callers sort results if order matters.
class SpatialIndex {
public:
    explicit SpatialIndex(std::int32_t cell = 1024) : cell_(cell) {}

    void clear() { cells_.clear(); pos_.clear(); }
    void insert(EntityId id, Vec2 p) {
        pos_[id] = p;
        cells_[key_of(p)].push_back({id, p});
    }
    void erase(EntityId id) {
        const auto pi = pos_.find(id);
        if (pi == pos_.end()) return;
        const auto ci = cells_.find(key_of(pi->second));
        if (ci != cells_.end()) {
            auto& v = ci->second;
            for (std::size_t i = 0; i < v.size(); ++i)
                if (v[i].id == id) { v[i] = v.back(); v.pop_back(); break; }
        }
        pos_.erase(pi);
    }
    void move(EntityId id, Vec2 to) {
        const auto pi = pos_.find(id);
        if (pi == pos_.end()) { insert(id, to); return; }
        if (pi->second == to) return;
        erase(id);
        insert(id, to);
    }
    void set_pos(EntityId id, Vec2 p) { move(id, p); }

    // Entity ids within `radius` map units of p (Euclidean, integer compare).
    std::vector<EntityId> within(Vec2 p, std::int64_t radius) const;
    // Allocation-free variant: calls fn(id) for every entity within radius.
    // Iteration order is unspecified — only for order-free reductions.
    template <typename F>
    void for_each_within(Vec2 p, std::int64_t radius, F&& fn) const {
        const std::int64_t r2 = radius * radius;
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
                    if (dist2(p, en.p) <= r2) fn(en.id);
            }
    }
    bool contains(EntityId id) const { return pos_.count(id) != 0; }
    Vec2 pos_of(EntityId id) const {
        const auto it = pos_.find(id);
        return it == pos_.end() ? Vec2{} : it->second;
    }

private:
    std::int32_t cell_;
    // bucket entries carry the position so range queries never touch pos_
    struct Entry { EntityId id; Vec2 p; };
    std::unordered_map<std::int64_t, std::vector<Entry>> cells_;
    std::map<EntityId, Vec2> pos_;
    std::int64_t key_of(Vec2 p) const {
        const std::int64_t cx = p.x >= 0 ? p.x / cell_ : (p.x - cell_ + 1) / cell_;
        const std::int64_t cy = p.y >= 0 ? p.y / cell_ : (p.y - cell_ + 1) / cell_;
        return (cx << 32) ^ (cy & 0xFFFFFFFFll);
    }
};

}  // namespace simlab
