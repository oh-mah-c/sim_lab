// Geometry.cpp — TODO §8.5 parametric physical geometry.
//
// All quantities are integer proxies derived from geom_* props; nothing
// here mutates state. Conventions (documented, deterministic):
//   rod:   axis along local +X, length L, section ~W*W  -> V = L*W*W
//   plate: footprint L*W, thickness T                   -> V = L*W*T
//   blob:  diameter = len                               -> V = len^3 / 8
// Surface proxies use the same units family so ratios stay meaningful.
// Grip point = entity position (actor holds/wields at the entity origin);
// `swing` is the farthest reach of any primitive from that point.

#include "simlab/Geometry.hpp"
#include "simlab/World.hpp"
#include "simlab/Relations.hpp"
#include <algorithm>

namespace simlab {
namespace {

PropId gprop(const World& w, const char* n) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, n);
}

std::int64_t gv(const World& w, const Entity& e, PropId p, std::int64_t fb = 0) {
    return p != kNoProp ? w.prop_int(e, p, fb) : fb;
}

// Deterministic integer trig: sin/cos in permille (-1000..1000).
// 32-step table + linear interpolation — no floats in sim state, and the
// table is exact integer data so results are platform-independent.
constexpr std::int64_t kSinTab[33] = {
    0, 195, 383, 556, 707, 831, 924, 981, 1000, 981, 924, 831, 707, 556,
    383, 195, 0, -195, -383, -556, -707, -831, -924, -981, -1000, -981,
    -924, -831, -707, -556, -383, -195, 0
};
std::int64_t sin_permille(std::int64_t turn) {   // turn: 0..999 of full circle
    turn = ((turn % 1000) + 1000) % 1000;
    const std::int64_t pos = turn * 32 / 1000;    // 0..31
    const std::int64_t frac = (turn * 32 % 1000) * 1000 / 1000; // step fraction*1000
    const std::int64_t a = kSinTab[pos], b = kSinTab[pos + 1];
    return a + (b - a) * frac / 1000;
}
std::int64_t cos_permille(std::int64_t turn) {
    return sin_permille(turn + 250);
}

}  // namespace

std::int64_t prim_volume(GeomType t, std::int64_t len, std::int64_t wid,
                         std::int64_t thk) {
    len = std::max<std::int64_t>(len, 0);
    wid = std::max<std::int64_t>(wid, 0);
    thk = std::max<std::int64_t>(thk, 0);
    switch (t) {
        case GeomType::Rod:   return len * wid * wid;          // L * section
        case GeomType::Plate: return len * wid * std::max<std::int64_t>(thk, 1);
        case GeomType::Blob:  return len * len * len / 8;      // sphere d=len
        default: return 0;
    }
}

std::int64_t prim_surface(GeomType t, std::int64_t len, std::int64_t wid,
                          std::int64_t thk) {
    len = std::max<std::int64_t>(len, 0);
    wid = std::max<std::int64_t>(wid, 0);
    thk = std::max<std::int64_t>(thk, 0);
    switch (t) {
        case GeomType::Rod:   return 3 * len * wid + 2 * wid * wid;   // ~piDL + caps
        case GeomType::Plate: return 2 * (len * wid + len * thk + wid * thk);
        case GeomType::Blob:  return len * len;                      // ~4 pi r^2 /4
        default: return 0;
    }
}

GeomStats geom_stats(const World& w, EntityId id) {
    GeomStats st;
    const Entity* e = w.find(id);
    if (!e || !e->alive) return st;

    const PropId gt = gprop(w, "geom_type");
    const PropId go = gprop(w, "geom_orient");
    const PropId gl = gprop(w, "geom_len");
    const PropId gw = gprop(w, "geom_wid");
    const PropId gk = gprop(w, "geom_thk");
    const PropId massp = gprop(w, "mass");
    const PropId densp = gprop(w, "density");

    // collect primitives: composite -> part_of members at world positions,
    // otherwise the entity's own geom props at its position.
    struct Prim { GeomType t; std::int64_t l, w, k, o, x, y, m; };
    std::vector<Prim> prims;
    bool composite = false;
    for (const Relation& rel : w.relations_of(id, "part_of")) {
        if (rel.b != id) continue;
        const Entity* part = w.find(rel.a);
        if (!part || !part->alive) continue;
        const GeomType pt = static_cast<GeomType>(
            gv(w, *part, gt, 0));
        if (pt == GeomType::None) continue;
        prims.push_back({pt,
                         gv(w, *part, gl, 0), gv(w, *part, gw, 0),
                         gv(w, *part, gk, 0),
                         gv(w, *part, go, 0),
                         part->pos.x, part->pos.y,
                         massp != kNoProp ? w.prop_int(*part, massp, 1000) : 1000});
        composite = true;
    }
    if (!composite) {
        const GeomType t = static_cast<GeomType>(gv(w, *e, gt, 0));
        if (t != GeomType::None)
            prims.push_back({t, gv(w, *e, gl, 0), gv(w, *e, gw, 0),
                             gv(w, *e, gk, 0), gv(w, *e, go, 0),
                             e->pos.x, e->pos.y,
                             massp != kNoProp ? w.prop_int(*e, massp, 1000) : 1000});
    }
    if (prims.empty()) return st;
    st.empty = false;

    // mass-weighted aggregates; grip/origin = entity pos for swing+inertia
    std::int64_t tot_m = 0, com_nx = 0, com_ny = 0;
    for (const Prim& p : prims) {
        // primitive center: rod extends L/2 along its orientation, others
        // centered on position. Orientation is turn-permille, integer trig.
        const std::int64_t c = cos_permille(p.o), sn = sin_permille(p.o);
        const std::int64_t adx = p.t == GeomType::Rod ? p.l / 2 : 0;
        const std::int64_t cx = p.x + adx * c / 1000;
        const std::int64_t cy = p.y + adx * sn / 1000;
        const std::int64_t v = prim_volume(p.t, p.l, p.w, p.k);
        const std::int64_t sf = prim_surface(p.t, p.l, p.w, p.k);
        const std::int64_t m = std::max<std::int64_t>(p.m, 1);
        st.volume += v; st.surface += sf;
        com_nx += cx * m; com_ny += cy * m; tot_m += m;
        // leverage: mass at distance from grip (entity pos) squared
        const std::int64_t dx = cx - e->pos.x, dy = cy - e->pos.y;
        st.inertia += m * (dx * dx + dy * dy) / 1000;
        // swing: farthest extent of this primitive from entity pos, with
        // the primitive's orientation rotating its half-extents
        const std::int64_t ac = c < 0 ? -c : c, as = sn < 0 ? -sn : sn;
        const std::int64_t ex = (ac * p.l / 2 + as * p.w / 2) / 1000
            + (p.t == GeomType::Blob ? p.l / 2 : 0);
        const std::int64_t ey = (as * p.l / 2 + ac * p.w / 2) / 1000
            + (p.t == GeomType::Blob ? p.l / 2 : 0);
        const std::int64_t xreach = (dx >= 0 ? dx : -dx) + ex;
        const std::int64_t yreach = (dy >= 0 ? dy : -dy) + ey;
        st.swing = std::max(st.swing, std::max(xreach, yreach));
    }
    if (tot_m > 0) { st.com_x = com_nx / tot_m; st.com_y = com_ny / tot_m; }
    if (st.volume > 0) {
        // Buoyancy for solid primitives: an object floats iff its material
        // density is below water (1000 kg/m^3 -> 1000000 in prop units).
        // Hollow/air-pocket shapes refine this once containers exist.
        const std::int64_t dens = densp != kNoProp ? w.prop_int(*e, densp, 1000000) : 1000000;
        st.bulk_density = dens;
        st.floats = dens < 1000000;
    }
    return st;
}

