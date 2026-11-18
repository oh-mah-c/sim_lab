#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {

using namespace lawutil;

LawId PhysicsModel::id() const { return 1; }

bool PhysicsModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "impact" && !ctx.targets.empty();
}

// Damage model (all integer, all property-driven):
//   E     = applied force (milli-units)
//   armor = target hardness (0..1000)
//   crit  = brittleness raises crack damage, lowers needed force to fracture
//   mass  slows the effective hit: dmg /= (1 + mass/50000)
//   integrity loss = E * (1000 + 2*crit) / (armor + 500) / mass_factor
// Fracture: if a single hit exceeds `fracture_threshold` permille of the
// target's brittleness-scaled ceiling, integrity is set to 0 (destroyed).
// Nothing here names a material — obsidian vs wood differ only in data.
PredictionResult PhysicsModel::predict(const World& w, const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id();
    r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    // Resolve well-known property ids by name (per-world registry).
    const PropertyDef* pd_hard = w.props.find_name("hardness");
    const PropertyDef* pd_brit = w.props.find_name("brittleness");
    const PropertyDef* pd_mass = w.props.find_name("mass");
    const PropertyDef* pd_integ = w.props.find_name("integrity");
    if (!pd_hard || !pd_brit || !pd_mass || !pd_integ) {
        r.failure = "physics requires hardness/brittleness/mass/integrity properties";
        return r;
    }

    const std::int64_t E0 = ctx.parameters.force > 0 ? ctx.parameters.force : 1000;
    // wielded tools: thrust_range extends reach, edge sharpens the blow
    const auto [treach, tedge] = w.tool_bonus(ctx.actor);
    // Nhịp 4c: motor_precision scales transmitted force — a master's
    // strike lands harder, a clumsy one's glances off.
    const PropId motor_pid = pid(w, "mind_motor_precision");
    const std::int64_t motor = motor_pid != kNoProp
        ? w.prop_int(*actor, motor_pid, 1000) : 1000;
    const std::int64_t E = E0 * (1000 + tedge) / 1000 * motor / 1000;
    const PropId reach_pid = pid(w, "reach"), energy_pid = pid(w, "energy"),
                 contained_pid = pid(w, "contained");
    const EntityId striker = wielded_tool(w, ctx.actor);
    std::int64_t wear_total = 0;

    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (is_contained(w, *t, contained_pid)) continue;       // inside a structure
        if (!in_reach(w, *actor, *t, reach_pid, treach, &r)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }
        const std::int64_t hard = w.prop_int(*t, pd_hard->id, 300);
        const std::int64_t brit = w.prop_int(*t, pd_brit->id, 400);
        const std::int64_t mass = w.prop_int(*t, pd_mass->id, 1000);
        const std::int64_t integ = w.prop_int(*t, pd_integ->id, 500);

        const std::int64_t mass_factor = 1000 + mass / 50;      // heavier absorbs more
        std::int64_t dmg = E * (1000 + 2 * brit) / ((hard + 500) * mass_factor / 1000);
        if (dmg < 1 && E > 0) dmg = 1;
        // brittle targets fracture when the hit exceeds what their surface can take
        const bool fracture = dmg * 4 > (hard + brit) && brit > 600;
        const std::int64_t new_integ = fracture ? 0 : std::max<std::int64_t>(0, integ - dmg);

        StateChange ch;
        ch.entity = tid; ch.prop = pd_integ->id; ch.had_before = true;
        ch.before = Value::integer(integ);
        ch.after = Value::integer(new_integ);
        r.changes.push_back(ch);
        if (striker != kNoEntity && striker != tid)
            wear_total += reaction_wear(w, striker, integ - new_integ, hard);

        std::ostringstream n;
        n << "impact E=" << E << " on " << tid
          << " hard=" << hard << " brit=" << brit << " mass=" << mass
          << " -> dmg=" << dmg << (fracture ? " FRACTURE" : "");
        r.notes.push_back(n.str());
        r.possible = true;
    }
    // precise hands damage their own tools less (collateral wear proxy
    // for self-injury: 1000 -> 1.0x, 200 -> 1.4x, 2500 -> 0.25x)
    if (r.possible && wear_total > 0)
        wear_total = wear_total *
            std::clamp<std::int64_t>(3000 - motor, 0, 3000) / 2000;
    if (r.possible && wear_total > 0) apply_tool_wear(w, striker, wear_total, r);
    if (r.possible && !charge_energy(*actor, energy_pid, E0, r)) {
        r.possible = false; r.changes.clear(); r.spawns.clear(); r.notes.clear();
        return r;
    }
    r.ticks = ctx.parameters.ticks;
    if (!r.possible && r.failure.empty()) r.failure = "no valid targets";
    return r;
}

std::unique_ptr<INaturalLawModel> PhysicsModel::clone() const {
    return std::make_unique<PhysicsModel>();
}

}  // namespace simlab
