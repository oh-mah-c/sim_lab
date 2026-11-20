#include "simlab/Laws.hpp"
#include "simlab/World.hpp"
#include "simlab/Processes.hpp"
#include "LawUtil.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <sstream>

namespace simlab {

using namespace lawutil;

namespace {

struct PropIds {
    PropId exposed = kNoProp, min_force = kNoProp, dig_resistance = kNoProp,
           yield_rate = kNoProp, deposit_remaining = kNoProp,
           quantity = kNoProp, integrity = kNoProp, temperature = kNoProp;
};

PropIds resolve_props(const World& w) {
    PropIds p;
    static thread_local SeqPidCache get_c_;
    SeqPid get(get_c_, w.props);
    p.exposed = get("exposed"); p.min_force = get("min_force");
    p.dig_resistance = get("dig_resistance"); p.yield_rate = get("yield_rate");
    p.deposit_remaining = get("deposit_remaining"); p.quantity = get("quantity");
    p.integrity = get("integrity"); p.temperature = get("temperature");
    return p;
}

bool prop_bool(const World& w, const Entity& e, PropId p) {
    const Value* v = w.prop_of(e, p);
    return v && ((v->type == ValType::Boolean && v->b) ||
                 ((v->type == ValType::Integer) && v->i != 0));
}

std::int64_t entity_qty(const World& w, const Entity& e, PropId quantity) {
    if (quantity == kNoProp) return 1;
    const Value* v = w.prop_of(e, quantity);
    if (!v) return 1;                      // no quantity prop = one unit
    return v->i;
}

}  // namespace

// ============================ Extraction ======================================

LawId ExtractionModel::id() const { return 2; }

bool ExtractionModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return (ctx.parameters.action == "dig" || ctx.parameters.action == "chop" ||
            ctx.parameters.action == "strike") && !ctx.targets.empty();
}

