#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "LawUtil.hpp"
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
    const std::int64_t speed = speed_p != kNoProp
        ? w.prop_int(*actor, speed_p, 0) : 0;
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

    // Effort scales with distance actually covered (milliJ per map unit).
    constexpr std::int64_t kEnergyPerUnit = 50;
    if (!lawutil::charge_energy(*actor, energy_p, covered * kEnergyPerUnit, r))
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
