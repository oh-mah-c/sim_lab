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
#include <set>
#include <string>
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
};

// 3-tier fog of war. Viewer-side state (NOT World Truth): which cells some
// observer has ever seen, which it sees now, and the last seen look of static
// structures so a shrouded region keeps its architecture but hides motion.
class FogOfWar {
public:
    // Avatar: only the possessed individual's own eyes (tribe_ = entity id)
    enum class Mode : std::uint8_t { GodEye = 0, Tribe = 1, Avatar = 2 };
    void set_mode(Mode m, std::int64_t tribe_lang = 0) { mode_ = m; tribe_ = tribe_lang; }
    Mode mode() const { return mode_; }
    std::int64_t tribe() const { return tribe_; }
    // recompute the Visible set from the observers' sight (minds of the
    // tribe, or every mind); marks newly seen cells Explored.
    void update(const World& w);
    Fog cell_fog(int gx, int gy) const;
    Fog point_fog(const World& w, Vec2 p) const;
    std::size_t explored_count() const { return explored_.size(); }
    std::size_t visible_count() const { return visible_.size(); }
    const std::set<std::pair<int, int>>& explored_cells() const { return explored_; }
    const std::set<std::pair<int, int>>& visible_cells() const { return visible_; }
    // last seen static structures (enclosures, inscriptions, deposits)
    std::map<EntityId, EntityView> remembered;
private:
    Mode mode_ = Mode::GodEye;
    std::int64_t tribe_ = 0;
    int cell_ = 16;
    std::set<std::pair<int, int>> explored_, visible_;
};

// Capture what the camera sees. `fog` decides tiers (call fog.update first).
RenderSnapshot capture(const World& w, const ViewRect& view, FogOfWar& fog);

}  // namespace simlab::render
