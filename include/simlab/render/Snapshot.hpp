#pragma once
// Phase 13 — read-only observation layer (SDL-free).
//
// The renderer NEVER touches World. Each frame it asks for a RenderSnapshot:
// a flat copy of exactly what is inside the camera rectangle, with the
// visibility tier of every cell/entity already decided by the FogOfWar. The
// capture takes `const World&` — it cannot change state_hash() by
// construction — and it is O(cells in view + entities in view) thanks to the
// spatial index (viewport culling happens here, not in the draw loop).
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "simlab/Types.hpp"

namespace simlab { class World; }

namespace simlab::render {

enum class Fog : std::uint8_t { Unexplored = 0, Shrouded = 1, Visible = 2 };

struct ViewRect { std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };   // map units, [x0,x1)

struct CellView {
    int gx = 0, gy = 0;                  // global cell
    std::int32_t elevation = 0, moisture = 0, temperature = 0, vegetation = 0,
                 wear = 0, slope = 0;
    std::uint8_t water = 0;
    Fog fog = Fog::Unexplored;
    int fauna = 0, flora = 0;            // visible-species census (ecology lens)
};

struct EntityView {
    EntityId id = kNoEntity;
    Vec2 pos;
    std::string kind;
    std::string species;                 // kind_name when it names a species
    MaterialId material = kNoMaterial;
    Fog fog = Fog::Visible;
    bool alive = true, minded = false, flora = false, fauna = false, structure = false;
    // state the art reads (all World Truth, copied)
    std::int64_t integrity = 1000, fruit = 0, fruit_max = 0, pain = 0, bleed = 0,
                 toxin = 0, morale = -1, routed = 0, lang = 0, radius = 0,
                 barrier = 0, temperature = 200, quantity = 0;
    bool infected = false, leader = false, sacred = false, epic = false;
    std::int64_t grievance = 0, sacred_conv = 0, wealth = 0, launch_range = 0;
    std::int64_t trophic_tier = 0, migration_drive = 0;   // ecology lens
    // equipment
    std::int64_t w_sharp = 0, w_blunt = 0, w_elastic = 0, armor = 0;
    bool w_coated = false;
    MaterialId w_mat = kNoMaterial, armor_mat = kNoMaterial;
    std::string function;                // enclosure: inferred function
    std::string text;                    // inscription glyph/clause text
    std::uint32_t lang_seed = 0;
    std::uint64_t moved_tick = 0;        // last tick its position changed (bobbing)
};

struct LinkView { EntityId from = kNoEntity, to = kNoEntity; Vec2 a, b; int type = 0; };   // 0 defer_to

struct RenderSnapshot {
    std::uint64_t tick = 0;
    int day_phase = 0;                   // 0..23 hour of day
    ViewRect view;
    int cell = 16;
    std::vector<CellView> cells;
    std::vector<EntityView> entities;    // sorted by (pos.y, id): back-to-front
    std::vector<LinkView> links;
    std::size_t culled = 0;              // entities skipped by viewport culling
    // CL-1 shadow-brain observability (Phase 1): populated only when the
    // world runs brain_shadow; -1/0 = no brain data. Read-only stats —
    // never fed back into the simulation.
    std::int64_t brain_slots = -1;
    std::int64_t brain_wm_mae = -1;      // mean |y-yhat| per label channel
    std::int64_t brain_avg_surprise = -1;// mean sigma 0..1000
};

// 3-tier fog of war. Viewer-side state (NOT World Truth): which cells some
// observer has ever seen, which it sees now, and the last seen look of static
// structures so a shrouded region keeps its architecture but hides motion.
//
// Storage is a FLAT byte grid over global cell coords (0=unexplored,
// 1=explored/shrouded, 2=visible) — O(1) cell_fog, cache-friendly bulk
// scans, and no per-cell heap nodes. Bounds default to the terrain plus a
// margin and can be pinned explicitly (planet worlds pass their full
// planet_w x planet_h cell extent); out-of-bounds cells read Unexplored.
class FogOfWar {
public:
    // Avatar: only the possessed individual's own eyes (tribe_ = entity id)
    enum class Mode : std::uint8_t { GodEye = 0, Tribe = 1, Avatar = 2 };
    void set_mode(Mode m, std::int64_t tribe_lang = 0) { mode_ = m; tribe_ = tribe_lang; }
    Mode mode() const { return mode_; }
    std::int64_t tribe() const { return tribe_; }
    // Pin the fog window to explicit GLOBAL cell bounds [x0,x1) x [y0,y1).
    // Call before the first update(); later calls are ignored.
    void set_bounds(int x0, int y0, int x1, int y1) {
        if (bounds_set_) return;
        bounds_set_ = true; bx0_ = x0; by0_ = y0; bx1_ = x1; by1_ = y1;
    }
    // Planet worlds wrap longitude: gx is folded into [0, wrap_x).
    void set_wrap_x(int period) { wrap_x_ = period > 0 ? period : 0; }
    // recompute the Visible set from the observers' sight (minds of the
    // tribe, or every mind); marks newly seen cells Explored.
    void update(const World& w);
    Fog cell_fog(int gx, int gy) const;
    Fog point_fog(const World& w, Vec2 p) const;
    std::size_t explored_count() const { return explored_list_.size(); }
    std::size_t visible_count() const { return visible_list_.size(); }
    // flat-grid access for bulk consumers (globe fog): indices are
    // (gy - oy) * gw + (gx - ox); lists store packed (gx<<32 | gy) cells.
    int grid_w() const { return gw_; }
    int grid_h() const { return gh_; }
    int grid_ox() const { return ox_; }
    int grid_oy() const { return oy_; }
    const std::vector<std::uint64_t>& explored_indices() const { return explored_list_; }
    const std::vector<std::uint64_t>& visible_indices() const { return visible_list_; }
    // bumped on every update that touched the grid — bulk fog textures
    // rebuild only when this changes.
    std::uint64_t generation() const { return generation_; }
    // last seen static structures (enclosures, inscriptions, deposits)
    std::map<EntityId, EntityView> remembered;
private:
    Mode mode_ = Mode::GodEye;
    std::int64_t tribe_ = 0;
    int cell_ = 16;
    bool bounds_set_ = false;
    int bx0_ = 0, by0_ = 0, bx1_ = 0, by1_ = 0;
    int wrap_x_ = 0;                    // longitude wrap period (planet)
    int ox_ = 0, oy_ = 0, gw_ = 0, gh_ = 0;
    std::vector<std::uint8_t> grid_;                    // 0/1/2 per cell
    std::vector<std::uint64_t> explored_list_, visible_list_;
    std::uint64_t generation_ = 0;
    void ensure_grid(const World& w);
};

// Reusable scratch for capture(): caches the O(all entities) wealth scan
// for ~30 ticks instead of paying it every frame.
struct CaptureScratch {
    std::uint64_t epoch = ~0ull;
    std::unordered_map<std::int64_t, std::int64_t> wealth;
};

// Capture what the camera sees. `fog` decides tiers (call fog.update first).
// `scratch` (optional) carries cross-frame caches; pass nullptr for one-shot.
RenderSnapshot capture(const World& w, const ViewRect& view, FogOfWar& fog,
                       CaptureScratch* scratch = nullptr);

}  // namespace simlab::render
