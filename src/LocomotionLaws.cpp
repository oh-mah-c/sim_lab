#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "LawUtil.hpp"
#include "simlab/Cognition.hpp"
#include <string>

namespace simlab {

namespace {

// Deterministic integer square root (Newton iteration on integers) —
// locomotion must not depend on floating point.
std::int64_t isqrt(std::int64_t v) {
    if (v <= 0) return 0;
    std::int64_t x = v;
    std::int64_t y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

}  // namespace

// ---- maritime physics helpers (shared with the driver's risk sense) ---------
namespace lawutil {

namespace {
std::int64_t mat_density(const World& w, MaterialId m) {
    const MaterialDef* d = w.materials.find(m);
    const PropId dp = lawutil::pid(w, "density");
    if (!d || dp == kNoProp) return 0;
    const auto it = d->props.find(dp);
    return it == d->props.end() ? 0 : it->second.i;
}
std::int64_t water_density(const World& w) {
    const MaterialDef* wd = w.materials.find_name("water");
    const std::int64_t d = wd ? mat_density(w, wd->id) : 0;
    return d > 0 ? d : 1000000;
}
std::int64_t entity_density(const World& w, const Entity& e) {
    const PropId dp = lawutil::pid(w, "density");
    if (dp != kNoProp) if (const Value* v = w.prop_of(e, dp)) return v->i;
    return mat_density(w, e.material);
}
}  // namespace

std::int64_t water_depth(const World& w, Vec2 p) {
    const TerrainGrid& t = w.terrain;
    if (t.empty() || !t.contains(p)) return 0;
    const std::size_t i = t.index(p);
    const std::uint8_t wt = t.water[i];
    if (wt == 0) return 0;
    if (wt == 2) return 12;                                  // river: wadeable
    const std::int64_t below = static_cast<std::int64_t>(w.gen_sea_level) - t.elevation[i];
    if (below <= 0) return 40;                               // basin lake above sea level
    return std::max<std::int64_t>(20, below);
}

std::int64_t wave_at(const World& w, Vec2 p) {
    const std::int64_t d = water_depth(w, p);
    if (d == 0) return 0;
    const TerrainGrid& t = w.terrain;
    const std::size_t i = t.index(p);
    const std::int64_t wx = t.wind_x.size() > i ? t.wind_x[i] : 0, wy = t.wind_y.size() > i ? t.wind_y[i] : 0;
    const std::int64_t wind = (wx < 0 ? -wx : wx) + (wy < 0 ? -wy : wy);
    return std::clamp<std::int64_t>(d * 3 + wind / 4, 0, 1000);
}

EntityId craft_of(const World& w, EntityId actor) {
    for (const Relation& r : w.relations_of(actor, "aboard"))
        if (r.a == actor) if (const Entity* c = w.find(r.b); c && c->alive) return r.b;
    return kNoEntity;
}

std::int64_t sinking_load(const World& w, const Entity& e) {
    const std::int64_t D = water_density(w);
    const PropId mp = lawutil::pid(w, "mass");
    auto excess = [&](const Entity& x) {
        const std::int64_t m = mp != kNoProp ? w.prop_int(x, mp, 0) : 0;
        const std::int64_t d = entity_density(w, x);
        return d > D ? m * (d - D) / D : 0;
    };
    std::int64_t load = excess(e) + 1000;                    // head above water
    for (const char* rel : {"wielding", "wearing"})
        for (const Relation& r : w.relations_of(e.id, rel))
            if (r.a == e.id) if (const Entity* g = w.find(r.b); g && g->alive) load += excess(*g) + 
                (mp != kNoProp ? w.prop_int(*g, mp, 0) / 10 : 0);   // bulk drags even when it floats
    return load;
}

std::int64_t swim_stroke_cost(const World& w, const Entity& e, Vec2 p) {
    const std::int64_t depth = water_depth(w, p), wave = wave_at(w, p);
    const PropId sk = lawutil::pid(w, "swim_skill"), mp = lawutil::pid(w, "mass"),
               nbt = lawutil::pid(w, "normal_body_temp");
    const std::int64_t skill = sk != kNoProp ? w.prop_int(e, sk, 0) : 0;
    const std::int64_t body = std::max<std::int64_t>(1, mp != kNoProp ? w.prop_int(e, mp, 40000) : 40000);
    // Water temperature stress: |local temp - set-point| makes strokes dearer
    // in both directions (cold water stiffens, hot water exhausts). Only
    // entities carrying a thermal phenotype (normal_body_temp) feel it —
    // poikilotherms and abstract actors pay nothing extra.
    std::int64_t tempterm = 0;
    if (nbt != kNoProp && !w.terrain.empty() && w.terrain.contains(p)) {
        const std::int64_t normal = w.prop_int(e, nbt, 0);
        if (normal > 0) {
            const std::int64_t wt = w.terrain.temp_at(p);
            const std::int64_t d = wt > normal ? wt - normal : normal - wt;
            tempterm = std::min<std::int64_t>(d, 600) / 20;
        }
    }
    const std::int64_t base = 15 + depth / 10 + wave / 50 + tempterm;
    const std::int64_t loadf = 1000 + (sinking_load(w, e) - 1000) * 1000 / body;   // 1000 = bare body
    const std::int64_t cost =
        std::max<std::int64_t>(1, base * loadf / 1000 * 1000 / (500 + skill));
    // conditioning: a trained body strokes cheaper (same factor as effort)
    return std::max<std::int64_t>(1,
        cost * effort_permille(w, e) / 1000);
}

std::int64_t craft_capacity_left(const World& w, EntityId craft) {
    const Entity* c = w.find(craft);
    const PropId fc = lawutil::pid(w, "float_capacity");
    if (!c || fc == kNoProp) return 0;
    std::int64_t left = w.prop_int(*c, fc, 0);
    for (const Relation& r : w.relations_of(craft, "aboard"))
        if (r.b == craft) if (const Entity* p = w.find(r.a); p && p->alive) left -= sinking_load(w, *p);
    return left;
}

}  // namespace lawutil

LawId LocomotionModel::id() const { return 9; }

bool LocomotionModel::supports(const World& w, const InteractionContext& ctx) const {
    if (ctx.parameters.action == "board" || ctx.parameters.action == "disembark")
        return (w.ruleset & World::kRulePrimordial) != 0;
    return ctx.parameters.action == "move" && ctx.parameters.has_destination;
}

// board: step onto a floating craft (Primordial ruleset). The craft must be
// in reach, touch water, still afloat, and hold up the newcomer's sinking
// load on top of everyone already aboard (float_capacity is derived at
// assembly from the parts' real densities — see CompositionLaws).
static PredictionResult predict_board(const World& w, const InteractionContext& ctx, LawId law, const std::string& lname) {
    PredictionResult r;
    r.law = law; r.law_name = lname;
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (!lawutil::conscious_enough(w, *actor, r)) return r;
    if (ctx.parameters.action == "disembark") {
        const EntityId c = lawutil::craft_of(w, ctx.actor);
        if (c == kNoEntity) { r.failure = "not aboard anything"; return r; }
        r.rel_changes.push_back({ctx.actor, "aboard", c, false});
        r.possible = true; r.ticks = 1;
        r.notes.push_back("disembark");
        return r;
    }
    if (ctx.targets.empty()) { r.failure = "board what?"; return r; }
    const EntityId cid = ctx.targets.front();
    const Entity* c = w.find(cid);
    if (!c || !c->alive || cid == ctx.actor) { r.failure = "no craft"; return r; }
    if (lawutil::craft_of(w, ctx.actor) != kNoEntity) { r.failure = "already aboard"; return r; }
    const PropId reach_p = lawutil::pid(w, "reach");
    const std::int64_t reach = reach_p != kNoProp ? w.prop_int(*actor, reach_p, 64) : 64;
    if (dist2(actor->pos, c->pos) > reach * reach) { r.failure = "craft out of reach"; return r; }
    const PropId fc = lawutil::pid(w, "float_capacity");
    if (fc == kNoProp || w.prop_int(*c, fc, 0) <= 0) { r.failure = "it does not float"; return r; }
    // water close enough to haul the craft to (2 x reach): on land, boarding
    // means taking hold of it — it is dragged along until it floats
    bool wet = false;
    const int cell = std::max(1, w.terrain.cell);
    const int rc = static_cast<int>(2 * reach / cell) + 1;
    for (int dy = -rc; dy <= rc && !wet; ++dy)
        for (int dx = -rc; dx <= rc; ++dx)
            if (lawutil::water_depth(w, {c->pos.x + dx * cell, c->pos.y + dy * cell}) > 0) { wet = true; break; }
    if (!wet) { r.failure = "no water within hauling distance"; return r; }
    const std::int64_t need = lawutil::sinking_load(w, *actor);
    const std::int64_t left = lawutil::craft_capacity_left(w, cid);
    lawutil::add_cond(r, "float_capacity", left, need);
    if (left < need) { r.failure = "craft would sink under this load"; return r; }
    r.rel_changes.push_back({ctx.actor, "aboard", cid, true});
    r.possible = true; r.ticks = 1;
    r.notes.push_back("board craft " + std::to_string(cid) + " (lift left " + std::to_string(left - need) + ")");
    return r;
}

PredictionResult LocomotionModel::predict(const World& w,
                                          const InteractionContext& ctx) const {
    if (ctx.parameters.action == "board" || ctx.parameters.action == "disembark")
        return predict_board(w, ctx, id(), name());
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropId speed_p = lawutil::pid(w, "speed");
    const PropId energy_p = lawutil::pid(w, "energy");
    std::int64_t speed = speed_p != kNoProp
        ? w.prop_int(*actor, speed_p, 0) : 0;
    // Phase 10: broken/severed legs slow locomotion proportionally.
    speed = speed * w.anatomy_function(actor->id, "leg") / 1000;
    if (!lawutil::conscious_enough(w, *actor, r)) return r;
    lawutil::add_cond(r, "speed", speed, 1);
    if (speed <= 0) { r.failure = "actor cannot move (no speed)"; return r; }

    const Vec2 from = actor->pos, to = ctx.parameters.destination;
    const std::int64_t dx = static_cast<std::int64_t>(to.x) - from.x;
    const std::int64_t dy = static_cast<std::int64_t>(to.y) - from.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (d2 == 0) {
        r.possible = true;
        r.notes.push_back("already at destination");
        return r;
    }
    const std::int64_t dist = isqrt(d2);
    // speed unit: milli-map-units per tick (data declares e.g. 6000 = 6/tick).
    const std::int64_t ticks = ctx.parameters.ticks > 0 ? ctx.parameters.ticks : 1;
    std::int64_t step = speed * ticks / 1000;
    if (step <= 0) step = 1;
    const bool arrive = step >= dist;
    const std::int64_t covered = arrive ? dist : step;

    // Integer direction: scale the displacement by covered/dist. All math is
    // integer — deterministic across platforms, no floats.
    Vec2 np = from;
    if (arrive) np = to;
    else {
        np.x = from.x + static_cast<std::int32_t>(dx * covered / dist);
        np.y = from.y + static_cast<std::int32_t>(dy * covered / dist);
    }

    // Terrain: water is a hard boundary for land-bound actors — but a
    // straight step that lands in water triggers LOCAL STEERING: try
    // headings rotated ±45/±90/±135 degrees (O(1), deterministic integer
    // math; the preferred side is fixed per actor id so NPCs consistently
    // skirt shorelines in one direction instead of oscillating in place).
    auto water_at = [&](Vec2 p) {
        return w.terrain.contains(p) && w.terrain.is_water(p);
    };
    // ---- Primordial maritime physics: sail, wade, swim, drown -------------
    if ((w.ruleset & World::kRulePrimordial) && !w.terrain.empty()) {
        const EntityId craft = lawutil::craft_of(w, ctx.actor);
        const bool wet_next = water_at(np);
        const bool afloat_now = water_at(from);
        if (craft != kNoEntity && !wet_next && afloat_now) {
            // stepping ashore leaves the craft beached where it floats
            r.rel_changes.push_back({ctx.actor, "aboard", craft, false});
            r.notes.push_back("disembark ashore");
        } else if (craft != kNoEntity && !wet_next) {
            // on land with a craft in hand: haul it along (heavy going)
            r.moves.push_back({craft, np});
            r.notes.push_back("haul craft");
        } else if (craft != kNoEntity) {
            // sailing: the craft carries everyone aboard; swell batters it
            const Entity* c = w.find(craft);
            r.moves.push_back({craft, np});
            for (const Relation& rel : w.relations_of(craft, "aboard"))
                if (rel.b == craft && rel.a != ctx.actor)
                    if (const Entity* pa = w.find(rel.a); pa && pa->alive) r.moves.push_back({rel.a, np});
            const PropId wr_p = lawutil::pid(w, "wave_rating"), integ_p = lawutil::pid(w, "integrity");
            const std::int64_t wave = lawutil::wave_at(w, np);
            const std::int64_t rating = wr_p != kNoProp ? w.prop_int(*c, wr_p, 0) : 0;
            lawutil::add_cond(r, "wave_rating", rating, wave);
            if (wave > rating && integ_p != kNoProp) {
                const std::int64_t i0 = w.prop_int(*c, integ_p, 1000);
                const std::int64_t i1 = std::max<std::int64_t>(0, i0 - ((wave - rating) / 4 + 1));
                lawutil::set_change(r, craft, integ_p, i0, i1);
                if (i1 == 0) {                                 // capsized: everyone in the water
                    for (const Relation& rel : w.relations_of(craft, "aboard"))
                        if (rel.b == craft) r.rel_changes.push_back({rel.a, "aboard", craft, false});
                    r.notes.push_back("capsized in swell " + std::to_string(wave));
                } else r.notes.push_back("craft strained by swell " + std::to_string(wave));
            } else r.notes.push_back("sail");
            if (!lawutil::charge_effort(w, *actor, energy_p, covered * 10, r)) return r;   // paddling
            r.moves.push_back({ctx.actor, np});
            r.possible = true; r.ticks = 1;
            return r;
        } else if (wet_next && lawutil::water_depth(w, np) > lawutil::kWadeDepth &&
                   ctx.parameters.note == "swim") {
            // swimming: fatigue per stroke; exhausted in deep water = drowning
            const PropId fat_p = lawutil::pid(w, "fatigue"), integ_p = lawutil::pid(w, "integrity"),
                         sk_p = lawutil::pid(w, "swim_skill");
            const std::int64_t cost = lawutil::swim_stroke_cost(w, *actor, np);
            const std::int64_t f0 = fat_p != kNoProp ? w.prop_int(*actor, fat_p, 0) : 0;
            const std::int64_t f1 = std::min<std::int64_t>(1000, f0 + cost);
            lawutil::add_cond(r, "fatigue_headroom", 1000 - f0, cost);
            if (fat_p != kNoProp) lawutil::set_change(r, ctx.actor, fat_p, f0, f1);
            // conditioning: each stroke is sustained work — accrue exertion
            // strain proportional to the fatigue it costs (rest absorbs it).
            {
                const PropId ex_p = lawutil::pid(w, "exertion");
                if (ex_p != kNoProp) {
                    const bool had_ex = w.prop_of(*actor, ex_p) != nullptr;
                    const std::int64_t ex0 = w.prop_int(*actor, ex_p, 0);
                    const std::int64_t ex1 = std::min<std::int64_t>(
                        100000000, ex0 + cost * 150);
                    if (ex1 != ex0 || !had_ex)
                        lawutil::set_change(r, ctx.actor, ex_p, ex0, ex1);
                }
            }
            if (sk_p != kNoProp) {                              // practice makes a swimmer
                const std::int64_t s0 = w.prop_int(*actor, sk_p, 0);
                if (s0 < 1000) lawutil::set_change(r, ctx.actor, sk_p, s0, s0 + 1);
            }
            if (f0 + cost >= 1000 && integ_p != kNoProp) {
                const std::int64_t i0 = w.prop_int(*actor, integ_p, 1000);
                const std::int64_t i1 = std::max<std::int64_t>(0, i0 - 250);
                lawutil::set_change(r, ctx.actor, integ_p, i0, i1);
                r.notes.push_back(i1 == 0 ? "drowned (exhausted in water " + std::to_string(lawutil::water_depth(w, np)) + " deep)"
                                          : "drowning (exhausted in deep water)");
            } else r.notes.push_back("swim (stroke cost " + std::to_string(cost) + ")");
            r.moves.push_back({ctx.actor, np});
            r.possible = true; r.ticks = 1;
            return r;
        } else if (wet_next && lawutil::water_depth(w, np) <= lawutil::kWadeDepth) {
            // wading a ford: slow, wet, but safe
            r.notes.push_back("wade");
            r.moves.push_back({ctx.actor, np});
            r.possible = true; r.ticks = 1;
            return r;
        }
    }
    if (water_at(np)) {
        auto rot45 = [](std::int64_t x, std::int64_t y, int k,
                        std::int64_t& rx, std::int64_t& ry) {
            switch (((k % 8) + 8) % 8) {   // k = quarter-turns of 45 degrees
            case 0:  rx = x;                     ry = y;                     break;
            case 1:  rx = (x - y) * 707 / 1000;  ry = (x + y) * 707 / 1000;  break;
            case 2:  rx = -y;                    ry = x;                     break;
            case 3:  rx = -(x + y) * 707 / 1000; ry = (x - y) * 707 / 1000;  break;
            case 4:  rx = -x;                    ry = -y;                    break;
            case 5:  rx = (y - x) * 707 / 1000;  ry = -(x + y) * 707 / 1000; break;
            case 6:  rx = y;                     ry = -x;                    break;
            default: rx = (x + y) * 707 / 1000;  ry = (y - x) * 707 / 1000;  break;
            }
        };
        const int side = (ctx.actor & 1ull) ? -1 : 1;    // per-actor turn bias
        static const int kTry[6] = {1, -1, 2, -2, 3, -3};
        bool steered = false;
        for (int k : kTry) {
            std::int64_t rx, ry;
            rot45(dx, dy, side * k, rx, ry);
            Vec2 cand = from;
            cand.x = from.x + static_cast<std::int32_t>(rx * covered / dist);
            cand.y = from.y + static_cast<std::int32_t>(ry * covered / dist);
            if (!water_at(cand)) { np = cand; steered = true; break; }
        }
        if (!steered) {
            r.failure = "cannot enter water";
            lawutil::add_cond(r, "water", 1, 0);
            return r;
        }
        r.notes.push_back("steered around water");
    }

    // Phase 12.4 sealed shells: a well-fortified enclosure owned by others
    // blocks entry unless the mover holds consent (owns/entry_granted).
    // Low-barrier shells stay climbable — entry then reads as trespass.
    {
        const EntityId enc = arch_enclosure_at(w, np);
        if (enc != kNoEntity) {
            const EntityId owner = arch_owner_of(w, enc);
            if (owner != kNoEntity && owner != ctx.actor) {
                const PropId p_bar = lawutil::pid(w, "access_barrier");
                const std::int64_t bar = p_bar != kNoProp
                    ? w.prop_int(*w.find(enc), p_bar, 0) : 0;
                bool consent = false;
                if (const AgentMind* am = w.find_mind(ctx.actor)) {
                    const std::string enc_key = std::to_string(
                        static_cast<std::uint64_t>(enc));
                    const Belief* ob = am->find_belief("owns:" + enc_key);
                    if (ob && ob->confidence >= 600) consent = true;
                    const Belief* gb = am->find_belief(
                        "entry_granted:" + enc_key);
                    if (gb && gb->confidence >= 500) consent = true;
                }
                if (bar >= 600 && !consent) {
                    r.failure = "sealed enclosure — no consent";
                    lawutil::add_cond(r, "access_barrier", bar, 0);
                    return r;
                }
            }
        }
    }

    // Phase 12.4 trails: compacted ground underfoot is faster going and
    // cheaper walking — wear is World Truth on the terrain cell.
    std::int64_t wear_bonus = 0;
    if (!w.terrain.empty() && w.terrain.contains(from))
        wear_bonus = w.terrain.wear_at(from);
    if (wear_bonus > 0 && arrive) {
        // arriving faster on a trail: covered already == dist, nothing to do
    } else if (wear_bonus > 0) {
        // speed the NEXT step: recompute covered with trail bonus (cap dist)
        const std::int64_t eff = covered + covered * wear_bonus / 4000;
        if (eff > covered) {
            const std::int64_t nc = std::min<std::int64_t>(eff, dist);
            if (nc > covered) {
                Vec2 tp = from;
                tp.x = from.x + static_cast<std::int32_t>(dx * nc / dist);
                tp.y = from.y + static_cast<std::int32_t>(dy * nc / dist);
                if (!water_at(tp)) np = tp;    // trail bonus yields to water
            }
        }
    }

    // Effort scales with distance actually covered (milliJ per map unit);
    // worn trails discount the cost.
    const std::int64_t dist_covered =
        static_cast<std::int64_t>(isqrt(
            static_cast<std::int64_t>(
                static_cast<std::int64_t>(np.x - from.x) * (np.x - from.x) +
                static_cast<std::int64_t>(np.y - from.y) * (np.y - from.y))));
    const std::int64_t unit_cost =
        50 * (1000 - std::min<std::int64_t>(wear_bonus, 800) / 2) / 1000;
    if (!lawutil::charge_effort(w, *actor, energy_p,
                                dist_covered * unit_cost, r))
        return r;

    r.moves.push_back({ctx.actor, np});
    r.possible = true;
    // Stride vs world-time are decoupled: ctx.parameters.ticks scales the
    // STEP LENGTH (callers pass 8 for a ~48-unit stride) but locomotion
    // still declares a 1-tick duration, so needs/clock pacing is unchanged
    // — a fast stride must not tax survival metabolism 8x.
    r.ticks = 1;
    r.notes.push_back("move " + std::to_string(covered) + "/" +
                      std::to_string(dist) + " units toward (" +
                      std::to_string(to.x) + "," + std::to_string(to.y) + ")");
    return r;
}

std::unique_ptr<INaturalLawModel> LocomotionModel::clone() const {
    return std::make_unique<LocomotionModel>(*this);
}

}  // namespace simlab
