#pragma once
#include <cstdint>
#include <vector>
#include "Types.hpp"

namespace simlab {

class World;

// ---- Parametric physical geometry (TODO §8.5) --------------------------------
// Shape is STATE, not material identity: `geom_*` props hold an oriented
// bounding primitive per entity (instance override > material default), and
// physical actions (carve/chop/assemble) mutate them at runtime.
//   geom_type: 0 none, 1 rod/capsule, 2 plate/box, 3 blob/sphere
//   geom_len/wid/thk: bounding dims in map units (blob: len = diameter)
// Composite structures have no geom props of their own — geom_stats()
// aggregates the `part_of` members' primitives at their real positions.

enum class GeomType : std::int64_t { None = 0, Rod = 1, Plate = 2, Blob = 3 };

// Derived physical quantities, all integer proxies (deterministic, hashed
// via the underlying props — no separate state). Volume/surface are in
// abstract cubic/map-unit³ proxies; consumers compare like-with-like.
struct GeomStats {
    std::int64_t volume = 0;        // displaced-volume proxy
    std::int64_t surface = 0;       // surface-area proxy
    std::int64_t com_x = 0, com_y = 0;   // mass-weighted center of mass
    std::int64_t inertia = 0;       // sum m_i * d_i^2 from grip point (entity pos)
    std::int64_t swing = 0;         // effective reach: farthest primitive extent
    std::int64_t bulk_density = 0;  // mass*1e6/volume — vs water ~100000 floats
    bool floats = false;            // bulk_density below water threshold
    bool empty = true;
};

// Aggregate geometry of an entity: its own geom_* props, or — for a
// composite — the union of its `part_of` members positioned in the world.
GeomStats geom_stats(const World& w, EntityId id);

// Attachment sockets (§8.5.3): named connection points derived from the
// primitive — rod: 2 end sockets; plate: 4 edge-midpoint sockets;
// blob: 1 center socket. World-space positions; composites return the union
// of their parts' sockets. Empty for entities without geometry.
std::vector<Vec2> socket_points(const World& w, EntityId id);
// Minimum distance between any socket of `a` and any socket of `b`.
// Returns INT64_MAX when either side has no sockets.
std::int64_t socket_gap(const World& w, EntityId a, EntityId b);

// Primitive volume proxy for one bounding primitive (integer).
std::int64_t prim_volume(GeomType t, std::int64_t len, std::int64_t wid,
                         std::int64_t thk);
// Primitive surface proxy.
std::int64_t prim_surface(GeomType t, std::int64_t len, std::int64_t wid,
                          std::int64_t thk);

}  // namespace simlab
