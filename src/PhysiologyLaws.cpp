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
#include "LawUtil.hpp"
#include <algorithm>
#include <sstream>

namespace simlab {
using namespace lawutil;
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
    static thread_local LitPidCache c_;
    return c_.get(w.props, name);
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
    const PropId consciousness = prop_id(w, "consciousness");
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
    std::int64_t mass_gain = 0;     // solid matter awaiting the gut (10.3)
    std::int64_t fluid_gain = 0;    // liquid awaiting the kidneys (10.3)
    std::int64_t toxin_gain = 0;    // Phase 11.2: blighted meals poison
    std::int64_t energy_penalty = 0;// the gut and the calories both suffer

    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;                    // cannot eat oneself
        const Entity* t = w.find(tid);
        if (!t) continue;
        // Dead organisms are edible remains (Phase 7 food chain): their
        // `nutrition`/`hydration` still counts World-Truth-wise. A carcass is
        // stripped as it is eaten so it cannot feed twice — see below.
        if (contained != kNoProp && prop_bool(w, *t, contained)) continue;
        // an awake animal (consciousness >= 300, no stock to nibble) cannot
        // be swallowed whole while it lives — it must be subdued first
        // (struck/bled until unconscious or dead). Physics, not a taboo.
        if ((w.ruleset & World::kRulePrimordial) && t->alive && consciousness != kNoProp &&
            w.prop_int(*t, consciousness, 0) >= 300 &&
            !(quantity != kNoProp && w.prop_of(*t, quantity)) &&
            !(fruit_count != kNoProp && w.prop_of(*t, fruit_count))) {
            r.notes.push_back("target " + std::to_string(tid) + " is awake and resists");
            continue;
        }
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
        // digestion bookkeeping: relief is instant, but the matter itself
        // still has to pass through — undigested load feeds bowel/bladder
        // fill in anatomy_drift (Phase 10.3).
        if (nut > 0) mass_gain += nut * units * 4;
        if (hyd > 0) fluid_gain += hyd * units * 4;
        // toxin: `toxicity` (permille of the integrity scale per unit, read
        // from the FOOD source — fruit material for flora, entity material
        // otherwise) damages the EATER — relief and poison arrive together,
        // so a bad meal still fills the belly while it kills. This is the
        // data hook that lets a witnessed death teach a material taboo.
        if (tox > 0) harm += tox * units;
        // Phase 11.2 blight: raw fruit from a spore-infected plant is a
        // gut poison — toxin_load rises and calorie gain is blunted. A
        // cooked_state >= 500 plant is preserved: the same heat/chemical
        // rule that sterilizes meat sterilizes standing fruit. The toxin
        // change lands in ev.changes, so outcome beliefs and the actor's
        // event_charge engram mark the plant as the culprit by themselves.
        if (fruiting) {
            const PropId cooked_p = prop_id(w, "cooked_state");
            const bool sterile = cooked_p != kNoProp &&
                w.prop_int(*t, cooked_p, 0) >= 500;
            if (!sterile) {
                const PropId ps_p = prop_id(w, "pathogen_strain"),
                             pl_p = prop_id(w, "pathogen_load"),
                             spr_p = prop_id(w, "spore_rate"),
                             vir_p = prop_id(w, "virulence");
                if (ps_p != kNoProp && spr_p != kNoProp) {
                    std::int64_t pen = 0;
                    for (const Relation& rel : w.relations_of(tid, "carries")) {
                        if (rel.a != tid) continue;
                        const Entity* ie = w.find(rel.b);
                        if (!ie || !ie->alive) continue;
                        const std::int64_t seid = w.prop_int(*ie, ps_p, 0);
                        const Entity* st = seid > 0
                            ? w.find(static_cast<EntityId>(seid)) : nullptr;
                        if (!st || !st->alive) continue;
                        if (w.prop_int(*st, spr_p, 0) <= 0) continue;
                        const std::int64_t ld = pl_p != kNoProp
                            ? w.prop_int(*ie, pl_p, 0) : 0;
                        const std::int64_t vr = vir_p != kNoProp
                            ? w.prop_int(*st, vir_p, 0) : 0;
                        pen += std::min<std::int64_t>(200, ld * vr / 400);
                    }
                    pen = std::min<std::int64_t>(500, pen) * units;
                    if (pen > 0) {
                        toxin_gain += pen;
                        energy_penalty += pen * 8;
                        r.notes.push_back("blighted fruit: gut poison");
                    }
                }
            }
        }
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
    {
        const PropId um_p = prop_id(w, "undigested_mass");
        const PropId uf_p = prop_id(w, "undigested_fluid");
        if (mass_gain > 0 && um_p != kNoProp && w.prop_of(*actor, um_p))
            set_change(r, ctx.actor, um_p, w.prop_int(*actor, um_p, 0),
                       w.prop_int(*actor, um_p, 0) + mass_gain);
        if (fluid_gain > 0 && uf_p != kNoProp && w.prop_of(*actor, uf_p))
            set_change(r, ctx.actor, uf_p, w.prop_int(*actor, uf_p, 0),
                       w.prop_int(*actor, uf_p, 0) + fluid_gain);
    }
    if (hunger_dirty)
        set_change(r, ctx.actor, hunger, w.prop_int(*actor, hunger, 0), hunger_after);
    if (thirst_dirty)
        set_change(r, ctx.actor, thirst, w.prop_int(*actor, thirst, 0), thirst_after);
    if (harm > 0) {
        const std::int64_t ib = w.prop_int(*actor, integrity, 0);
        set_change(r, ctx.actor, integrity, ib, std::max<std::int64_t>(0, ib - harm));
    }
    if (toxin_gain > 0) {
        const PropId tl_p = prop_id(w, "toxin_load");
        if (tl_p != kNoProp && w.prop_of(*actor, tl_p)) {
            const std::int64_t tb = w.prop_int(*actor, tl_p, 0);
            set_change(r, ctx.actor, tl_p, tb,
                       std::min<std::int64_t>(1000, tb + toxin_gain));
        }
    }
    if (energy_penalty > 0)
        energy_gain = std::max<std::int64_t>(0, energy_gain - energy_penalty);

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

// ---- evacuate (Phase 10.3): generic hollow-organ voiding ---------------------
// Actor-only-ish verb: targets[0] is the LUMEN PART to evacuate (must be a
// part_of child of the actor). Everything reads part props — lumen_capacity,
// contents_load, content_viscosity, contractility, infection_level,
// evacuate_material — no organ names. Viscosity above contractility means
// mechanical obstruction: the attempt fails physically, load stays, pain
// spikes. Near-threshold or infected contents pass but hurt and bleed.
static PredictionResult predict_evacuate(const World& w,
                                         const InteractionContext& ctx) {
    PredictionResult r;
    r.law = 6; r.law_name = "physiology.metabolism";
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    if (ctx.targets.empty()) { r.failure = "evacuate needs an organ target"; return r; }
    const EntityId part_id = ctx.targets[0];
    const Entity* part = w.find(part_id);
    if (!part || !part->alive) { r.failure = "no such organ"; return r; }
    // must be an attached part of the actor
    bool attached = false;
    for (const Relation& rel : w.relations_of(ctx.actor, "part_of"))
        if (rel.b == ctx.actor && rel.a == part_id) { attached = true; break; }
    if (!attached) { r.failure = "organ is not part of the actor"; return r; }

    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, w.props);
    const PropId lc = pid("lumen_capacity"), cl = pid("contents_load"),
               ca = pid("contents_accum"), cv = pid("content_viscosity"),
               ct = pid("contractility"), em = pid("evacuate_material"),
               pain = pid("pain_level"), bleed = pid("bleed_rate"),
               inf = pid("infection_level"), energy = pid("energy"),
               qty = pid("quantity"), integ = pid("integrity");
    if (lc == kNoProp || w.prop_int(*part, lc, 0) <= 0) {
        r.failure = "target has no lumen"; return r;
    }
    const std::int64_t load = cl != kNoProp ? w.prop_int(*part, cl, 0) : 0;
    if (load < 50) { r.failure = "organ is empty"; return r; }
    const std::int64_t visc = cv != kNoProp ? w.prop_int(*part, cv, 0) : 0;
    const std::int64_t contract = ct != kNoProp ? w.prop_int(*part, ct, 0) : 0;
    const std::int64_t pinf = inf != kNoProp ? w.prop_int(*part, inf, 0) : 0;