PredictionResult ExtractionModel::predict(const World& w, const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropIds P = resolve_props(w);
    const auto [treach, tedge] = w.tool_bonus(ctx.actor);
    const std::int64_t E0 = ctx.parameters.force > 0 ? ctx.parameters.force : 1000;
    // Nhịp 4c: motor_precision scales extraction yield AND fracture force.
    const PropId motor_pid = pid(w, "mind_motor_precision");
    const std::int64_t motor0 = motor_pid != kNoProp
        ? w.prop_int(*actor, motor_pid, 1000) : 1000;
    const std::int64_t motor = motor0 * anatomy_motor(w, *actor) / 1000;
    if (!conscious_enough(w, *actor, r)) return r;
    const std::int64_t E = E0 * (1000 + tedge) / 1000 * motor / 1000;
    const PropId reach_pid = pid(w, "reach"), energy_pid = pid(w, "energy"),
                 contained_pid = pid(w, "contained");
    const EntityId striker = wielded_tool(w, ctx.actor);
    std::int64_t wear_total = 0;
    if (P.min_force == kNoProp || P.yield_rate == kNoProp ||
        P.deposit_remaining == kNoProp || P.integrity == kNoProp) {
        r.failure = "extraction requires min_force/yield_rate/deposit_remaining/integrity";
        return r;
    }
    const std::string& act = ctx.parameters.action;

    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (is_contained(w, *t, contained_pid)) continue;
        if (!in_reach(w, *actor, *t, reach_pid, treach, &r)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }

        // ---- deposit occurrence: yields its composition --------------------
        if (t->deposit != kNoDeposit) {
            const DepositDef* d = w.deposits.find(t->deposit);
            if (!d) continue;
            const bool exposed = P.exposed != kNoProp && prop_bool(w, *t, P.exposed);
            if (!exposed && act != "dig") {
                r.notes.push_back("deposit " + d->name + " buried: needs dig");
                continue;
            }
            const std::int64_t min_f = w.prop_int(*t, P.min_force, 1000);
            const std::int64_t dig_r = w.prop_int(*t, P.dig_resistance, 0);
            const std::int64_t req = exposed ? min_f : min_f * dig_r / 1000;
            add_cond(r, "force", E, req);
            if (E < req) {
                r.notes.push_back("deposit " + d->name + " needs force " +
                                  std::to_string(req) + " (have " + std::to_string(E) + ")");
                continue;
            }
            const std::int64_t remaining = w.prop_int(*t, P.deposit_remaining, 0);
            if (remaining <= 0) { r.notes.push_back("deposit depleted"); continue; }
            const std::int64_t yr = w.prop_int(*t, P.yield_rate, 10);
            std::int64_t yield_total = E * yr / 100000;
            if (yield_total < 1) yield_total = 1;
            yield_total = std::min(yield_total, remaining);

            // distribute yield across composition shares (permille, sum=1000);
            // rounding remainder goes to the first component (deterministic)
            std::vector<std::int64_t> qs(d->composition.size(), 0);
            std::int64_t assigned = 0;
            for (std::size_t ci = 0; ci < d->composition.size(); ++ci) {
                qs[ci] = yield_total * d->composition[ci].share / 1000;
                assigned += qs[ci];
            }
            if (!qs.empty()) qs[0] += yield_total - assigned;
            for (std::size_t ci = 0; ci < d->composition.size(); ++ci) {
                const std::int64_t q = qs[ci];
                if (q <= 0) continue;
                SpawnSpec s;
                s.kind = "raw_resource"; s.material = d->composition[ci].material; s.pos = t->pos;
                PropertyBag pb;
                if (P.quantity != kNoProp) pb.set(P.quantity, Value::integer(q));
                s.props = pb;
                r.spawns.push_back(s);
            }
            set_change(r, tid, P.deposit_remaining, remaining, remaining - yield_total);
            if (striker != kNoEntity)
                wear_total += reaction_wear(w, striker, 20, w.prop_int(*t, pid(w, "hardness"), 500));
            if (!exposed && P.exposed != kNoProp)
                set_change_v(r, tid, P.exposed, Value::boolean(false),
                             Value::boolean(true));
            r.possible = true;
            std::ostringstream n;
            n << "extract " << d->name << " yield=" << yield_total
              << " remaining=" << (remaining - yield_total)
              << (exposed ? "" : " (exposed by dig)");
            r.notes.push_back(n.str());
            continue;
        }

        // ---- plain material entity: generic fracture/separate ----------------
        if (t->material == kNoMaterial) continue;
        const std::int64_t integ = w.prop_int(*t, P.integrity, 500);
        if (integ <= 0) continue;
        const std::int64_t mult = act == "chop" ? 3 : 1;
        std::int64_t units = E * mult / 2000;
        if (units < 1) units = 1;
        const std::int64_t dmg = units * 5;
        const std::int64_t new_integ = std::max<std::int64_t>(0, integ - dmg);

        SpawnSpec s;
        s.kind = "fragment"; s.material = t->material; s.pos = t->pos;
        PropertyBag pb;
        if (P.quantity != kNoProp) pb.set(P.quantity, Value::integer(units));
        // geometry is state (§8.5): a chipped fragment inherits the parent's
        // primitive scaled down by the share it represents
        PropId gtt = kNoProp, gll = kNoProp, gww = kNoProp, gkk = kNoProp;
        if (const PropertyDef* d = w.props.find_name("geom_type")) gtt = d->id;
        if (const PropertyDef* d = w.props.find_name("geom_len")) gll = d->id;
        if (const PropertyDef* d = w.props.find_name("geom_wid")) gww = d->id;
        if (const PropertyDef* d = w.props.find_name("geom_thk")) gkk = d->id;
        const std::int64_t gtype = w.prop_int(*t, gtt, 0);
        if (gtt != kNoProp && gtype > 0) {
            const std::int64_t scale = std::max<std::int64_t>(100, 1000 / units);
            pb.set(gtt, Value::integer(gtype));
            if (gll != kNoProp) pb.set(gll, Value::integer(std::max<std::int64_t>(10, w.prop_int(*t, gll, 0) * scale / 1000)));
            if (gww != kNoProp) pb.set(gww, Value::integer(std::max<std::int64_t>(10, w.prop_int(*t, gww, 0) * scale / 1000)));
            if (gkk != kNoProp) pb.set(gkk, Value::integer(std::max<std::int64_t>(10, w.prop_int(*t, gkk, 0) * scale / 1000)));
        }
        pb.set(P.integrity, Value::integer(integ));   // fragment is as sound as its parent was
        s.props = pb;
        r.spawns.push_back(s);
        set_change(r, tid, P.integrity, integ, new_integ);
        if (striker != kNoEntity && striker != tid)
            wear_total += reaction_wear(w, striker, integ - new_integ,
                                        w.prop_int(*t, pid(w, "hardness"), 300));
        r.possible = true;
        std::ostringstream n;
        n << act << " material=" << t->material << " units=" << units
          << " integ " << integ << "->" << new_integ;
        r.notes.push_back(n.str());
    }
    if (r.possible && wear_total > 0)
        wear_total = wear_total *
            std::clamp<std::int64_t>(3000 - motor, 0, 3000) / 2000;
    if (r.possible && wear_total > 0) apply_tool_wear(w, striker, wear_total, r);
    if (r.possible && !charge_energy(*actor, energy_pid, E0, r)) {
        r.possible = false; r.changes.clear(); r.spawns.clear(); r.notes.clear();
        return r;
    }
    r.ticks = ctx.parameters.ticks;
    if (!r.possible && r.failure.empty()) {
        r.failure = "no extractable target";
        for (const std::string& n : r.notes) r.failure += "; " + n;   // why each target refused
    }
    return r;
}

std::unique_ptr<INaturalLawModel> ExtractionModel::clone() const {
    return std::make_unique<ExtractionModel>();
}

// ============================ Processing ======================================

LawId ProcessingModel::id() const { return 3; }

bool ProcessingModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "process" && !ctx.parameters.note.empty() &&
           !ctx.targets.empty();
}

