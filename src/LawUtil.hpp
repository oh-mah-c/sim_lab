#pragma once
// Internal helpers shared by the law solvers (physics / extraction /
// processing / thermal). One definition — the reach, energy and wear rules
// must be identical in every domain, not copy-pasted per translation unit.
#include <algorithm>
#include <string>
#include "simlab/Laws.hpp"
#include "simlab/World.hpp"

namespace simlab::lawutil {

// Weapon profile of one blow (Phase 12.6). All channels are material/
// composite properties: edge_sharpness (cut), blunt_mass (impulse),
// coat_toxin / coat_strain (what a penetrating edge injects). `mech`
// overrides the damage-mechanism tag ("projectile", "blast").
struct Blow {
    std::int64_t sharp = 0, blunt = 0, coat_toxin = 0, coat_strain = 0;
    const char* mech = nullptr;
};
std::int64_t armor_resist(const World& w, EntityId body);
Blow blow_of(const World& w, EntityId weapon, std::int64_t base_edge);
void deliver_blow(const World& w, PredictionResult& r, EntityId tid,
                  std::int64_t E, const Blow& b, EntityId striker,
                  std::int64_t& wear_total);
// Energetic release around `center` (WarfareLaws.cpp). 0 = too weak to blast.
std::int64_t blast(const World& w, PredictionResult& r, Vec2 center,
                   std::int64_t expansion, std::int64_t casing,
                   std::int64_t mass, const std::vector<EntityId>& exclude);

// Actors without a `reach` prop get a default arm-span of 64 map units.
constexpr std::int64_t kDefaultReach = 64;

inline PropId pid(const World& w, const char* name) {
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
}

inline void add_cond(PredictionResult& r, const char* what, std::int64_t actual,
                     std::int64_t required) {
    Condition c; c.what = what; c.actual = actual; c.required = required;
    c.met = actual >= required;
    r.conditions.push_back(std::move(c));
}

// Entity is inside a composite structure (`contained`) -> laws skip it.
inline bool is_contained(const World& w, const Entity& e, PropId contained_pid) {
    if (contained_pid == kNoProp) return false;
    const Value* cv = w.prop_of(e, contained_pid);
    return cv && cv->type == ValType::Boolean && cv->b;
}

// Reach constraint (TODO §6.1): the actor must be within `reach` (+ tool
// bonus) map units of the target. Records a reach condition.
inline bool in_reach(const World& w, const Entity& actor, const Entity& target,
                     PropId reach_pid, std::int64_t reach_bonus, PredictionResult* r = nullptr) {
    const std::int64_t reach = (reach_pid != kNoProp
        ? w.prop_int(actor, reach_pid, kDefaultReach) : kDefaultReach) + reach_bonus;
    const std::int64_t d2 = dist2(actor.pos, target.pos);
    const bool ok = d2 <= reach * reach;
    if (r) {
        // report in map units: required distance = ceil(sqrt(d2)) via integer search
        std::int64_t d = 0;
        while (d * d < d2) ++d;
        add_cond(*r, "reach", reach, d);
    }
    return ok;
}

inline void set_change_v(PredictionResult& r, EntityId id, PropId prop,
                         const Value& before, const Value& after) {
    StateChange ch;
    ch.entity = id; ch.prop = prop; ch.had_before = true;
    ch.before = before; ch.after = after;
    r.changes.push_back(ch);
}
inline void set_change(PredictionResult& r, EntityId id, PropId prop,
                       std::int64_t before, std::int64_t after) {
    set_change_v(r, id, prop, Value::integer(before), Value::integer(after));
}

// Energy accounting (TODO §3): actors carrying an `energy` prop pay the
// action's EFFORT; actors without one are abstract/unbounded. A sharper tool
// multiplies the effect of effort, never its cost. False + failure when short.
inline bool charge_energy(const Entity& actor, PropId energy_pid, std::int64_t cost,
                          PredictionResult& r) {
    if (energy_pid == kNoProp || cost <= 0) return true;
    const Value* ev = actor.props.get(energy_pid);
    if (!ev) return true;                       // unbounded actor
    const std::int64_t before = ev->i;
    add_cond(r, "energy", before, cost);
    if (before < cost) {
        r.failure = "insufficient energy: need " + std::to_string(cost) +
                    ", have " + std::to_string(before);
        return false;
    }
    set_change(r, actor.id, energy_pid, before, before - cost);
    return true;
}

// First wielded, alive, non-contained entity of `actor` (deterministic: the
// relation set is sorted). The striker that takes the reaction force.
inline EntityId wielded_tool(const World& w, EntityId actor) {
    const PropId cont = pid(w, "contained");
    for (const Relation& rel : w.relations_of(actor, "wielding")) {
        if (rel.a != actor) continue;
        const Entity* t = w.find(rel.b);
        if (t && t->alive && !is_contained(w, *t, cont)) return rel.b;
    }
    return kNoEntity;
}

// Reaction wear (Newton III, TODO §6 "both may be damaged"): a blow that
// removes `dealt` permille integrity from a target of hardness `target_hard`
// wears the striker in proportion to how hard the target is relative to it.
inline std::int64_t reaction_wear(const World& w, EntityId tool, std::int64_t dealt,
                                  std::int64_t target_hard) {
    const Entity* t = w.find(tool);
    if (!t || dealt <= 0) return 0;
    const std::int64_t tool_hard = w.prop_int(*t, pid(w, "hardness"), 300);
    return dealt * target_hard / (tool_hard + target_hard + 100) / 4;
}

inline void apply_tool_wear(const World& w, EntityId tool, std::int64_t wear,
                            PredictionResult& r) {
    const Entity* t = w.find(tool);
    const PropId integ = pid(w, "integrity");
    if (!t || integ == kNoProp || wear <= 0) return;
    const std::int64_t cur = w.prop_int(*t, integ, 500);
    set_change(r, tool, integ, cur, std::max<std::int64_t>(0, cur - wear));
    r.notes.push_back("tool " + std::to_string(tool) + " worn -" + std::to_string(wear));
}

// Phase 10: effective motor precision factor (permille) from anatomy —
// arm function (severed/broken arms weaken grip and craft) scaled by
// circulatory status (blood_volume < 600 -> shaky hands). Entities without
// anatomy or circulation return 1000.
inline std::int64_t anatomy_motor(const World& w, const Entity& actor) {
    std::int64_t f = 1000;
    if (actor.alive)
        f = f * w.anatomy_function(actor.id, "arm") / 1000;
    const PropId bv = pid(w, "blood_volume");
    if (bv != kNoProp) {
        const Value* v = w.prop_of(actor, bv);
        if (v && v->type == ValType::Integer) {
            const std::int64_t b = v->i;
            if (b < 600) f = f * std::max<std::int64_t>(200, b * 1000 / 600) / 1000;
        }
    }
    return f;
}

// Consciousness gate (Phase 10): blood_volume < 300 -> consciousness < 300
// -> no voluntary action. Returns false + note when the actor is out.
inline bool conscious_enough(const World& w, const Entity& actor,
                             PredictionResult& r) {
    const PropId cs = pid(w, "consciousness");
    if (cs == kNoProp) return true;
    const Value* v = w.prop_of(actor, cs);
    if (!v || v->type != ValType::Integer) return true;
    if (v->i >= 300) return true;
    r.failure = "unconscious (consciousness " + std::to_string(v->i) + ")";
    return false;
}

}  // namespace simlab::lawutil