    // mechanical obstruction: contents thicker than the wall can push —
    // nothing passes, but the strain still hurts.
    const bool obstructed = visc > contract;
    // near-threshold or inflamed contents pass with trauma
    const bool painful = !obstructed &&
        ((visc >= contract - 200 && visc <= contract) || pinf > 200);

    if (!lawutil::charge_energy(*actor, energy, 300, r)) {
        r.possible = false; r.changes.clear(); r.spawns.clear(); r.notes.clear();
        return r;
    }
    r.possible = true;
    r.ticks = 100;
    if (obstructed) {
        if (pain != kNoProp && w.prop_of(*actor, pain))
            set_change(r, ctx.actor, pain, w.prop_int(*actor, pain, 0),
                       std::min<std::int64_t>(1000,
                           w.prop_int(*actor, pain, 0) + 300));
        r.notes.push_back("evacuate obstructed: viscosity " +
                          std::to_string(visc) + " > contractility " +
                          std::to_string(contract));
        return r;
    }
    // success: contents leave through the wall as the declared material
    set_change(r, part_id, cl, load, 0);
    if (ca != kNoProp && w.prop_of(*part, ca))
        set_change(r, part_id, ca, w.prop_int(*part, ca, 0), 0);
    if (painful) {
        if (pain != kNoProp && w.prop_of(*actor, pain))
            set_change(r, ctx.actor, pain, w.prop_int(*actor, pain, 0),
                       std::min<std::int64_t>(1000,
                           w.prop_int(*actor, pain, 0) + 550));
        if (bleed != kNoProp && w.prop_of(*part, bleed))
            set_change(r, part_id, bleed, w.prop_int(*part, bleed, 0),
                       std::min<std::int64_t>(1000,
                           w.prop_int(*part, bleed, 0) + 15));
    }
    std::string evac_name = "waste";
    if (em != kNoProp) {
        const Value* ev = w.prop_of(*part, em);
        if (ev && ev->type == ValType::Text && !ev->s.empty()) evac_name = ev->s;
    }
    SpawnSpec sp;
    sp.kind = evac_name;
    if (const MaterialDef* wm = w.materials.find_name(evac_name))
        sp.material = wm->id;
    sp.pos = actor->pos;
    if (qty != kNoProp) sp.props.set(qty, Value::integer(load / 4 + 50));
    // Phase 10.4 fecal-oral vector: shed waste carries the strain of the
    // host's first active infection — the pathogen survives outside the
    // body inside its own evacuate_material, fully data-driven.
    {
        const PropId ps = pid("pathogen_strain");
        if (ps != kNoProp) {
            for (const Relation& rel : w.relations_of(ctx.actor, "carries")) {
                if (rel.a != ctx.actor) continue;
                const Entity* ie = w.find(rel.b);
                if (!ie || !ie->alive) continue;
                const std::int64_t seid = w.prop_int(*ie, ps, 0);
                if (seid > 0) {
                    const PropId sr = pid("strain_ref");
                    if (sr != kNoProp)
                        sp.props.set(sr, Value::integer(seid));
                    break;
                }
            }
        }
    }
    r.spawns.push_back(sp);
    r.notes.push_back("evacuate " + evac_name + " load " +
                      std::to_string(load) + "->0" +
                      (painful ? " (painful)" : ""));
    return r;
}

}  // namespace