PredictionResult ProcessingModel::predict(const World& w, const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }
    const ProcessDef* proc = w.processes.find(ctx.parameters.note);
    if (!proc) { r.failure = "unknown process '" + ctx.parameters.note + "'"; return r; }

    const PropIds P = resolve_props(w);
    const auto [ptreach, ptedge] = w.tool_bonus(ctx.actor);
    (void)ptedge;
    const PropId reach_pid = pid(w, "reach"), energy_pid = pid(w, "energy"),
                 contained_pid = pid(w, "contained");

    // gather live participants: actor + targets (all must be in reach —
    // you cannot smelt ore on the other side of the map)
    std::vector<const Entity*> parts;
    parts.push_back(actor);
    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t || !t->alive) continue;
        if (is_contained(w, *t, contained_pid)) continue;  // inside a structure
        if (!in_reach(w, *actor, *t, reach_pid, ptreach, &r)) {
            r.failure = "target " + std::to_string(tid) + " out of reach";
            return r;
        }
        parts.push_back(t);
    }

    // temperature gate: best participant temperature must reach the recipe's
    std::int64_t best_temp = 0;
    if (P.temperature != kNoProp)
        for (const Entity* e : parts)
            best_temp = std::max(best_temp, w.prop_int(*e, P.temperature, 0));
    if (proc->min_temperature > 0) add_cond(r, "temperature", best_temp, proc->min_temperature);
    if (proc->min_temperature > 0 && best_temp < proc->min_temperature) {
        r.failure = "temperature " + std::to_string(best_temp) +
                    " below required " + std::to_string(proc->min_temperature);
        return r;
    }

    // consume inputs + fuels: match material among targets, track per-entity use
    std::map<EntityId, std::int64_t> consumed;
    auto consume = [&](const std::vector<ProcessIO>& reqs, const char* kind) -> bool {
        for (const ProcessIO& io : reqs) {
            std::int64_t need = io.qty;
            for (EntityId tid : ctx.targets) {
                if (need <= 0) break;
                const Entity* t = w.find(tid);
                if (!t || !t->alive || t->material != io.material) continue;
                const std::int64_t avail = entity_qty(w, *t, P.quantity) - consumed[tid];
                const std::int64_t take = std::min(avail, need);
                if (take > 0) { consumed[tid] += take; need -= take; }
            }
            add_cond(r, (std::string(kind) + ":" + io.material_name).c_str(), io.qty - need, io.qty);
            if (need > 0) {
                r.failure = std::string("missing ") + kind + " " + io.material_name +
                            " (need " + std::to_string(io.qty) + ")";
                return false;
            }
        }
        return true;
    };
    if (!consume(proc->inputs, "input")) return r;
    if (!consume(proc->fuels, "fuel")) return r;

    // commit consumption as quantity/integrity changes
    for (const auto& [tid, used] : consumed) {
        const Entity* t = w.find(tid);
        if (!t) continue;
        const bool had_qty_prop = P.quantity != kNoProp && t->props.get(P.quantity);
        const std::int64_t eff = entity_qty(w, *t, P.quantity);
        const std::int64_t left = eff - used;
        if (had_qty_prop) {
            set_change(r, tid, P.quantity, eff, left);
            if (left <= 0)
                set_change(r, tid, P.integrity, w.prop_int(*t, P.integrity, 500), 0);
        } else {
            // whole-entity consumption (no quantity tracking): destroy it
            set_change(r, tid, P.integrity, w.prop_int(*t, P.integrity, 500), 0);
        }
    }

    // Process efficiency (TODO §9.5 "thất thoát"): a process run only just at
    // its minimum temperature recovers 80% of the product; recovery reaches
    // 100% once the charge is 20% of min_temperature above it. The unrecovered
    // part is NOT deleted — it joins the first waste stream (mass balance).
    std::int64_t efficiency = 1000;
    if (proc->min_temperature > 0 && !proc->wastes.empty()) {
        const std::int64_t margin = best_temp - proc->min_temperature;
        const std::int64_t span = std::max<std::int64_t>(1, proc->min_temperature / 5);
        efficiency = 800 + std::min<std::int64_t>(200, margin * 200 / span);
    }
    std::int64_t lost = 0;
    // outputs + waste spawn at the actor (the processor holds the product)
    auto spawn_io = [&](const std::vector<ProcessIO>& ios, const char* kind, bool is_output) {
        bool first_waste = true;
        for (const ProcessIO& io : ios) {
            std::int64_t q = io.qty;
            if (is_output) { const std::int64_t got = q * efficiency / 1000; lost += q - got; q = got; }
            else if (first_waste) { q += lost; first_waste = false; }
            if (q <= 0) continue;
            SpawnSpec s;
            s.kind = kind; s.material = io.material; s.pos = actor->pos;
            PropertyBag pb;
            if (P.quantity != kNoProp) pb.set(P.quantity, Value::integer(q));
            s.props = pb;
            r.spawns.push_back(s);
        }
    };
    spawn_io(proc->outputs, "processed_material", true);
    spawn_io(proc->wastes, "waste", false);

    std::int64_t in_qty = 0, out_qty = 0;
    for (const ProcessIO& io : proc->inputs) in_qty += io.qty;
    for (const ProcessIO& io : proc->outputs) out_qty += io.qty;
    for (const ProcessIO& io : proc->wastes) out_qty += io.qty;

    // energy cost: process duration drives it (1 J per 10 ticks, floor 1)
    const std::int64_t cost = std::max<std::int64_t>(1, proc->ticks / 10);
    if (!charge_energy(*actor, energy_pid, cost, r)) return r;

    r.possible = true;
    r.ticks = proc->ticks;
    std::ostringstream n;
    n << "process " << proc->name << " in=" << in_qty << " out=" << out_qty
      << " ticks=" << proc->ticks << " temp=" << best_temp
      << " efficiency=" << efficiency << " lost_to_waste=" << lost
      << " energy=" << cost;
    r.notes.push_back(n.str());
    return r;
}

