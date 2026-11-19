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

LawId LocomotionModel::id() const { return 9; }

bool LocomotionModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "move" && ctx.parameters.has_destination;
}

PredictionResult LocomotionModel::predict(const World& w,
                                          const InteractionContext& ctx) const {
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

    // Terrain: water is a hard boundary for land-bound actors.
    if (!w.terrain.empty() && w.terrain.contains(np) && w.terrain.is_water(np)) {
        r.failure = "cannot enter water";
        lawutil::add_cond(r, "water", 1, 0);
        return r;
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
                np = tp;
                // re-check water at the further point
                if (w.terrain.contains(np) && w.terrain.is_water(np)) {
                    r.failure = "cannot enter water";
                    return r;
                }
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
    if (!lawutil::charge_energy(*actor, energy_p,
                                dist_covered * unit_cost, r))
        return r;

    r.moves.push_back({ctx.actor, np});
    r.possible = true;
    r.notes.push_back("move " + std::to_string(covered) + "/" +
                      std::to_string(dist) + " units toward (" +
                      std::to_string(to.x) + "," + std::to_string(to.y) + ")");
    return r;
}

std::unique_ptr<INaturalLawModel> LocomotionModel::clone() const {
    return std::make_unique<LocomotionModel>(*this);
}

}  // namespace simlab