LawId PhysiologyModel::id() const { return 6; }

bool PhysiologyModel::supports(const World& /*w*/, const InteractionContext& ctx) const {
    if (ctx.parameters.action == "rest") return true;   // actor-only action
    if (ctx.parameters.action == "evacuate") return true;
    return ctx.parameters.action == "consume" && !ctx.targets.empty();
}

PredictionResult PhysiologyModel::predict(const World& w,
                                          const InteractionContext& ctx) const {
    if (ctx.parameters.action == "rest") return predict_rest(w, ctx);
    if (ctx.parameters.action == "evacuate") return predict_evacuate(w, ctx);
    return predict_consume(w, ctx);
}

std::unique_ptr<INaturalLawModel> PhysiologyModel::clone() const {
    return std::make_unique<PhysiologyModel>();
}

// ---- treat: primitive, property-driven medicine -----------------------------
// The patient is targets[0]; the dressing/material is targets[1]. The law
// finds the patient's WORST attached anatomical part (max bleed + infection)
// and lets the material's absorbency / antiseptic / temperature decide the
// outcome. Everything is integer, deterministic, and causal.

LawId TreatModel::id() const { return 21; }

bool TreatModel::supports(const World& /*w*/, const InteractionContext& ctx) const {
    return ctx.parameters.action == "treat" && ctx.targets.size() >= 2;
}

