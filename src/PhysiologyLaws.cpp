// PhysiologyLaws.cpp — Phase 5 completion: agent body-state solver (TODO §10.1).
//
// Needs (hunger/thirst/fatigue) are entity props in domain "need" — private
// body state, never observable to other entities. They grow with `metabolism`
// inside World::advance_ticks (ledgered as "metabolize" events).
//
// This law provides the two generic satisfiers. Everything is property-driven:
//   consume — target must carry `nutrition` and/or `hydration` (permille of
//             the matching need relieved per unit eaten). `quantity` is
//             decremented unit-by-unit; without it the whole entity is eaten.
//             The actor's `hunger`/`thirst` drop by the nutrition/hydration
//             values. An actor with no need props has nothing to satisfy.
//   rest    — actor-only: `fatigue` recovers by `rest_recovery` permille per
//             tick over ctx.ticks. Requires the actor to carry `fatigue`.
//
// No material, species or food type is named anywhere — a new edible is a
// new data row, exactly like every other domain.

#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {
namespace {

constexpr std::int64_t kDefaultReach = 64;
constexpr std::int64_t kConsumeTicks = 2;
constexpr std::int64_t kConsumeEnergy = 50;    // milliJ — eating is cheap
constexpr std::int64_t kDefaultRestRecovery = 20;  // permille per tick
// Nutrition is CALORIES: each permille of `nutrition` eaten converts to
// this many milliJ of actor `energy` (capped by `max_energy`). Without
// this, `energy` was a static tank that eating never refilled — food
// relieved hunger but could not power reproduction or industry.
constexpr std::int64_t kNutToEnergy = 25;      // milliJ per permille
constexpr std::int64_t kDefaultMaxEnergy = 500000;

PropId prop_id(const World& w, const char* name) {
    const PropertyDef* d = w.props.find_name(name);
    return d ? d->id : kNoProp;
}

bool prop_bool(const World& w, const Entity& e, PropId p) {
    const Value* v = w.prop_of(e, p);
    return v && ((v->type == ValType::Boolean && v->b) ||
                 (v->type == ValType::Integer && v->i != 0));
}

bool in_reach(const World& w, const Entity& actor, const Entity& target,
              PropId reach_pid) {
    const std::int64_t reach = reach_pid != kNoProp
        ? w.prop_int(actor, reach_pid, kDefaultReach) : kDefaultReach;
    return dist2(actor.pos, target.pos) <= reach * reach;
}

void set_change(PredictionResult& r, EntityId id, PropId prop,
                std::int64_t before, std::int64_t after) {
    StateChange ch;
    ch.entity = id; ch.prop = prop; ch.had_before = true;
    ch.before = Value::integer(before);
    ch.after = Value::integer(after);
    r.changes.push_back(ch);
}

// ---- consume ------------------------------------------------------------------
static PredictionResult predict_consume(const World& w,
                                        const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 6; r.law_name = "physiology.metabolism";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropId hunger = prop_id(w, "hunger");
    const PropId thirst = prop_id(w, "thirst");
    const PropId fatigue = prop_id(w, "fatigue");
    const PropId nutrition = prop_id(w, "nutrition");
    const PropId hydration = prop_id(w, "hydration");
    const PropId quantity = prop_id(w, "quantity");
    const PropId integrity = prop_id(w, "integrity");
    const PropId energy = prop_id(w, "energy");
    const PropId max_energy = prop_id(w, "max_energy");
    const PropId reach = prop_id(w, "reach");
    const PropId contained = prop_id(w, "contained");
    const PropId toxicity = prop_id(w, "toxicity");
    const PropId fruit_material = prop_id(w, "fruit_material");
    const PropId fruit_count = prop_id(w, "fruit_count");
    (void)fatigue;
    if (hunger == kNoProp && thirst == kNoProp) {
        r.failure = "actor has no hunger/thirst need to satisfy";
        return r;
    }
    if (nutrition == kNoProp && hydration == kNoProp) {
        r.failure = "no consumable properties defined";
        return r;
    }
    if (integrity == kNoProp) { r.failure = "no integrity property"; return r; }

    const bool has_hunger = hunger != kNoProp && w.prop_of(*actor, hunger);
    const bool has_thirst = thirst != kNoProp && w.prop_of(*actor, thirst);
    // Need relief accumulates ACROSS targets within one action: eating two
    // berries relieves twice as much as one. Track running totals and emit a
    // single change per need prop — per-target world reads would make the
    // last write win instead of stacking.
    std::int64_t hunger_after = has_hunger ? w.prop_int(*actor, hunger, 0) : 0;
    std::int64_t thirst_after = has_thirst ? w.prop_int(*actor, thirst, 0) : 0;
    bool hunger_dirty = false, thirst_dirty = false;
    std::int64_t energy_gain = 0;   // milliJ metabolized from eaten nutrition
    std::int64_t harm = 0;          // integrity-scale damage from toxins

    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;                    // cannot eat oneself
        const Entity* t = w.find(tid);
        if (!t) continue;
        // Dead organisms are edible remains (Phase 7 food chain): their
        // `nutrition`/`hydration` still counts World-Truth-wise. A carcass is
        // stripped as it is eaten so it cannot feed twice — see below.
        if (contained != kNoProp && prop_bool(w, *t, contained)) continue;
        if (!in_reach(w, *actor, *t, reach)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }

        // ---- two-layer flora: body vs fruit ---------------------------------
        // A fruiting plant (fruit_material + fruit_count props) is NOT itself
        // food: nutrition/hydration/toxicity come from the FRUIT material,
        // and the stock drained is `fruit_count` — never `integrity`. The
        // bush survives bare (fruit_count 0) and fruiting resumes via drift.
        MaterialId food_mat = t->material;
        PropId stock_pid = quantity;
        bool fruiting = false;
        if (fruit_material != kNoProp && fruit_count != kNoProp) {
            if (const Value* fmv = w.prop_of(*t, fruit_material);
                fmv && fmv->type == ValType::Text && !fmv->s.empty() &&
                w.prop_of(*t, fruit_count)) {
                const MaterialId fm = w.materials.id_of(fmv->s);
                if (fm != kNoMaterial) { food_mat = fm; fruiting = true;
                                         stock_pid = fruit_count; }
            }
        }
        auto food_prop = [&](PropId p) -> std::int64_t {
            if (p == kNoProp) return 0;
            if (!fruiting) return w.prop_int(*t, p, 0);   // entity+material fallback
            const MaterialDef* fd = w.materials.find(food_mat);
            if (!fd) return 0;
            const auto it = fd->props.find(p);
            return it == fd->props.end() ? 0 : it->second.i;
        };
        const std::int64_t nut = food_prop(nutrition);
        const std::int64_t hyd = food_prop(hydration);
        const std::int64_t tox = food_prop(toxicity);
        if (nut <= 0 && hyd <= 0) {
            r.notes.push_back("target " + std::to_string(tid) + " not edible/drinkable");
            continue;
        }

        // consumption accounting: stock entities (`quantity` items,
        // `fruit_count` on fruiting flora) lose one unit per consume; whole
        // entities (no stock) are eaten entirely. Corpses are stripped of
        // nutrition once consumed so remains cannot feed repeatedly.
        const Value* qv = stock_pid != kNoProp ? w.prop_of(*t, stock_pid) : nullptr;
        const bool whole = qv == nullptr;
        const std::int64_t q_before = whole ? 0 : qv->i;
        if (!whole && q_before <= 0) {
            r.notes.push_back("target " + std::to_string(tid) +
                              (fruiting ? " has no fruit" : " is empty"));
            continue;                                     // bare branch / empty remains
        }
        const std::int64_t units = whole ? 1 : std::min<std::int64_t>(1, q_before);
        bool strip = false;
        if (!whole) {
            const std::int64_t q_after = q_before - units;
            set_change(r, tid, stock_pid, q_before, q_after);
            if (!fruiting) {
                // finite stock items die when emptied; a fruiting plant's
                // BODY is never touched by fruit loss — bare, not dead.
                if (t->alive) {
                    if (q_after <= 0)
                        set_change(r, tid, integrity,
                                   w.prop_int(*t, integrity, 500), 0);
                } else if (q_after <= 0) strip = true;
            }
        } else if (t->alive) {
            set_change(r, tid, integrity,
                       w.prop_int(*t, integrity, 500), 0);
        } else {
            strip = true;
        }
        if (strip) {
            if (nut > 0) set_change(r, tid, nutrition, nut, 0);
            if (hyd > 0) set_change(r, tid, hydration, hyd, 0);
        }

        // relief is per-unit nutrition/hydration (permille of the need)
        if (has_hunger && nut > 0) {
            hunger_after = std::max<std::int64_t>(0, hunger_after - nut * units);
            hunger_dirty = true;
            energy_gain += nut * units * kNutToEnergy;
        }
        if (has_thirst && hyd > 0) {
            thirst_after = std::max<std::int64_t>(0, thirst_after - hyd * units);
            thirst_dirty = true;
        }
        // toxin: `toxicity` (permille of the integrity scale per unit, read
        // from the FOOD source — fruit material for flora, entity material
        // otherwise) damages the EATER — relief and poison arrive together,
        // so a bad meal still fills the belly while it kills. This is the
        // data hook that lets a witnessed death teach a material taboo.
        if (tox > 0) harm += tox * units;
        r.possible = true;
        std::ostringstream n;
        n << "consume " << tid << " nut=" << nut << " hyd=" << hyd
          << (fruiting ? " fruit " + std::to_string(q_before) + "->" +
               std::to_string(q_before - units)
             : whole ? " (whole)" : " qty " + std::to_string(q_before) + "->" +
               std::to_string(q_before - units));
        r.notes.push_back(n.str());
    }