std::unique_ptr<INaturalLawModel> ProcessingModel::clone() const {
    return std::make_unique<ProcessingModel>();
}

// ============================ Thermal =========================================

LawId ThermalModel::id() const { return 4; }

bool ThermalModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "heat" && !ctx.targets.empty();
}

// Heat transfer is property-driven: thermal_mass (J/K) resists warming,
// conductivity spreads it. delta deci-°C = E(milliJ) / (100 * thermal_mass).
// COMBUSTION: a flammable target at/above 300°C burns — every heating action
// while hot costs integrity in proportion to flammability (sustained burning,
// not a one-shot crossing). When burning destroys it, the material's declared
// `residue` (+ `residue_share` permille of its mass) remains as a new entity;
// the rest leaves as gas. No material is named anywhere in this code.
PredictionResult ThermalModel::predict(const World& w, const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const PropIds P = resolve_props(w);
    const auto [treach, tedge] = w.tool_bonus(ctx.actor);
    (void)tedge;   // edge sharpens blows, not conducted heat
    const std::int64_t E = ctx.parameters.force > 0 ? ctx.parameters.force : 1000;
    const PropId reach_pid = pid(w, "reach"), energy_pid = pid(w, "energy"),
                 flamm_pid = pid(w, "flammability"), contained_pid = pid(w, "contained"),
                 tmass_pid = pid(w, "thermal_mass"), mass_pid = pid(w, "mass"),
                 residue_pid = pid(w, "residue"), share_pid = pid(w, "residue_share");
    if (P.temperature == kNoProp || P.integrity == kNoProp) {
        r.failure = "thermal requires temperature/integrity properties";
        return r;
    }
    std::int64_t t_max = 10000;
    if (const PropertyDef* td = w.props.find(P.temperature))
        if (td->has_range) t_max = td->hi;
    constexpr std::int64_t kIgnition = 3000;   // 300°C in deci-°C

    for (EntityId tid : ctx.targets) {
        const Entity* t = w.find(tid);
        if (!t) continue;
        if (is_contained(w, *t, contained_pid)) continue;
        if (!in_reach(w, *actor, *t, reach_pid, treach, &r)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }
        const std::int64_t tmass = w.prop_int(*t, tmass_pid, 1000);
        if (tmass <= 0) continue;
        const std::int64_t cur = w.prop_int(*t, P.temperature, 200);   // ambient ~20°C
        const std::int64_t delta = E / (100 * tmass);
        const std::int64_t nw = std::min<std::int64_t>(t_max, cur + delta);
        set_change(r, tid, P.temperature, cur, nw);
        // Phase 12.6 energetic charge: a volatile mix that reaches its
        // ignition point (lower for more volatile mixes) releases its gas all
        // at once — a blast scaled by expansion_force and by how tightly the
        // casing (its own integrity) held the pressure. The charge is spent.
        {
            const PropId vol_p = SIMLAB_PID(w.props, "volatility");
            const PropId exf_p = SIMLAB_PID(w.props, "expansion_force");
            const std::int64_t vol = vol_p != kNoProp ? w.prop_int(*t, vol_p, 0) : 0;
            const std::int64_t exf = exf_p != kNoProp ? w.prop_int(*t, exf_p, 0) : 0;
            if (vol >= 300 && exf >= 300 && nw >= 4000 - vol * 3 && cur < 4000 - vol * 3) {
                const std::int64_t casing = w.prop_int(*t, P.integrity, 200);
                const std::int64_t mass = w.prop_int(*t, SIMLAB_PID(w.props, "mass"), 500);
                blast(w, r, t->pos, exf, casing, mass, {tid});   // the one who lit it is NOT spared
                set_change(r, tid, P.integrity, casing, 0);
                r.possible = true;
                continue;
            }
        }
        if (!t->alive) {
            // Phase 11 cremation: dead remains still conduct heat; past the
            // material's data-defined cremation_temp they burn down to the
            // declared residue (ash) and every carried pathogen dies with
            // them — strain_ref/contamination/odor zeroed, infection edges
            // cut, infection pathogen mass destroyed.
            const PropId cre_p = pid(w, "cremation_temp");
            const std::int64_t cre = cre_p != kNoProp
                ? w.prop_int(*t, cre_p, 0) : 0;
            r.possible = true;
            if (cre > 0 && nw >= cre) {
            auto zero_if = [&](const char* name) {
                const PropId zp = pid(w, name);
                if (zp == kNoProp) return;
                const std::int64_t zv = w.prop_int(*t, zp, 0);
                if (zv != 0) set_change(r, tid, zp, zv, 0);
            };
            zero_if("strain_ref"); zero_if("contamination");
            zero_if("odor"); zero_if("corpse_decay_level");
            const PropId cooked_p = pid(w, "cooked_state");
            if (cooked_p != kNoProp &&
                w.prop_int(*t, cooked_p, 0) != 1000)
                set_change(r, tid, cooked_p,
                           w.prop_int(*t, cooked_p, 0), 1000);
            const PropId crem_p = pid(w, "cremated");
            if (crem_p != kNoProp)
                set_change(r, tid, crem_p, w.prop_int(*t, crem_p, 0), 1);
            const PropId integ_p = pid(w, "integrity");
            const PropId pl_p = pid(w, "pathogen_load");
            for (const Relation& rel : w.relations_of(tid, "carries")) {
                if (rel.a != tid) continue;
                r.rel_changes.push_back({tid, "carries", rel.b, false});
                const Entity* ie = w.find(rel.b);
                if (!ie || !ie->alive) continue;
                if (pl_p != kNoProp) {
                    const std::int64_t lv = w.prop_int(*ie, pl_p, 0);
                    if (lv != 0) set_change(r, rel.b, pl_p, lv, 0);
                }
                if (integ_p != kNoProp)
                    set_change(r, rel.b, integ_p,
                               w.prop_int(*ie, integ_p, 0), 0);
            }
            // residue: the material's own declared burn product (ash)
            if (residue_pid != kNoProp && share_pid != kNoProp) {
                const Value* rv = w.prop_of(*t, residue_pid);
                const MaterialId rm = rv && rv->type == ValType::Text
                    ? w.materials.id_of(rv->s) : kNoMaterial;
                const std::int64_t share = w.prop_int(*t, share_pid, 0);
                const std::int64_t q = w.prop_int(*t, mass_pid, 0) * share / 1000;
                if (rm != kNoMaterial && q > 0) {
                    SpawnSpec rs;
                    rs.kind = "remains"; rs.material = rm; rs.pos = t->pos;
                    if (P.quantity != kNoProp)
                        rs.props.set(P.quantity, Value::integer(q));
                    r.spawns.push_back(rs);
                }
            }
            r.notes.push_back("cremated: residue spawned, pathogens cleared");
            continue;
            }
            // dead but not yet cremation-hot: sterilization still applies —
            // cooked meat is dead tissue that must reach cooked_state.
            {
                const PropId st_p = pid(w, "sterilization_temp");
                const std::int64_t st = st_p != kNoProp
                    ? w.prop_int(*t, st_p, 0) : 0;
                if (st > 0 && nw >= st) {
                    const PropId cooked_p = pid(w, "cooked_state");
                    if (cooked_p != kNoProp &&
                        w.prop_int(*t, cooked_p, 0) != 1000)
                        set_change(r, tid, cooked_p,
                                   w.prop_int(*t, cooked_p, 0), 1000);
                    const PropId sr_p = pid(w, "strain_ref");
                    if (sr_p != kNoProp && w.prop_int(*t, sr_p, 0) != 0)
                        set_change(r, tid, sr_p, w.prop_int(*t, sr_p, 0), 0);
                    const PropId cont_p = pid(w, "contamination");
                    if (cont_p != kNoProp && w.prop_int(*t, cont_p, 0) != 0)
                        set_change(r, tid, cont_p, w.prop_int(*t, cont_p, 0), 0);
                    // Phase 11.2: sterilized remains cannot keep seeding —
                    // carried infections are killed and any strain stamps
                    // on the husk are wiped.
                    const PropId pl_p = pid(w, "pathogen_load");
                    const PropId ip_p = pid(w, "integrity");
                    for (const Relation& rel : w.relations_of(tid, "carries")) {
                        if (rel.a != tid) continue;
                        r.rel_changes.push_back({tid, "carries", rel.b, false});
                        const Entity* ie = w.find(rel.b);
                        if (!ie || !ie->alive) continue;
                        if (pl_p != kNoProp) {
                            const std::int64_t lv = w.prop_int(*ie, pl_p, 0);
                            if (lv != 0) set_change(r, rel.b, pl_p, lv, 0);
                        }
                        if (ip_p != kNoProp)
                            set_change(r, rel.b, ip_p,
                                       w.prop_int(*ie, ip_p, 0), 0);
                    }
                    const PropId ssl_p = pid(w, "soil_spore_load");
                    const PropId ssr_p = pid(w, "soil_strain_ref");
                    const PropId sda_p = pid(w, "spore_decay_accum");
                    if (ssl_p != kNoProp && w.prop_int(*t, ssl_p, 0) != 0)
                        set_change(r, tid, ssl_p, w.prop_int(*t, ssl_p, 0), 0);
                    if (ssr_p != kNoProp && w.prop_int(*t, ssr_p, 0) != 0)
                        set_change(r, tid, ssr_p, w.prop_int(*t, ssr_p, 0), 0);
                    if (sda_p != kNoProp && w.prop_int(*t, sda_p, 0) != 0)
                        set_change(r, tid, sda_p, w.prop_int(*t, sda_p, 0), 0);
                    r.notes.push_back("sterilized dead tissue");
                }
            }
            r.notes.push_back("heat dead " + std::to_string(tid));
            continue;
        }

        // combustion: hot flammable material burns while it is heated
        const std::int64_t flamm = flamm_pid != kNoProp ? w.prop_int(*t, flamm_pid, 0) : 0;
        if (nw >= kIgnition && flamm > 0) {
            const std::int64_t integ = w.prop_int(*t, P.integrity, 500);
            const std::int64_t burn = std::max<std::int64_t>(1, flamm / 20);
            const std::int64_t left = std::max<std::int64_t>(0, integ - burn);
            set_change(r, tid, P.integrity, integ, left);
            r.notes.push_back(std::string(cur < kIgnition ? "ignited" : "burning") +
                              ": flammability=" + std::to_string(flamm));
            if (left == 0 && residue_pid != kNoProp && share_pid != kNoProp) {
                const Value* rv = w.prop_of(*t, residue_pid);
                const MaterialId rm = rv && rv->type == ValType::Text
                    ? w.materials.id_of(rv->s) : kNoMaterial;
                const std::int64_t share = w.prop_int(*t, share_pid, 0);
                const std::int64_t q = w.prop_int(*t, mass_pid, 0) * share / 1000;
                if (rm != kNoMaterial && q > 0) {
                    SpawnSpec s;
                    s.kind = "residue"; s.material = rm; s.pos = t->pos;
                    if (P.quantity != kNoProp) s.props.set(P.quantity, Value::integer(q));
                    r.spawns.push_back(s);
                    r.notes.push_back("burnt to residue qty=" + std::to_string(q));
                }
            }
        }
        // Phase 11 sterilization: a target at/above its material's
        // sterilization_temp is cooked/preserved — cooked_state 1000 makes
        // it safe to eat/drink and blocks every pathogen route.
        {
            const PropId st_p = pid(w, "sterilization_temp");
            const std::int64_t st = st_p != kNoProp
                ? w.prop_int(*t, st_p, 0) : 0;
            if (st > 0 && std::max(cur, nw) >= st) {
                const PropId cooked_p = pid(w, "cooked_state");
                if (cooked_p != kNoProp &&
                    w.prop_int(*t, cooked_p, 0) != 1000)
                    set_change(r, tid, cooked_p,
                               w.prop_int(*t, cooked_p, 0), 1000);
                const PropId sr_p = pid(w, "strain_ref");
                if (sr_p != kNoProp && w.prop_int(*t, sr_p, 0) != 0)
                    set_change(r, tid, sr_p, w.prop_int(*t, sr_p, 0), 0);
                const PropId cont_p = pid(w, "contamination");
                if (cont_p != kNoProp && w.prop_int(*t, cont_p, 0) != 0)
                    set_change(r, tid, cont_p, w.prop_int(*t, cont_p, 0), 0);
                r.notes.push_back("sterilized at temperature");
            }
        }
        // Phase 11.2 thermal cleansing: heat past sterilization_temp on a
        // spore host (blight_susceptibility > 0) or a soil patch
        // (soil_spore_load > 0) kills every carried infection and the
        // banked spore load; burnt plant biomass returns the material's
        // ash_nutrient_bonus fertility to the soil beneath — burn-barrows
        // emerge from the physics, never from a script.
        {
            const PropId susc_p = pid(w, "blight_susceptibility"),
                         pl_p = pid(w, "pathogen_load"),
                         ip_p = pid(w, "integrity"),
                         ssl_p = pid(w, "soil_spore_load"),
                         ssr_p = pid(w, "soil_strain_ref"),
                         sda_p = pid(w, "spore_decay_accum"),
                         st_p = pid(w, "sterilization_temp"),
                         anb_p = pid(w, "ash_nutrient_bonus"),
                         snu_p = pid(w, "soil_nutrient");
            const bool plant_host = susc_p != kNoProp &&
                w.prop_int(*t, susc_p, 0) > 0;
            const bool soil_host = ssl_p != kNoProp &&
                w.prop_of(*t, ssl_p) != nullptr;
            const std::int64_t st = st_p != kNoProp
                ? w.prop_int(*t, st_p, 0) : 0;
            if ((plant_host || soil_host) && st > 0 && nw >= st) {
                for (const Relation& rel : w.relations_of(tid, "carries")) {
                    if (rel.a != tid) continue;
                    r.rel_changes.push_back({tid, "carries", rel.b, false});
                    const Entity* ie = w.find(rel.b);
                    if (!ie || !ie->alive) continue;
                    if (pl_p != kNoProp) {
                        const std::int64_t lv = w.prop_int(*ie, pl_p, 0);
                        if (lv != 0) set_change(r, rel.b, pl_p, lv, 0);
                    }
                    if (ip_p != kNoProp)
                        set_change(r, rel.b, ip_p,
                                   w.prop_int(*ie, ip_p, 0), 0);
                }
                if (ssl_p != kNoProp && w.prop_int(*t, ssl_p, 0) != 0)
                    set_change(r, tid, ssl_p, w.prop_int(*t, ssl_p, 0), 0);
                if (ssr_p != kNoProp && w.prop_int(*t, ssr_p, 0) != 0)
                    set_change(r, tid, ssr_p, w.prop_int(*t, ssr_p, 0), 0);
                if (sda_p != kNoProp && w.prop_int(*t, sda_p, 0) != 0)
                    set_change(r, tid, sda_p, w.prop_int(*t, sda_p, 0), 0);
                r.notes.push_back("thermal cleansing: spores destroyed");
                // burnt plant biomass -> ash + soil fertility credit
                if (plant_host) {
                    const MaterialId ash_mat = w.materials.id_of("ash");
                    if (ash_mat != kNoMaterial) {
                        SpawnSpec as;
                        as.kind = "residue"; as.material = ash_mat;
                        as.pos = t->pos;
                        const std::int64_t mq = w.prop_int(*t, mass_pid, 0);
                        if (P.quantity != kNoProp && mq > 0)
                            as.props.set(P.quantity, Value::integer(
                                std::max<std::int64_t>(1, mq / 20)));
                        r.spawns.push_back(as);
                    }
                    // fertility credit is a property of the ASH material —
                    // the burn product decides how much the soil gains.
                    std::int64_t bonus = 0;
                    if (anb_p != kNoProp) {
                        if (const MaterialDef* amd = w.materials.find(ash_mat)) {
                            const auto it = amd->props.find(anb_p);
                            if (it != amd->props.end()) bonus = it->second.i;
                        }
                    }
                    if (bonus > 0 && snu_p != kNoProp) {
                        EntityId patch = kNoEntity;
                        {
                            std::vector<EntityId> near =
                                w.entities_near(t->pos, 48);
                            std::sort(near.begin(), near.end());
                            for (EntityId nid : near) {
                                const Entity* o = w.find(nid);
                                if (!o || !o->alive || nid == tid) continue;
                                if (ssl_p != kNoProp &&
                                    w.prop_of(*o, ssl_p) != nullptr) {
                                    patch = nid; break;
                                }
                            }
                        }
                        if (patch != kNoEntity) {
                            const Entity* po = w.find(patch);
                            const std::int64_t cur =
                                w.prop_int(*po, snu_p, 0);
                            set_change(r, patch, snu_p, cur,
                                std::min<std::int64_t>(2000, cur + bonus));
                        } else {
                            const MaterialId soil_mat =
                                w.materials.id_of("soil_loam");
                            if (soil_mat != kNoMaterial) {
                                SpawnSpec ps;
                                ps.kind = "soil_patch"; ps.material = soil_mat;
                                ps.pos = t->pos;
                                PropertyBag pb;
                                pb.set(snu_p, Value::integer(bonus));
                                if (ssl_p != kNoProp)
                                    pb.set(ssl_p, Value::integer(0));
                                if (ssr_p != kNoProp)
                                    pb.set(ssr_p, Value::integer(0));
                                if (sda_p != kNoProp)
                                    pb.set(sda_p, Value::integer(0));
                                const PropId cont_p = pid(w, "contamination");
                                if (cont_p != kNoProp)
                                    pb.set(cont_p, Value::integer(0));
                                ps.props = pb;
                                r.spawns.push_back(ps);
                            }
                        }
                        r.notes.push_back("ash fertilized soil");
                    }
                }
            }
        }
        r.possible = true;
        std::ostringstream n;
        n << "heat E=" << E << " tmass=" << tmass << " temp " << cur << "->" << nw;
        r.notes.push_back(n.str());
    }
    if (r.possible && !charge_energy(*actor, energy_pid, E, r)) {
        r.possible = false; r.changes.clear(); r.spawns.clear(); r.notes.clear();
        return r;
    }
    r.ticks = ctx.parameters.ticks;
    if (!r.possible && r.failure.empty()) r.failure = "no heatable target";
    return r;
}