PredictionResult TreatModel::predict(const World& w,
                                     const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const EntityId patient_id = ctx.targets[0];
    const EntityId med_id = ctx.targets[1];
    const Entity* patient = w.find(patient_id);
    const Entity* med = w.find(med_id);
    if (!patient || !patient->alive || patient_id == med_id) {
        r.failure = "bad treat targets"; return r;
    }
    if (!med || !med->alive) { r.failure = "no dressing material"; return r; }

    static thread_local SeqPidCache pid_c_;
    SeqPid pid(pid_c_, w.props);
    const PropId role_p = pid("part_role"), sev_p = pid("severed"),
                 br_p = pid("bleed_rate"), inf_p = pid("infection_level"),
                 nec_p = pid("necrotic"), band_p = pid("bandaged_integrity"),
                 integ_p = pid("integrity"), vital_p = pid("vital_organ"),
                 absorb_p = pid("absorbency"), anti_p = pid("antiseptic"),
                 temp_p = pid("temperature"), pain_p = pid("pain_level"),
                 reach_p = pid("reach"), energy_p = pid("energy"),
                 contained_p = pid("contained");

    // reach: actor must reach BOTH patient and material
    const std::int64_t reach = reach_p != kNoProp
        ? w.prop_int(*actor, reach_p, lawutil::kDefaultReach)
        : lawutil::kDefaultReach;
    if (dist2(actor->pos, patient->pos) > reach * reach ||
        dist2(actor->pos, med->pos) > reach * reach) {
        r.failure = "out of reach"; return r;
    }

    // worst wounded attached part of the patient (deterministic: sorted
    // relations, strict > keeps the first/lowest-id on ties)
    EntityId worst = kNoEntity;
    std::int64_t worst_score = -1;
    for (const Relation& rel : w.relations_of(patient_id, "part_of")) {
        if (rel.b != patient_id) continue;
        const Entity* p = w.find(rel.a);
        if (!p || !p->alive) continue;
        if (sev_p != kNoProp) {
            const Value* sv = w.prop_of(*p, sev_p);
            if (sv && sv->type == ValType::Integer && sv->i != 0) continue;
        }
        if (nec_p != kNoProp) {
            const Value* nv = w.prop_of(*p, nec_p);
            if (nv && nv->type == ValType::Integer && nv->i != 0) continue;
        }
        if (role_p != kNoProp) {
            const Value* rv = w.prop_of(*p, role_p);
            if (!rv || rv->type != ValType::Text || rv->s.empty()) continue;
        }
        const std::int64_t br = br_p != kNoProp ? w.prop_int(*p, br_p, 0) : 0;
        const std::int64_t iv = inf_p != kNoProp ? w.prop_int(*p, inf_p, 0) : 0;
        const std::int64_t integ = integ_p != kNoProp ? w.prop_int(*p, integ_p, 1000) : 1000;
        const std::int64_t score = br * 3 + iv * 2 + std::max<std::int64_t>(0, 500 - integ);
        if (score > worst_score) { worst_score = score; worst = rel.a; }
    }
    if (worst == kNoEntity || worst_score <= 0) {
        r.failure = "patient has no treatable wound"; return r;
    }
    const Entity* part = w.find(worst);

    const std::int64_t absorb = absorb_p != kNoProp ? w.prop_int(*med, absorb_p, 0) : 0;
    const std::int64_t antiseptic = anti_p != kNoProp ? w.prop_int(*med, anti_p, 0) : 0;
    const std::int64_t temp = temp_p != kNoProp ? w.prop_int(*med, temp_p, 0) : 0;
    const std::int64_t mat_integ = integ_p != kNoProp ? w.prop_int(*med, integ_p, 500) : 500;

    const std::int64_t p_br = br_p != kNoProp ? w.prop_int(*part, br_p, 0) : 0;
    const std::int64_t p_inf = inf_p != kNoProp ? w.prop_int(*part, inf_p, 0) : 0;
    const std::int64_t p_integ = integ_p != kNoProp ? w.prop_int(*part, integ_p, 1000) : 1000;
    const std::int64_t p_band = band_p != kNoProp ? w.prop_int(*part, band_p, 0) : 0;

    bool acted = false;
    std::int64_t n_br = p_br, n_inf = p_inf, n_band = p_band, n_integ = p_integ;
    std::int64_t med_wear = 0;
    std::string what;

    // cauterization: hot object burns the wound shut — bleed & infection
    // zeroed, surface tissue damaged, max pain on the patient.
    if (temp >= 500 && (p_br > 0 || p_inf > 0)) {
        n_br = 0; n_inf = 0; n_integ = std::max<std::int64_t>(1, p_integ - 200);
        med_wear = 50;
        what = "cauterized";
        acted = true;
        if (pain_p != kNoProp) {
            const std::int64_t pp = pain_p != kNoProp ? w.prop_int(*patient, pain_p, 0) : 0;
            StateChange ch; ch.entity = patient_id; ch.prop = pain_p;
            ch.had_before = true; ch.before = Value::integer(pp);
            ch.after = Value::integer(std::min<std::int64_t>(1000, pp + 800));
            r.changes.push_back(ch);
        }
    }
    // antiseptic: potent materials drive infection down hard
    if (antiseptic >= 400 && n_inf > 0) {
        n_inf = std::max<std::int64_t>(0, n_inf - antiseptic * 3 / 2);
        med_wear = std::max<std::int64_t>(med_wear, 100);
        what = what.empty() ? "antiseptic" : what + "+antiseptic";
        acted = true;
    }
    // absorbency: dressing soaks blood and mechanically covers the wound
    if (absorb >= 400 && n_br > 0) {
        n_band = std::min<std::int64_t>(1000,
            (absorb * 3 / 4 + mat_integ / 4));
        n_br = n_br * (1000 - std::min<std::int64_t>(800, absorb * 4 / 5)) / 1000;
        med_wear = std::max<std::int64_t>(med_wear, 200);
        what = what.empty() ? "bandaged" : what + "+bandaged";
        acted = true;
    }
    // a bandage also shields existing infection from worsening while on
    if (n_band >= 300 && n_inf > 0) {
        n_inf = std::max<std::int64_t>(0, n_inf - 100);   // covered wound stabilizes
        acted = true;
    }
    if (!acted) { r.failure = "material has no medicinal effect here"; return r; }

    auto setp = [&](EntityId id, PropId p, std::int64_t b4, std::int64_t af) {
        if (p == kNoProp || b4 == af) return;
        StateChange ch; ch.entity = id; ch.prop = p; ch.had_before = true;
        ch.before = Value::integer(b4); ch.after = Value::integer(af);
        r.changes.push_back(ch);
    };
    setp(worst, br_p, p_br, n_br);
    setp(worst, inf_p, p_inf, n_inf);
    setp(worst, band_p, p_band, n_band);
    setp(worst, integ_p, p_integ, n_integ);
    if (med_wear > 0 && med_id != patient_id)
        setp(med_id, integ_p, mat_integ, std::max<std::int64_t>(1, mat_integ - med_wear));
    if (!lawutil::charge_energy(*actor, energy_p, 500, r)) {
        r.possible = false; r.changes.clear(); r.notes.clear();
        return r;
    }
    r.possible = true;
    r.ticks = ctx.parameters.ticks;
    r.notes.push_back("treat " + std::to_string(med_id) + " on part " +
                      std::to_string(worst) + " of " + std::to_string(patient_id) +
                      ": " + what);
    return r;
}

std::unique_ptr<INaturalLawModel> TreatModel::clone() const {
    return std::make_unique<TreatModel>();
}




}  // namespace simlab