    // emit accumulated need relief once per prop (see note above)
    if (hunger_dirty)
        set_change(r, ctx.actor, hunger, w.prop_int(*actor, hunger, 0), hunger_after);
    if (thirst_dirty)
        set_change(r, ctx.actor, thirst, w.prop_int(*actor, thirst, 0), thirst_after);
    if (harm > 0) {
        const std::int64_t ib = w.prop_int(*actor, integrity, 0);
        set_change(r, ctx.actor, integrity, ib, std::max<std::int64_t>(0, ib - harm));
    }

    if (r.possible) {
        r.ticks = kConsumeTicks;
        if (energy != kNoProp) {
            const Value* ev = w.prop_of(*actor, energy);
            if (ev && ev->i < kConsumeEnergy) {
                r.possible = false; r.changes.clear(); r.notes.clear();
                r.failure = "insufficient energy to eat";
                return r;
            }
            if (ev) {
                // Eat first (pay the action cost), then store the calories:
                // net = cur - cost + gain, clamped to the species' energy
                // ceiling. Only actors carrying an `energy` reserve get the
                // bookkeeping — unbounded actors keep legacy behavior.
                const std::int64_t cap = max_energy != kNoProp
                    ? w.prop_int(*actor, max_energy, kDefaultMaxEnergy)
                    : kDefaultMaxEnergy;
                // pay the action cost, then store calories only up to the
                // storage ceiling — excess energy already carried is never
                // destroyed by the cap (conservation).
                const std::int64_t after_cost = ev->i - kConsumeEnergy;
                const std::int64_t headroom =
                    std::max<std::int64_t>(0, cap - after_cost);
                const std::int64_t nxt =
                    after_cost + std::min(energy_gain, headroom);
                set_change(r, ctx.actor, energy, ev->i, nxt);
            }
        }
    }
    if (!r.possible && r.failure.empty()) r.failure = "nothing edible in reach";
    return r;
}