std::unique_ptr<INaturalLawModel> ThermalModel::clone() const {
    return std::make_unique<ThermalModel>();
}

// ============================ Combine =========================================
// Phase 11 material combination: antimicrobial >= 500 sterilizes siblings,
// burial_medium >= 500 buries dead siblings. Data decides everything.

LawId CombineModel::id() const { return 22; }

bool CombineModel::supports(const World& w, const InteractionContext& ctx) const {
    (void)w;
    return ctx.parameters.action == "combine" && ctx.targets.size() >= 2;
}

PredictionResult CombineModel::predict(const World& w,
                                       const InteractionContext& ctx) const {
    PredictionResult r;
    r.law = id(); r.law_name = name();
    const Entity* actor = w.find(ctx.actor);
    if (!actor || !actor->alive) { r.failure = "no living actor"; return r; }

    const auto [treach, tedge] = w.tool_bonus(ctx.actor);
    (void)tedge;
    const PropId reach_pid = pid(w, "reach"),
                 contained_pid = pid(w, "contained"),
                 am_p = pid(w, "antimicrobial"),
                 bm_p = pid(w, "burial_medium"),
                 cooked_p = pid(w, "cooked_state"),
                 sr_p = pid(w, "strain_ref"),
                 cont_p = pid(w, "contamination"),
                 buried_p = pid(w, "buried"),
                 odor_p = pid(w, "odor"),
                 nut_p = pid(w, "nutrition"),
                 hyd_p = pid(w, "hydration");
    if (am_p == kNoProp && bm_p == kNoProp) {
        r.failure = "no antimicrobial/burial_medium properties defined";
        return r;
    }
    struct Cand {
        EntityId id; const Entity* e; bool am, bm;
    };
    std::vector<Cand> cs;
    for (EntityId tid : ctx.targets) {
        if (tid == ctx.actor) continue;
        const Entity* t = w.find(tid);
        if (!t) continue;
        if (is_contained(w, *t, contained_pid)) continue;
        if (!in_reach(w, *actor, *t, reach_pid, treach, &r)) {
            r.notes.push_back("target " + std::to_string(tid) + " out of reach");
            continue;
        }
        cs.push_back({tid, t,
                      am_p != kNoProp && w.prop_int(*t, am_p, 0) >= 500,
                      bm_p != kNoProp && w.prop_int(*t, bm_p, 0) >= 500});
    }
    std::set<EntityId> sterilized, buried_done;
    auto sterilize = [&](EntityId tid) {
        if (!sterilized.insert(tid).second) return;
        const Entity* t = w.find(tid);
        if (!t) return;
        if (cooked_p != kNoProp) {
            const std::int64_t c = w.prop_int(*t, cooked_p, 0);
            if (c != 1000) set_change(r, tid, cooked_p, c, 1000);
        }
        if (sr_p != kNoProp) {
            const std::int64_t v = w.prop_int(*t, sr_p, 0);
            if (v != 0) set_change(r, tid, sr_p, v, 0);
        }
        if (cont_p != kNoProp) {
            const std::int64_t v = w.prop_int(*t, cont_p, 0);
            if (v != 0) set_change(r, tid, cont_p, v, 0);
        }
        r.notes.push_back("sterilized " + std::to_string(tid));
    };
    auto bury = [&](EntityId tid) {
        if (!buried_done.insert(tid).second) return;
        const Entity* t = w.find(tid);
        if (!t || t->alive) return;
        if (buried_p != kNoProp) {
            const std::int64_t b = w.prop_int(*t, buried_p, 0);
            if (b != 1) set_change(r, tid, buried_p, b, 1);
        }
        if (odor_p != kNoProp) {
            const std::int64_t o = w.prop_int(*t, odor_p, 0);
            if (o != 0) set_change(r, tid, odor_p, o, 0);
        }
        r.notes.push_back("buried " + std::to_string(tid));
    };
    for (const Cand& c : cs) {
        if (!c.am && !c.bm) continue;             // not a reagent
        for (const Cand& s : cs) {
            if (s.id == c.id) continue;
            if (c.am) {
                const Entity* t = w.find(s.id);
                if (!t) continue;
                // substrate must be preservable matter: remains, food,
                // drink, strain-bearing or already-treated entities.
                const bool eligible = !t->alive ||
                    w.prop_int(*t, nut_p, 0) > 0 ||
                    w.prop_int(*t, hyd_p, 0) > 0 ||
                    (sr_p != kNoProp && w.prop_int(*t, sr_p, 0) > 0) ||
                    (cont_p != kNoProp && w.prop_int(*t, cont_p, 0) > 0) ||
                    (cooked_p != kNoProp && w.prop_of(*t, cooked_p));
                if (eligible) sterilize(s.id);
            }
            if (c.bm) bury(s.id);                  // bury() gates on dead
        }
    }
    r.possible = !sterilized.empty() || !buried_done.empty();
    if (!r.possible && r.failure.empty())
        r.failure = "no antimicrobial/burial_medium effect applies";
    r.ticks = ctx.parameters.ticks;
    return r;
}

std::unique_ptr<INaturalLawModel> CombineModel::clone() const {
    return std::make_unique<CombineModel>();
}

}  // namespace simlab