// ---- attachment sockets -------------------------------------------------------
// Sockets are DERIVED from the primitive (no extra state): rod ends, plate
// edge midpoints, blob center — positioned by geom_orient. A composite's
// sockets are the union of its live parts' sockets.
namespace {
struct PrimGeom { GeomType t; std::int64_t l, w, k, o; Vec2 pos; };

std::vector<PrimGeom> prims_of(const World& w, EntityId id) {
    std::vector<PrimGeom> out;
    const Entity* e = w.find(id);
    if (!e || !e->alive) return out;
    const PropId gt = gprop(w, "geom_type");
    const PropId gl = gprop(w, "geom_len");
    const PropId gw = gprop(w, "geom_wid");
    const PropId gk = gprop(w, "geom_thk");
    const PropId go = gprop(w, "geom_orient");
    auto read = [&](const Entity* x, Vec2 p) {
        const GeomType t = static_cast<GeomType>(gv(w, *x, gt, 0));
        if (t == GeomType::None) return;
        out.push_back({t, gv(w, *x, gl, 0), gv(w, *x, gw, 0),
                       gv(w, *x, gk, 0), gv(w, *x, go, 0), p});
    };
    bool any = false;
    for (const Relation& rel : w.relations_of(id, "part_of")) {
        if (rel.b != id) continue;
        const Entity* part = w.find(rel.a);
        if (!part || !part->alive) continue;
        const GeomType pt = static_cast<GeomType>(gv(w, *part, gt, 0));
        if (pt == GeomType::None) continue;
        read(part, part->pos); any = true;
    }
    if (!any) read(e, e->pos);
    return out;
}

std::vector<Vec2> sockets_of_prim(const PrimGeom& p) {
    std::vector<Vec2> out;
    const std::int64_t c = cos_permille(p.o), sn = sin_permille(p.o);
    auto rot = [&](std::int64_t x, std::int64_t y) -> Vec2 {
        return {static_cast<std::int32_t>(p.pos.x + (x * c - y * sn) / 1000),
                static_cast<std::int32_t>(p.pos.y + (x * sn + y * c) / 1000)};
    };
    if (p.t == GeomType::Rod) {
        // grip/wrap points along the shaft: ends + thirds + center
        for (std::int64_t f : {-500, -250, 0, 250, 500})
            out.push_back(rot(p.l * f / 1000, 0));
    } else if (p.t == GeomType::Plate) {
        out.push_back(rot(-p.l / 2, 0)); out.push_back(rot( p.l / 2, 0));
        out.push_back(rot(0, -p.w / 2)); out.push_back(rot(0,  p.w / 2));
        out.push_back(p.pos);   // face center
    } else if (p.t == GeomType::Blob) {
        out.push_back(p.pos);
    }
    return out;
}
}  // namespace

std::vector<Vec2> socket_points(const World& w, EntityId id) {
    std::vector<Vec2> out;
    for (const PrimGeom& p : prims_of(w, id)) {
        std::vector<Vec2> ss = sockets_of_prim(p);
        out.insert(out.end(), ss.begin(), ss.end());
    }
    return out;
}

std::int64_t socket_gap(const World& w, EntityId a, EntityId b) {
    const std::vector<Vec2> sa = socket_points(w, a);
    const std::vector<Vec2> sb = socket_points(w, b);
    if (sa.empty() || sb.empty()) return INT64_MAX;
    std::int64_t best = INT64_MAX;
    for (Vec2 x : sa) for (Vec2 y : sb) best = std::min(best, dist2(x, y));
    return best;   // squared distance; caller compares to gap^2
}

}  // namespace simlab