// ---- rest ---------------------------------------------------------------------
static PredictionResult predict_rest(const World& w,
                                     const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 6; r.law_name = "physiology.metabolism";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropId fatigue = prop_id(w, "fatigue");
    const PropId recovery = prop_id(w, "rest_recovery");
    if (fatigue == kNoProp || !w.prop_of(*actor, fatigue)) {
        r.failure = "actor has no fatigue need";
        return r;
    }
    const std::int64_t rec = (recovery != kNoProp
        ? w.prop_int(*actor, recovery, kDefaultRestRecovery)
        : kDefaultRestRecovery);
    const std::int64_t ticks = ctx.parameters.ticks > 0 ? ctx.parameters.ticks : 1;
    const std::int64_t cur = w.prop_int(*actor, fatigue, 0);
    const std::int64_t relief = rec * ticks;
    set_change(r, ctx.actor, fatigue, cur, std::max<std::int64_t>(0, cur - relief));
    r.possible = true;
    r.ticks = ticks;
    std::ostringstream n;
    n << "rest " << ticks << " ticks rec=" << rec
      << " fatigue " << cur << "->" << std::max<std::int64_t>(0, cur - relief);
    r.notes.push_back(n.str());
    return r;
}

}  // namespace

LawId PhysiologyModel::id() const { return 6; }

bool PhysiologyModel::supports(const World& /*w*/, const InteractionContext& ctx) const {
    if (ctx.parameters.action == "rest") return true;   // actor-only action
    return ctx.parameters.action == "consume" && !ctx.targets.empty();
}

PredictionResult PhysiologyModel::predict(const World& w,
                                          const InteractionContext& ctx) const {
    if (ctx.parameters.action == "rest") return predict_rest(w, ctx);
    return predict_consume(w, ctx);
}

std::unique_ptr<INaturalLawModel> PhysiologyModel::clone() const {
    return std::make_unique<PhysiologyModel>();
}

}  // namespace simlab
